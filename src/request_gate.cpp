//
//  request_gate.cpp
//  Kufar Telegram Notifier
//

#include <algorithm>
#include <cmath>
#include <utility>

#include "request_gate.hpp"

namespace Networking {
    using namespace std::chrono_literals;

    namespace {
        constexpr auto STOP_CHECK_SLICE = 250ms;
        constexpr std::chrono::seconds MAX_COOLDOWN{300};
    }

    RequestGate::RequestGate(
        RequestGateConfig configuration,
        GateStopFunction shouldStop,
        GateNowFunction now
    ) : config_(configuration),
        shouldStop_(std::move(shouldStop)),
        now_(std::move(now)) {
        if (!now_) {
            now_ = [] { return GateClock::now(); };
        }

        if (!std::isfinite(config_.requestsPerSecond) || config_.requestsPerSecond <= 0.0) {
            config_.requestsPerSecond = 4.0;
        }
        if (config_.burst <= 0) {
            config_.burst = 4;
        }
        if (config_.cooldown < 1s) {
            config_.cooldown = 30s;
        }

        tokens_ = static_cast<double>(config_.burst);
        lastRefill_ = currentTime();
        pausedUntil_ = lastRefill_;
    }

    GateClock::time_point RequestGate::currentTime() const {
        return now_ ? now_() : GateClock::now();
    }

    void RequestGate::refillLocked(GateClock::time_point now) {
        if (now < lastRefill_) {
            lastRefill_ = now;
            return;
        }

        const double elapsed = std::chrono::duration<double>(now - lastRefill_).count();
        tokens_ = std::min(
            static_cast<double>(config_.burst),
            tokens_ + elapsed * config_.requestsPerSecond
        );
        lastRefill_ = now;
    }

    bool RequestGate::tryAcquireAt(GateClock::time_point now) {
        std::lock_guard<std::mutex> lock(mtx_);
        refillLocked(now);

        if (now < pausedUntil_ || tokens_ < 1.0) {
            return false;
        }

        tokens_ -= 1.0;
        return true;
    }

    bool RequestGate::acquire() {
        std::unique_lock<std::mutex> lock(mtx_);

        while (true) {
            if (shouldStop_ && shouldStop_()) {
                return false;
            }

            const GateClock::time_point now = currentTime();
            refillLocked(now);

            if (now >= pausedUntil_ && tokens_ >= 1.0) {
                tokens_ -= 1.0;
                return true;
            }

            GateClock::time_point wakeAt = now + STOP_CHECK_SLICE;

            if (pausedUntil_ > now && pausedUntil_ < wakeAt) {
                wakeAt = pausedUntil_;
            }

            if (now >= pausedUntil_ && tokens_ < 1.0) {
                const double seconds = (1.0 - tokens_) / config_.requestsPerSecond;
                const GateClock::time_point tokenAt = now +
                    std::chrono::duration_cast<GateClock::duration>(
                        std::chrono::duration<double>(seconds)
                    );
                if (tokenAt < wakeAt) {
                    wakeAt = tokenAt;
                }
            }

            if (wakeAt <= now) {
                wakeAt = now + 1ms;
            }

            cv_.wait_until(lock, wakeAt);
        }
    }

    void RequestGate::reportSuccess() {
        std::lock_guard<std::mutex> lock(mtx_);
        consecutiveFailures_ = 0;
    }

    void RequestGate::reportFailure(
        int statusCode,
        std::optional<std::chrono::seconds> retryAfter
    ) {
        const bool retryable = statusCode == 0 || statusCode == 429 || statusCode >= 500;

        {
            std::lock_guard<std::mutex> lock(mtx_);

            if (!retryable) {
                consecutiveFailures_ = 0;
                return;
            }

            ++consecutiveFailures_;

            std::chrono::seconds cooldown = config_.cooldown;

            if (statusCode == 429) {
                if (retryAfter.has_value()) {
                    cooldown = retryAfter.value();
                }
            } else {
                const int shift = std::min(consecutiveFailures_ - 1, 4);
                cooldown = config_.cooldown * (1 << shift);
                if (retryAfter.has_value() && retryAfter.value() > cooldown) {
                    cooldown = retryAfter.value();
                }
            }

            cooldown = std::min(cooldown, MAX_COOLDOWN);
            pauseLocked(cooldown);
        }

        cv_.notify_all();
    }

    void RequestGate::pauseFor(std::chrono::seconds duration) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            pauseLocked(duration);
        }
        cv_.notify_all();
    }

    void RequestGate::pauseLocked(std::chrono::seconds duration) {
        if (duration <= std::chrono::seconds(0)) {
            return;
        }

        const GateClock::time_point target = currentTime() + duration;
        if (target > pausedUntil_) {
            pausedUntil_ = target;
        }
    }

    RequestGateStatus RequestGate::status() const {
        std::lock_guard<std::mutex> lock(mtx_);

        const GateClock::time_point now = currentTime();

        RequestGateStatus result;
        result.tokens = tokens_;
        result.requestsPerSecond = config_.requestsPerSecond;
        result.burst = config_.burst;
        result.consecutiveFailures = consecutiveFailures_;
        result.pausedFor = pausedUntil_ > now
            ? std::chrono::duration_cast<std::chrono::milliseconds>(pausedUntil_ - now)
            : std::chrono::milliseconds(0);
        return result;
    }
}
