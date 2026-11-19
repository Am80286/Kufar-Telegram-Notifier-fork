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
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kufar.hpp"
#include "market_normalizer.hpp"

namespace Market {
    struct PriceAssessment;

    using Price = std::int64_t;
    using SnapshotClock = std::chrono::system_clock;
    using RatingCallback = std::function<void(PriceAssessment)>;

    inline constexpr std::size_t DEFAULT_MINIMUM_SAMPLE_SIZE = 10;
    inline constexpr std::size_t FALLBACK_MINIMUM_SAMPLE_SIZE = 5;
    inline constexpr double OUTLIER_IQR_MULTIPLIER = 1.5;
    inline constexpr auto DEFAULT_SNAPSHOT_TTL = std::chrono::hours(6);
    inline constexpr auto DEFAULT_REFRESH_FAILURE_COOLDOWN = std::chrono::minutes(5);
    inline constexpr std::size_t DEFAULT_DEFERRED_RATING_COUNT_LIMIT = 100;
    inline constexpr int DEFAULT_MARKET_CRAWL_DELAY_MILLISECONDS = 150;

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
        MarketKey key;
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
        bool baseFallback = false;

        bool outsideTypicalRange() const;
    };

    enum class DeferredRatingStatus {
        AddSuccess,
        NotDeferred,
        BadPrice,
        BadCallback,
        NotRateable,
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

    using MarketSnapshotMap = std::unordered_map<MarketKey, MarketSnapshot, MarketKeyHash>;

    // Builds snapshots for every market represented in the crawl. Exact
    // variant keys require the higher threshold; base-model fallback keys
    // require the lower threshold.
    MarketSnapshotMap buildSnapshots(
        const Kufar::FetchResult &fetchResult,
        const MarketNormalizerRegistry &normalizers,
        SnapshotClock::time_point fetchedAt = SnapshotClock::now(),
        std::size_t exactMinimumSampleSize = DEFAULT_MINIMUM_SAMPLE_SIZE,
        std::size_t fallbackMinimumSampleSize = FALLBACK_MINIMUM_SAMPLE_SIZE
    );

    std::optional<PriceAssessment> scorePrice(Price price, const MarketSnapshot &snapshot);

    class MarketCache {
        public:
            std::optional<MarketSnapshot> getSnapshot(const MarketKey &key) const;
            void publish(const MarketKey &key, MarketSnapshot snapshot);
            void remove(const MarketKey &key);

        private:
            mutable std::mutex mtx_;
            std::unordered_map<MarketKey, MarketSnapshot, MarketKeyHash> snapshots_;
    };

    class MarketAnalyzer {
        public:
            using CrawlFunction = std::function<Kufar::FetchResult(const Kufar::KufarConfiguration &, std::stop_token stopToken)>;

            explicit MarketAnalyzer(
                SnapshotClock::duration snapshotTTL = DEFAULT_SNAPSHOT_TTL,
                SnapshotClock::duration refreshFailureCooldown = DEFAULT_REFRESH_FAILURE_COOLDOWN,
                CrawlFunction crawlFunction = CrawlFunction(),
                MarketNormalizerRegistry normalizers = createDefaultMarketNormalizerRegistry()
            );
            ~MarketAnalyzer();

            MarketAnalyzer(const MarketAnalyzer &) = delete;
            MarketAnalyzer &operator=(const MarketAnalyzer &) = delete;

            std::optional<PriceAssessment> rate(const Kufar::Ad &ad);

            // Ratings are delivered outside analyzer locks. Exceptions thrown
            // by the callback are logged and never escape the analyzer worker.
            DeferredRatingStatus rateDeferred(
                const Kufar::Ad &ad,
                RatingCallback callback
            );

        private:
            struct RefreshRequest {
                MarketCrawlScope scope;
            };

            MarketCache cache_;
            SnapshotClock::duration snapshotTTL_;
            SnapshotClock::duration refreshFailureCooldown_;
            CrawlFunction crawlFunction_;
            MarketNormalizerRegistry normalizers_;

            std::mutex refreshMutex_;
            std::condition_variable refreshCondition_;
            std::deque<RefreshRequest> refreshQueue_;
            std::unordered_set<std::string> refreshingScopes_;
            std::unordered_map<std::string, SnapshotClock::time_point> nextRefreshAttempt_;
            std::unordered_map<std::string, SnapshotClock::time_point> scopeCrawlStartedAt_;

            std::mutex deferMutex_;
            std::unordered_map<std::string, std::deque<DeferredRating>> deferredCallbacks_;
            std::deque<std::deque<DeferredRating>> readyCallbacks_;

            // Keep last declared so it's destroyed first
            std::jthread worker_;

            struct SnapshotMatch {
                MarketSnapshot snapshot;
                bool baseFallback = false;
            };

            std::optional<SnapshotMatch> findSnapshot(const MarketKey &key) const;

            void requestRefresh(
                const MarketCrawlScope &scope,
                SnapshotClock::time_point now,
                std::optional<std::time_t> missingAdDate = std::nullopt
            );
            void workerLoop(std::stop_token stopToken);
            void runCrawl(RefreshRequest request, std::stop_token stopToken);
    };
}

#endif /* market_hpp */
