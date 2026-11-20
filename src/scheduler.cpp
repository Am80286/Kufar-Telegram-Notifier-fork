//
//  scheduler.cpp
//  Kufar Telegram Notifier
//

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

#include <spdlog/spdlog.h>

#include "scheduler.hpp"
#include "helperfunctions.hpp"

namespace scheduling {
    using namespace std;
    using namespace std::chrono_literals;

    namespace {
        constexpr auto MAX_WAIT_SLICE = 250ms;
    }

    PollingScheduler::PollingScheduler(
        queries::QueryStore &queryStore,
        cache::CacheStore &cache,
        delivery::DeliverySink &delivery,
        SchedulerConfiguration configuration,
        NewestFetcher newestFetcher,
        WindowFetcher windowFetcher,
        NowFunction now,
        StopFunction stop
    ) : queryStore_(queryStore),
        cache_(cache),
        delivery_(delivery),
        config_(configuration),
        newestFetcher_(std::move(newestFetcher)),
        windowFetcher_(std::move(windowFetcher)),
        now_(std::move(now)),
        stop_(std::move(stop)),
        rng_(std::random_device{}()) {
        if (config_.defaultInterval.count() <= 0) {
            config_.defaultInterval = 60s;
        }
        if (config_.minInterval.count() <= 0) {
            config_.minInterval = config_.defaultInterval;
        }
        if (config_.maxInterval < config_.minInterval) {
            config_.maxInterval = config_.minInterval;
        }
        if (config_.jitterPercent < 0) {
            config_.jitterPercent = 0;
        }
        if (config_.jitterPercent > 100) {
            config_.jitterPercent = 100;
        }
        if (config_.coldStartListingLimit <= 0) {
            config_.coldStartListingLimit = 10;
        }
        if (config_.maxPages <= 0) {
            config_.maxPages = 1;
        }
        if (config_.frontierOverlapSeconds < 0) {
            config_.frontierOverlapSeconds = 0;
        }

        if (!newestFetcher_) {
            newestFetcher_ = [](const Kufar::KufarConfiguration &configuration, int limit) {
                return Kufar::getSearchPage(configuration, nullopt, limit).ads;
            };
        }
        if (!windowFetcher_) {
            windowFetcher_ = [](const Kufar::KufarConfiguration &configuration, const Kufar::FetchOptions &options) {
                return Kufar::getAllAds(configuration, options);
            };
        }
        if (!now_) {
            now_ = [] { return Clock::now(); };
        }
    }

    bool PollingScheduler::stopped() const {
        return stop_ && stop_();
    }

    void PollingScheduler::run() {
        while (!stopped()) {
            const Clock::time_point now = now_();

            try {
                reconcile(now);
            } catch (const exception &exc) {
                spdlog::error("[Poll] Failed to refresh query list: {}", exc.what());
                this_thread::sleep_for(1s);
                continue;
            }

            optional<chrono::milliseconds> wait;
            try {
                wait = stepOnce(now);
            } catch (const exception &exc) {
                spdlog::error("[Poll] Scheduler step failed: {}", exc.what());
                this_thread::sleep_for(1s);
                continue;
            }

            const auto delay = min(wait.value_or(1000ms), MAX_WAIT_SLICE);
            if (delay > 0ms) {
                this_thread::sleep_for(delay);
            }
        }
    }

