//
//  market.hpp
//  Kufar Telegram Notifier
//  
//  Created by Am80286 on 09.11.2026
//

#ifndef market_hpp
#define market_hpp

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kufar.hpp"
#include "queries.hpp"

namespace Market {
    struct PriceAssessment;

    using Price = std::int64_t;
    using SnapshotClock = std::chrono::system_clock;
    using RatingCallback = std::function<void(PriceAssessment)>;

    inline constexpr std::size_t DEFAULT_MINIMUM_SAMPLE_SIZE = 10;
    inline constexpr double OUTLIER_IQR_MULTIPLIER = 1.5;
    inline constexpr auto DEFAULT_SNAPSHOT_TTL = std::chrono::hours(6);
    inline constexpr auto DEFAULT_REFRESH_FAILURE_COOLDOWN = std::chrono::minutes(5);
    inline constexpr std::size_t DEFAULT_DEFERRED_RATING_COUNT_LIMIT = 100;

    struct PriceStatistics {
        std::size_t usableSampleCount = 0;
        std::size_t retainedSampleCount = 0;

        // All money values are kopecks
        double firstQuartile = 0.0;
        double median = 0.0;
        double thirdQuartile = 0.0;
        double interquartileRange = 0.0;
        double lowerFence = 0.0;
        double upperFence = 0.0;
        double filteredMedian = 0.0;

        bool outlierFilteringApplied = false;
    };

    struct DeferredRating {
        queries::QueryID id;
        double price = 0.0; // Kopecks
        RatingCallback callback;
    };

    struct MarketSnapshot {
        PriceStatistics statistics;
        SnapshotClock::time_point fetchedAt{};
        std::size_t crawledListingCount = 0;
        bool truncated = false;
    };

    struct PriceAssessment {
        double baselineMedian = 0.0; // Kopecks
        double deviationPercent = 0.0; // Negative is cheaper, positive is more expensive
        std::size_t baselineSampleCount = 0;
        bool belowTypicalRange = false;
        bool aboveTypicalRange = false;

        bool outsideTypicalRange() const;
    };

    enum class DeferredRatingStatus {
        AddSuccess,
        NotDeferred,
        BadPrice,
        BadCallback,
        NotRefreshing,
        RatingUnavailable,
    };

    std::optional<PriceStatistics> analyzePrices(
        const std::vector<Price> &prices,
        std::size_t minimumSampleSize = DEFAULT_MINIMUM_SAMPLE_SIZE
    );

    std::optional<MarketSnapshot> buildSnapshot(
        const Kufar::FetchResult &fetchResult,
        SnapshotClock::time_point fetchedAt = SnapshotClock::now(),
        std::size_t minimumSampleSize = DEFAULT_MINIMUM_SAMPLE_SIZE
    );

    std::optional<PriceAssessment> scorePrice(Price price, const MarketSnapshot &snapshot);

    class MarketCache {
        public:
            std::optional<MarketSnapshot> getSnapshot(queries::QueryID key) const;
            void publish(queries::QueryID queryId, MarketSnapshot snapshot);
            void remove(queries::QueryID queryId);

        private:
            mutable std::mutex mtx_;
            std::unordered_map<queries::QueryID, MarketSnapshot> snapshots_;
    };

    class MarketAnalyzer {
        public:
            using CrawlFunction = std::function<Kufar::FetchResult(const Kufar::KufarConfiguration &)>;

            explicit MarketAnalyzer(
                SnapshotClock::duration snapshotTTL = DEFAULT_SNAPSHOT_TTL,
                SnapshotClock::duration refreshFailureCooldown = DEFAULT_REFRESH_FAILURE_COOLDOWN,
                CrawlFunction crawlFunction = CrawlFunction()
            );
            ~MarketAnalyzer();

            MarketAnalyzer(const MarketAnalyzer &) = delete;
            MarketAnalyzer &operator=(const MarketAnalyzer &) = delete;

            std::optional<PriceAssessment> rate(const queries::QueryConfiguration &queryConfiguration, const Kufar::Ad &ad);

            // Ratings are delivered outside analyzer locks. Exceptions thrown
            // by the callback are logged and never escape the analyzer worker.
            DeferredRatingStatus rateDeferred(
                const queries::QueryConfiguration &queryConfiguration,
                const Kufar::Ad &ad,
                RatingCallback callback
            );

        private:
            MarketCache cache_;
            SnapshotClock::duration snapshotTTL_;
            SnapshotClock::duration refreshFailureCooldown_;
            CrawlFunction crawlFunction_;

            std::mutex refreshMutex_;
            std::condition_variable refreshCondition_;
            std::deque<queries::QueryConfiguration> refreshQueue_;
            std::unordered_set<queries::QueryID> refreshing_;
            std::unordered_map<queries::QueryID, SnapshotClock::time_point> nextRefreshAttempt_;

            std::mutex deferMutex_;
            std::unordered_map<queries::QueryID, std::deque<DeferredRating>> deferredCallbacks_;
            std::deque<std::deque<DeferredRating>> readyCallbacks_;

            // Keep last declared so it's destroyed first
            std::jthread worker_;

            void requestRefresh(const queries::QueryConfiguration &queryConfiguration, SnapshotClock::time_point now);
            void workerLoop(std::stop_token stopToken);
            void runCrawl(queries::QueryConfiguration queryConfiguration);
    };
}

#endif /* market_hpp */
