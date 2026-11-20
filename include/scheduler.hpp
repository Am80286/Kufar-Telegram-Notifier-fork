//
//  scheduler.hpp
//  Kufar Telegram Notifier
//
//  Per-query polling scheduler. Advances the fetch frontier independently of
//  delivery and chooses the next poll time using an adaptive interval.
//

#ifndef scheduler_hpp
#define scheduler_hpp

#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include "cache.hpp"
#include "delivery.hpp"
#include "kufar.hpp"
#include "queries.hpp"

namespace scheduling {
    using Clock = std::chrono::steady_clock;

    struct SchedulerConfiguration {
        std::chrono::seconds defaultInterval{60};
        std::chrono::seconds minInterval{60};
        std::chrono::seconds maxInterval{300};
        int jitterPercent = 15;
        int coldStartListingLimit = 10;
        int maxPages = 10;
        std::time_t frontierOverlapSeconds = 600;
    };

    struct PollSchedule {
        std::time_t frontier = 0;
        std::time_t suppressBefore = 0;
        bool overlapEnabled = false;
        bool initialized = false;
        std::chrono::seconds currentInterval{60};
        Clock::time_point nextDue;
    };

    using NewestFetcher = std::function<std::vector<Kufar::Ad>(
        const Kufar::KufarConfiguration &,
        int limit
    )>;

    using WindowFetcher = std::function<Kufar::FetchResult(
        const Kufar::KufarConfiguration &,
        const Kufar::FetchOptions &
    )>;

    using NowFunction = std::function<Clock::time_point()>;
    using StopFunction = std::function<bool()>;

    class PollingScheduler {
    public:
        PollingScheduler(
            queries::QueryStore &queryStore,
            cache::CacheStore &cache,
            delivery::DeliverySink &delivery,
            SchedulerConfiguration configuration,
            NewestFetcher newestFetcher = NewestFetcher(),
            WindowFetcher windowFetcher = WindowFetcher(),
            NowFunction now = NowFunction(),
            StopFunction stop = StopFunction()
        );

        void run();

        // Advances the query snapshot when QueryStore changed.
        void reconcile(Clock::time_point now);

        // Polls at most one due query. Returns how long to wait before the
        // next poll, or nullopt when there is nothing scheduled.
        std::optional<std::chrono::milliseconds> stepOnce(Clock::time_point now);

        std::optional<PollSchedule> scheduleFor(queries::QueryID id) const;

    private:
        std::chrono::seconds minIntervalFor(const queries::QueryConfiguration &) const;
        std::chrono::seconds maxIntervalFor(const queries::QueryConfiguration &) const;
        std::chrono::milliseconds jitter(std::chrono::seconds base);

        void initialize(
            queries::QueryID id,
            const queries::QueryConfiguration &query,
            Clock::time_point now
        );

        void poll(
            queries::QueryID id,
            PollSchedule &schedule,
            const queries::QueryConfiguration &query,
            Clock::time_point now
        );

        bool stopped() const;

        queries::QueryStore &queryStore_;
        cache::CacheStore &cache_;
        delivery::DeliverySink &delivery_;
        SchedulerConfiguration config_;
        NewestFetcher newestFetcher_;
        WindowFetcher windowFetcher_;
        NowFunction now_;
        StopFunction stop_;

        std::vector<queries::QueryConfiguration> queries_;
        std::uint64_t queryVersion_ = 0;
        std::unordered_map<queries::QueryID, PollSchedule> schedules_;
        std::mt19937 rng_;
    };
}

#endif /* scheduler_hpp */