    void PollingScheduler::reconcile(Clock::time_point now) {
        const uint64_t version = queryStore_.version();
        if (version == queryVersion_) {
            return;
        }

        queries_ = queryStore_.getQueries();
        queryVersion_ = version;

        unordered_set<queries::QueryID> activeIDs;
        activeIDs.reserve(queries_.size());
        for (const queries::QueryConfiguration &query : queries_) {
            activeIDs.insert(query.id);
        }

        for (auto iterator = schedules_.begin(); iterator != schedules_.end();) {
            if (!activeIDs.contains(iterator->first)) {
                iterator = schedules_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        for (const queries::QueryConfiguration &query : queries_) {
            if (!schedules_.contains(query.id)) {
                initialize(query.id, query, now);
            }
        }
    }

    void PollingScheduler::initialize(
        queries::QueryID id,
        const queries::QueryConfiguration &query,
        Clock::time_point now
    ) {
        PollSchedule schedule;
        schedule.currentInterval = minIntervalFor(query);

        if (const optional<cache::FrontierState> persisted = cache_.getFrontier(id); persisted.has_value()) {
            schedule.frontier = persisted->frontier;
            schedule.suppressBefore = persisted->suppressBefore > 0
                ? persisted->suppressBefore
                : persisted->frontier;
            schedule.overlapEnabled = true;
            schedule.initialized = schedule.frontier > 0;
        }

        // Stagger startup slightly so a large query set does not fire in one
        // synchronized burst. Jitter-free configurations start immediately,
        // which also keeps scheduling deterministic in tests.
        const auto stagger = config_.jitterPercent > 0
            ? chrono::milliseconds(rng_() % 2000)
            : chrono::milliseconds(0);
        schedule.nextDue = now + stagger;
        schedules_[id] = schedule;
    }

    optional<chrono::milliseconds> PollingScheduler::stepOnce(Clock::time_point now) {
        if (schedules_.empty()) {
            return 1000ms;
        }

        auto earliest = schedules_.begin();
        for (auto iterator = next(schedules_.begin()); iterator != schedules_.end(); ++iterator) {
            if (iterator->second.nextDue < earliest->second.nextDue) {
                earliest = iterator;
            }
        }

        if (earliest->second.nextDue > now) {
            return chrono::duration_cast<chrono::milliseconds>(earliest->second.nextDue - now);
        }

        const queries::QueryID id = earliest->first;
        const auto queryIterator = find_if(
            queries_.begin(),
            queries_.end(),
            [id](const queries::QueryConfiguration &query) { return query.id == id; }
        );

        if (queryIterator == queries_.end()) {
            schedules_.erase(id);
            return 0ms;
        }

        poll(id, earliest->second, *queryIterator, now);

        // Another query may already be due; let the caller step again.
        return 0ms;
    }

    optional<PollSchedule> PollingScheduler::scheduleFor(queries::QueryID id) const {
        const auto iterator = schedules_.find(id);
        if (iterator == schedules_.end()) {
            return nullopt;
        }
        return iterator->second;
    }

    chrono::seconds PollingScheduler::minIntervalFor(const queries::QueryConfiguration &query) const {
        const long long requested = query.pollIntervalSeconds.has_value()
            ? query.pollIntervalSeconds.value()
            : config_.defaultInterval.count();
        const long long floor = config_.minInterval.count();
        return chrono::seconds(max<long long>(1, max(requested, floor)));
    }

    chrono::seconds PollingScheduler::maxIntervalFor(const queries::QueryConfiguration &query) const {
        return max(config_.maxInterval, minIntervalFor(query));
    }

    chrono::milliseconds PollingScheduler::jitter(chrono::seconds base) {
        const long long minimumMilliseconds = 1000;
        const long long baseMilliseconds = max<long long>(minimumMilliseconds, base.count() * 1000);

        if (config_.jitterPercent <= 0) {
            return chrono::milliseconds(baseMilliseconds);
        }

        uniform_int_distribution<int> distribution(0, config_.jitterPercent * 2);
        const double offset = static_cast<double>(distribution(rng_) - config_.jitterPercent) / 100.0;
        const double factor = 1.0 + offset;

        const auto milliseconds = static_cast<long long>(baseMilliseconds * factor);
        return chrono::milliseconds(max(minimumMilliseconds, milliseconds));
    }

    void PollingScheduler::poll(
        queries::QueryID id,
        PollSchedule &schedule,
        const queries::QueryConfiguration &query,
        Clock::time_point now
    ) {
        const bool chronological = !query.kufar.sortType.has_value();
        const bool coldStart = !schedule.initialized;
        const string topic = query.cacheKey();

        vector<Kufar::Ad> fetched;
        time_t windowStart = 0;
        bool fetchedOK = false;
        bool stoppedDuringPoll = false;
        bool enqueueFailed = false;
        bool truncated = false;
        size_t enqueued = 0;

        try {
            if (!chronological || coldStart) {
                fetched = newestFetcher_(query.kufar, config_.coldStartListingLimit);
                fetchedOK = true;
            } else {
                const time_t overlap = schedule.overlapEnabled
                    ? config_.frontierOverlapSeconds
                    : 0;
                windowStart = max(schedule.frontier - overlap, schedule.suppressBefore);

                Kufar::FetchOptions options;
                options.pageSize = Kufar::DEFAULT_PAGE_SIZE;
                options.maxPages = config_.maxPages;
                options.newerThan = windowStart;
                options.shouldStop = stop_;

                Kufar::FetchResult result = windowFetcher_(query.kufar, options);
                fetched = std::move(result.ads);
                truncated = result.truncated;
                fetchedOK = true;
            }
        } catch (const exception &exc) {
            spdlog::error("[Poll] Query {} fetch failed: {}", id, exc.what());
        }

        if (fetchedOK) {
            for (const Kufar::Ad &advert : fetched) {
                if (stopped()) {
                    stoppedDuringPoll = true;
                    break;
                }

                if (chronological && !coldStart && advert.date < windowStart) {
                    continue;
                }

                if (!cache_.tryClaim(topic, advert.id)) {
                    continue;
                }

                delivery::DeliveryItem item;
                item.queryID = id;
                item.cacheKey = topic;
                item.messageThreadID = query.messageThreadID;
                item.ad = advert;

                if (!delivery_.enqueue(std::move(item))) {
                    cache_.releaseClaim(topic, advert.id);
                    enqueueFailed = true;
                    break;
                }

                enqueued++;
                spdlog::info(
                    "[New]: Queued [Title: {}], [ID: {}], [Tag: {}], [Topic: {}], [Link: {}]",
                    advert.title,
                    advert.id,
                    advert.tag.value_or(PROPERTY_UNDEFINED),
                    topic,
                    advert.link
                );
            }

            if (truncated && !stopped()) {
                spdlog::warn(
                    "[Poll] Query {} reached the page limit ({} pages); "
                    "older listings in this window were skipped.",
                    id,
                    config_.maxPages
                );
            }
        }

        const bool canAdvance = fetchedOK && !stoppedDuringPoll && !enqueueFailed;

        if (canAdvance) {
            if (chronological) {
                time_t newest = coldStart ? 0 : schedule.frontier;
                for (const Kufar::Ad &advert : fetched) {
                    newest = max(newest, advert.date);
                }

                if (newest > 0) {
                    const bool frontierChanged = coldStart
                        ? (schedule.frontier != newest || schedule.suppressBefore != newest)
                        : (schedule.frontier != newest);

                    schedule.frontier = newest;

                    if (coldStart) {
                        schedule.suppressBefore = newest;
                        schedule.overlapEnabled = false;
                    } else {
                        schedule.overlapEnabled = true;
                    }

                    schedule.initialized = true;

                    if (frontierChanged) {
                        cache_.setFrontier(id, cache::FrontierState{
                            schedule.frontier,
                            schedule.suppressBefore
                        });
                        cache_.saveIfDue(now);
                    }
                }
            } else {
                schedule.initialized = true;
            }
        }

        if (enqueued > 0) {
            schedule.currentInterval = minIntervalFor(query);
        } else {
            const auto grown = static_cast<long long>(ceil(schedule.currentInterval.count() * 1.5));
            schedule.currentInterval = min(
                maxIntervalFor(query),
                chrono::seconds(max<long long>(schedule.currentInterval.count() + 1, grown))
            );
        }

        schedule.nextDue = now + jitter(schedule.currentInterval);

        spdlog::debug(
            "[Poll] Query {}: fetched={}, enqueued={}, frontier={}, interval={}s, next in {}ms",
            id,
            fetched.size(),
            enqueued,
            schedule.frontier,
            schedule.currentInterval.count(),
            chrono::duration_cast<chrono::milliseconds>(schedule.nextDue - now).count()
        );
    }
}
