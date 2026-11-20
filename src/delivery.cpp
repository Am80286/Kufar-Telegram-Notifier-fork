//
//  delivery.cpp
//  Kufar Telegram Notifier
//

#include <algorithm>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "delivery.hpp"

namespace delivery {
    SendException::SendException(
        SendFailureKind kind,
        const std::string &message,
        std::chrono::seconds retryAfter
    ) : std::runtime_error(message),
        kind_(kind),
        retryAfter_(retryAfter) {
    }

    SendFailureKind SendException::kind() const {
        return kind_;
    }

    std::chrono::seconds SendException::retryAfter() const {
        return retryAfter_;
    }

    DeliveryWorker::DeliveryWorker(
        SendFunction send,
        cache::CacheStore &cache,
        QueryExistsFunction queryExists,
        DeliveryOptions options
    ) : send_(std::move(send)),
        cache_(cache),
        queryExists_(std::move(queryExists)),
        options_(options),
        nextSendAllowed_(Clock::now()) {
        if (!send_) {
            throw std::invalid_argument("DeliveryWorker requires a send function.");
        }
        if (!queryExists_) {
            queryExists_ = [](queries::QueryID) { return true; };
        }
        if (options_.queueCapacity == 0) {
            options_.queueCapacity = 1;
        }
        if (options_.maxAttempts <= 0) {
            options_.maxAttempts = 1;
        }
        if (options_.messagesPerMinute < 0) {
            options_.messagesPerMinute = 0;
        }

        sendSpacing_ = options_.messagesPerMinute > 0
            ? std::chrono::milliseconds(60000 / options_.messagesPerMinute)
            : std::chrono::milliseconds(0);

        worker_ = std::thread([this] { run(); });
    }

    DeliveryWorker::~DeliveryWorker() {
        stopAndDrain(std::chrono::milliseconds(0));
    }

    bool DeliveryWorker::enqueue(DeliveryItem item) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] {
            return stopping_ || queue_.size() < options_.queueCapacity;
        });

        if (stopping_) {
            return false;
        }

        queue_.push_back(std::move(item));
        cv_.notify_all();
        return true;
    }

    void DeliveryWorker::stopAndDrain(std::chrono::milliseconds grace) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!stopping_) {
                stopping_ = true;
                drainDeadline_ = Clock::now() + grace;
            }
        }
        cv_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void DeliveryWorker::run() {
        while (true) {
            DeliveryItem item;
            std::vector<DeliveryItem> discarded;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this] {
                    return stopping_ || !queue_.empty();
                });

                if (queue_.empty()) {
                    if (stopping_) {
                        break;
                    }
                    continue;
                }

                if (stopping_ && Clock::now() >= drainDeadline_) {
                    discarded.assign(
                        std::make_move_iterator(queue_.begin()),
                        std::make_move_iterator(queue_.end())
                    );
                    queue_.clear();
                } else {
                    item = std::move(queue_.front());
                    queue_.pop_front();
                    cv_.notify_all();
                }
            }

            if (!discarded.empty()) {
                for (const DeliveryItem &entry : discarded) {
                    cache_.releaseClaim(entry.cacheKey, entry.ad.id);
                }
                spdlog::warn(
                    "[Delivery] Dropped {} queued listing(s) during shutdown.",
                    discarded.size()
                );
                break;
            }

            process(std::move(item));
        }
    }

    void DeliveryWorker::process(DeliveryItem item) {
        if (!queryExists_(item.queryID)) {
            cache_.releaseClaim(item.cacheKey, item.ad.id);
            spdlog::debug(
                "[Delivery] Query {} was removed; dropping listing {}.",
                item.queryID,
                item.ad.id
            );
            return;
        }

        int contentAttempts = 0;
        int rateLimitWaits = 0;

        while (true) {
            if (!waitForSendSlot()) {
                release(item, "stopping");
                return;
            }

            try {
                send_(item);

                cache_.markDelivered(item.cacheKey, item.ad.id);
                cache_.saveIfDue(Clock::now());

                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    nextSendAllowed_ = Clock::now() + sendSpacing_;
                }

                spdlog::debug(
                    "[Delivery] Sent listing {} to topic {}.",
                    item.ad.id,
                    item.cacheKey
                );
                return;
            } catch (const SendException &exc) {
                switch (exc.kind()) {
                    case SendFailureKind::RateLimited: {
                        ++rateLimitWaits;
                        if (rateLimitWaits > options_.maxRateLimitWaits) {
                            release(item, "Telegram rate limit retries exhausted");
                            return;
                        }

                        const std::chrono::seconds retryAfter = exc.retryAfter();
                        {
                            std::lock_guard<std::mutex> lock(mtx_);
                            const Clock::time_point retryAt = Clock::now() + retryAfter;
                            if (retryAt > nextSendAllowed_) {
                                nextSendAllowed_ = retryAt;
                            }
                        }

                        spdlog::warn(
                            "[Delivery] Telegram rate limit for listing {}; waiting {}s.",
                            item.ad.id,
                            retryAfter.count()
                        );

                        if (!waitFor(retryAfter)) {
                            release(item, "stopping");
                            return;
                        }
                        break;
                    }
                    case SendFailureKind::Permanent: {
                        release(item, exc.what());
                        return;
                    }
                    case SendFailureKind::Transient: {
                        ++contentAttempts;
                        if (contentAttempts >= options_.maxAttempts) {
                            release(item, exc.what());
                            return;
                        }

                        const auto backoff = options_.retryBackoff * contentAttempts;
                        spdlog::warn(
                            "[Delivery] Listing {} failed (attempt {}/{}): {}. Retrying in {}ms.",
                            item.ad.id,
                            contentAttempts,
                            options_.maxAttempts,
                            exc.what(),
                            backoff.count()
                        );

                        if (!waitFor(backoff)) {
                            release(item, "stopping");
                            return;
                        }
                        break;
                    }
                }
            } catch (const std::exception &exc) {
                ++contentAttempts;
                if (contentAttempts >= options_.maxAttempts) {
                    release(item, exc.what());
                    return;
                }

                const auto backoff = options_.retryBackoff * contentAttempts;
                spdlog::warn(
                    "[Delivery] Listing {} failed (attempt {}/{}): {}. Retrying in {}ms.",
                    item.ad.id,
                    contentAttempts,
                    options_.maxAttempts,
                    exc.what(),
                    backoff.count()
                );

                if (!waitFor(backoff)) {
                    release(item, "stopping");
                    return;
                }
            }
        }
    }

    bool DeliveryWorker::waitFor(std::chrono::milliseconds duration) {
        if (duration <= std::chrono::milliseconds(0)) {
            return true;
        }

        std::unique_lock<std::mutex> lock(mtx_);
        const bool stopped = cv_.wait_for(lock, duration, [this] {
            return stopping_;
        });
        return !stopped;
    }

    bool DeliveryWorker::waitForSendSlot() {
        while (true) {
            Clock::time_point due;
            Clock::time_point deadline;
            bool stopping = false;

            {
                std::lock_guard<std::mutex> lock(mtx_);
                const Clock::time_point current = Clock::now();
                stopping = stopping_;
                deadline = drainDeadline_;

                if (stopping && current >= deadline) {
                    return false;
                }

                due = nextSendAllowed_;
            }

            const Clock::time_point current = Clock::now();
            if (current >= due) {
                return true;
            }

            Clock::time_point target = due;
            if (stopping && deadline < target) {
                target = deadline;
            }
            if (target <= current) {
                return !stopping || current < deadline;
            }

            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_until(lock, target);
        }
    }

    void DeliveryWorker::release(const DeliveryItem &item, const std::string &reason) {
        cache_.releaseClaim(item.cacheKey, item.ad.id);
        spdlog::error(
            "[Delivery] Dropped listing {} for topic {}: {}",
            item.ad.id,
            item.cacheKey,
            reason
        );
    }
}
