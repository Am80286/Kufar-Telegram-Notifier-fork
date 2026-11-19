//
//  market.cpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 09.11.2026
//

#include <algorithm>
#include <cmath>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

#include <spdlog/spdlog.h>

#include "market.hpp"
#include "kufar.hpp"
#include "market_normalizer.hpp"

namespace Market {
    using namespace std;
    using namespace Kufar;

    namespace {
        constexpr time_t AD_DATE_TOLERANCE_SECONDS = 60;

        // Type 7 quantile: linear interpolation at (n - 1) * probability.
        // The input must be nonempty and sorted in ascending order
        double quantileSorted(const std::vector<Price> &sortedPrices, double probability) {
            const double position = static_cast<double>(sortedPrices.size() - 1) * probability;
            const std::size_t lowerIndex = static_cast<std::size_t>(std::floor(position));
            const std::size_t upperIndex = static_cast<std::size_t>(std::ceil(position));
            const double weight = position - static_cast<double>(lowerIndex);

            const double lowerValue = static_cast<double>(sortedPrices[lowerIndex]);
            const double upperValue = static_cast<double>(sortedPrices[upperIndex]);
            return lowerValue + (upperValue - lowerValue) * weight;
        }

        double medianSorted(const std::vector<Price> &sortedPrices) {
            return quantileSorted(sortedPrices, 0.5);
        }

        bool invokeRatingCallback(const RatingCallback &callback, const PriceAssessment &assessment, const MarketKey &key) {
            if (!callback) {
                spdlog::error("[Market] {} has an empty deferred rating callback.", key.toString());
                return false;
            }

            try {
                callback(assessment);
                return true;
            } catch (const exception &exc) {
                spdlog::error("[Market] {} deferred rating callback failed: {}", key.toString(), exc.what());
            } catch (...) {
                spdlog::error("[Market] {} deferred rating callback failed with an unknown error.", key.toString());
            }
            return false;
        }
    }

    bool PriceAssessment::outsideTypicalRange() const {
        return belowTypicalRange || aboveTypicalRange;
    }

    std::optional<PriceStatistics> analyzePrices(const std::vector<Price> &prices, std::size_t minimumSampleSize) {
        if (minimumSampleSize == 0) {
            throw std::invalid_argument("Market minimum sample size must be positive.");
        }

        std::vector<Price> sortedPrices;
        sortedPrices.reserve(prices.size());
        for (const Price price : prices) {
            if (price > 0) {
                sortedPrices.push_back(price);
            }
        }

        if (sortedPrices.size() < minimumSampleSize) {
            return std::nullopt;
        }

        std::sort(sortedPrices.begin(), sortedPrices.end());

        PriceStatistics statistics;
        statistics.usableSampleCount = sortedPrices.size();
        statistics.firstQuartile = quantileSorted(sortedPrices, 0.25);
        statistics.median = medianSorted(sortedPrices);
        statistics.thirdQuartile = quantileSorted(sortedPrices, 0.75);
        statistics.interquartileRange = statistics.thirdQuartile - statistics.firstQuartile;
        statistics.lowerFence = statistics.firstQuartile - OUTLIER_IQR_MULTIPLIER * statistics.interquartileRange;
        statistics.upperFence = statistics.thirdQuartile + OUTLIER_IQR_MULTIPLIER * statistics.interquartileRange;

        if (statistics.interquartileRange == 0.0) {
            statistics.retainedSampleCount = sortedPrices.size();
            statistics.filteredMedian = statistics.median;
            return statistics;
        }

        std::vector<Price> retainedPrices;
        retainedPrices.reserve(sortedPrices.size());
        for (const Price price : sortedPrices) {
            const double value = static_cast<double>(price);
            if (value >= statistics.lowerFence && value <= statistics.upperFence) {
                retainedPrices.push_back(price);
            }
        }

        if (retainedPrices.empty()) {
            return std::nullopt;
        }

        statistics.outlierFilteringApplied = true;
        statistics.retainedSampleCount = retainedPrices.size();
        statistics.filteredMedian = medianSorted(retainedPrices);
        return statistics;
    }

    std::optional<MarketSnapshot> buildSnapshot(
        const Kufar::FetchResult &fetchResult,
        SnapshotClock::time_point fetchedAt,
        std::size_t minimumSampleSize
    ) {
        std::vector<Price> prices;
        prices.reserve(fetchResult.ads.size());
        for (const Kufar::Ad &ad : fetchResult.ads) {
            prices.push_back(static_cast<Price>(ad.price));
        }

        std::optional<PriceStatistics> statistics = analyzePrices(prices, minimumSampleSize);
        if (!statistics.has_value()) {
            return std::nullopt;
        }

        MarketSnapshot snapshot;
        snapshot.statistics = std::move(statistics.value());
        snapshot.fetchedAt = fetchedAt;
        snapshot.crawledListingCount = fetchResult.ads.size();
        snapshot.truncated = fetchResult.truncated;
        return snapshot;
    }

    MarketSnapshotMap buildSnapshots(
        const Kufar::FetchResult &fetchResult,
        const MarketNormalizerRegistry &normalizers,
        SnapshotClock::time_point fetchedAt,
        std::size_t exactMinimumSampleSize,
        std::size_t fallbackMinimumSampleSize
    ) {
        if (exactMinimumSampleSize == 0 || fallbackMinimumSampleSize == 0) {
            throw std::invalid_argument("Market minimum sample sizes must be positive.");
        }

        std::unordered_map<MarketKey, std::vector<Price>, MarketKeyHash> exactPrices;
        std::unordered_map<MarketKey, std::vector<Price>, MarketKeyHash> basePrices;

        for (const Kufar::Ad &ad : fetchResult.ads) {
            const NormalizationResult normalized = normalizers.normalize(ad);
            if (!normalized.key.has_value()) {
                continue;
            }

            const MarketKey &key = normalized.key.value();
            const Price price = static_cast<Price>(ad.price);
            exactPrices[key].push_back(price);
            basePrices[key.baseModel()].push_back(price);
        }

        const auto build = [&](const std::vector<Price> &prices, std::size_t minimumSampleSize) {
            std::optional<PriceStatistics> statistics = analyzePrices(prices, minimumSampleSize);
            if (!statistics.has_value()) {
                return std::optional<MarketSnapshot>();
            }

            MarketSnapshot snapshot;
            snapshot.statistics = std::move(statistics.value());
            snapshot.fetchedAt = fetchedAt;
            snapshot.crawledListingCount = fetchResult.ads.size();
            snapshot.truncated = fetchResult.truncated;
            return std::optional<MarketSnapshot>(std::move(snapshot));
        };

        MarketSnapshotMap snapshots;

        for (const auto &[key, prices] : exactPrices) {
            const std::size_t minimumSampleSize = key.variant.empty() ? fallbackMinimumSampleSize : exactMinimumSampleSize;
            std::optional<MarketSnapshot> snapshot = build(prices, minimumSampleSize);
            if (snapshot.has_value()) {
                snapshots.emplace(key, std::move(snapshot.value()));
            }
        }

        for (const auto &[key, prices] : basePrices) {
            if (snapshots.contains(key)) {
                continue;
            }

            std::optional<MarketSnapshot> snapshot = build(prices, fallbackMinimumSampleSize);
            if (snapshot.has_value()) {
                snapshots.emplace(key, std::move(snapshot.value()));
            }
        }

        return snapshots;
    }

    std::optional<PriceAssessment> scorePrice(Price price, const MarketSnapshot &snapshot) {
        const PriceStatistics &statistics = snapshot.statistics;
        const double baselineMedian = statistics.filteredMedian;
        if (
            price <= 0 ||
            statistics.retainedSampleCount == 0 ||
            baselineMedian <= 0.0 ||
            !std::isfinite(baselineMedian)
        ) {
            return std::nullopt;
        }

        const double currentPrice = static_cast<double>(price);

        PriceAssessment assessment;
        assessment.baselineMedian = baselineMedian;
        assessment.deviationPercent = 100.0 * (currentPrice - baselineMedian) / baselineMedian;
        assessment.baselineSampleCount = statistics.retainedSampleCount;

        // A zero IQR does not provide a useful typical range for classifying
        // the candidate, even though its median is still a valid baseline
        if (statistics.outlierFilteringApplied) {
            assessment.belowTypicalRange = currentPrice < statistics.lowerFence;
            assessment.aboveTypicalRange = currentPrice > statistics.upperFence;
        }

        return assessment;
    }

    optional<MarketSnapshot> MarketCache::getSnapshot(const MarketKey &key) const {
        lock_guard<std::mutex> lock(mtx_);

        const auto it = snapshots_.find(key);
        if (it == snapshots_.end())
            return nullopt;

        return it->second;
    }

    void MarketCache::publish(const MarketKey &key, MarketSnapshot snapshot) {
        lock_guard<std::mutex> lock(mtx_);
        snapshots_.insert_or_assign(key, std::move(snapshot));
    }

    void MarketCache::remove(const MarketKey &key) {
        lock_guard<std::mutex> lock(mtx_);
        snapshots_.erase(key);
    }

    MarketAnalyzer::MarketAnalyzer(
        SnapshotClock::duration snapshotTTL,
        SnapshotClock::duration refreshFailureCooldown,
        CrawlFunction crawlFunction,
        MarketNormalizerRegistry normalizers
    ) : snapshotTTL_(snapshotTTL),
        refreshFailureCooldown_(refreshFailureCooldown),
        crawlFunction_(std::move(crawlFunction)),
        normalizers_(std::move(normalizers)) {
        if (snapshotTTL_ < SnapshotClock::duration::zero()) {
            throw invalid_argument("Market snapshot TTL must not be negative.");
        }
        if (refreshFailureCooldown_ < SnapshotClock::duration::zero()) {
            throw invalid_argument("Market refresh failure cooldown must not be negative.");
        }
        if (!crawlFunction_) {
            crawlFunction_ = [](const KufarConfiguration &configuration, stop_token stopToken) {
                FetchOptions options;
                options.delayMilliseconds = DEFAULT_MARKET_CRAWL_DELAY_MILLISECONDS;
                options.shouldStop = [stopToken] { return stopToken.stop_requested(); };
                return getAllAds(configuration, options);
            };
        }

        worker_ = jthread([this](stop_token stopToken) {
            workerLoop(stopToken);
        });
    }

    MarketAnalyzer::~MarketAnalyzer() {
        worker_.request_stop();
        refreshCondition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    optional<MarketAnalyzer::SnapshotMatch> MarketAnalyzer::findSnapshot(const MarketKey &key) const {
        optional<MarketSnapshot> exact = cache_.getSnapshot(key);
        if (exact.has_value()) {
            SnapshotMatch match;
            match.snapshot = std::move(exact.value());
            return match;
        }

        const MarketKey baseKey = key.baseModel();
        if (baseKey == key) {
            return nullopt;
        }

        optional<MarketSnapshot> fallback = cache_.getSnapshot(baseKey);
        if (!fallback.has_value()) {
            return nullopt;
        }

        SnapshotMatch match;
        match.snapshot = std::move(fallback.value());
        match.baseFallback = true;
        return match;
    }

    optional<PriceAssessment> MarketAnalyzer::rate(const Ad &ad) {
        if (ad.price <= 0) {
            return nullopt;
        }

        const NormalizationResult normalized = normalizers_.normalize(ad);
        if (!normalized.key.has_value() || normalized.normalizer == nullptr) {
            spdlog::debug("[Market] Ad {} is not rateable: {}", ad.id, normalized.reason);
            return nullopt;
        }

        const MarketKey &key = normalized.key.value();
        const optional<MarketCrawlScope> scope = normalized.normalizer->buildCrawlScope(key);
        if (!scope.has_value()) {
            spdlog::warn("[Market] {} has no crawl scope.", key.toString());
            return nullopt;
        }

        const optional<SnapshotMatch> match = findSnapshot(key);
        const SnapshotClock::time_point now = SnapshotClock::now();

        if (!match.has_value()) {
            requestRefresh(scope.value(), now, ad.date);
            return nullopt;
        }

        if (now - match->snapshot.fetchedAt >= snapshotTTL_) {
            requestRefresh(scope.value(), now);
        }

        optional<PriceAssessment> assessment = scorePrice(static_cast<Price>(ad.price), match->snapshot);
        if (assessment.has_value()) {
            assessment->baseFallback = match->baseFallback;
        }
        return assessment;
    }

    DeferredRatingStatus MarketAnalyzer::rateDeferred(const Ad &ad, RatingCallback callback) {
        if (ad.price <= 0) {
            return DeferredRatingStatus::BadPrice;
        }
        if (!callback) {
            return DeferredRatingStatus::BadCallback;
        }

        const NormalizationResult normalized = normalizers_.normalize(ad);
        if (!normalized.key.has_value() || normalized.normalizer == nullptr) {
            return DeferredRatingStatus::NotRateable;
        }

        const MarketKey &key = normalized.key.value();
        const optional<MarketCrawlScope> scope = normalized.normalizer->buildCrawlScope(key);
        if (!scope.has_value()) {
            return DeferredRatingStatus::NotRateable;
        }

        optional<SnapshotMatch> match;
        {
            lock_guard<mutex> refreshLock(refreshMutex_);
            if (refreshingScopes_.contains(scope->id)) {
                lock_guard<mutex> deferLock(deferMutex_);
                deque<DeferredRating> &deferred = deferredCallbacks_[scope->id];
                if (deferred.size() >= DEFAULT_DEFERRED_RATING_COUNT_LIMIT) {
                    deferred.pop_front();
                }

                DeferredRating rating;
                rating.key = key;
                rating.callback = std::move(callback);
                rating.price = ad.price;
                deferred.push_back(std::move(rating));

                return DeferredRatingStatus::AddSuccess;
            }

            match = findSnapshot(key);
        }

        if (!match.has_value()) {
            return DeferredRatingStatus::NotRefreshing;
        }

        optional<PriceAssessment> assessment =
            scorePrice(static_cast<Price>(ad.price), match->snapshot);
        if (!assessment.has_value()) {
            return DeferredRatingStatus::RatingUnavailable;
        }

        assessment->baseFallback = match->baseFallback;
        invokeRatingCallback(callback, assessment.value(), key);
        return DeferredRatingStatus::NotDeferred;
    }

    void MarketAnalyzer::requestRefresh(
        const MarketCrawlScope &scope,
        SnapshotClock::time_point now,
        std::optional<time_t> missingAdDate
    ) {
        {
            lock_guard<mutex> lock(refreshMutex_);
            if (refreshingScopes_.contains(scope.id)) {
                return;
            }

            const auto retry = nextRefreshAttempt_.find(scope.id);
            if (retry != nextRefreshAttempt_.end() && now < retry->second) {
                return;
            }

            if (missingAdDate.has_value()) {
                const auto lastCrawl = scopeCrawlStartedAt_.find(scope.id);
                if (lastCrawl != scopeCrawlStartedAt_.end()) {
                    const time_t lastCrawlStart = SnapshotClock::to_time_t(lastCrawl->second);
                    if (missingAdDate.value() + AD_DATE_TOLERANCE_SECONDS < lastCrawlStart) {
                        return;
                    }
                }
            }

            refreshingScopes_.insert(scope.id);
            RefreshRequest request;
            request.scope = scope;
            refreshQueue_.push_back(std::move(request));
        }
        refreshCondition_.notify_one();
    }

    void MarketAnalyzer::workerLoop(stop_token stopToken) {
        while (true) {
            unique_lock<mutex> lock(refreshMutex_);
            refreshCondition_.wait(lock, [this, &stopToken] {
                return stopToken.stop_requested() || !refreshQueue_.empty();
            });

            if (stopToken.stop_requested()) {
                return;
            }

            RefreshRequest request = std::move(refreshQueue_.front());
            refreshQueue_.pop_front();
            lock.unlock();

            runCrawl(std::move(request), stopToken);

            while (!readyCallbacks_.empty()) {
                deque<DeferredRating> toRun = std::move(readyCallbacks_.front());
                readyCallbacks_.pop_front();
                if (toRun.empty()) {
                    continue;
                }

                while (!toRun.empty()) {
                    DeferredRating rating = std::move(toRun.front());
                    toRun.pop_front();

                    const optional<SnapshotMatch> match = findSnapshot(rating.key);
                    if (!match.has_value()) {
                        spdlog::warn("[Market] {} has no snapshot for a deferred rating.", rating.key.toString());
                        continue;
                    }

                    optional<PriceAssessment> assessment =
                        scorePrice(static_cast<Price>(rating.price), match->snapshot);
                    if (!assessment.has_value()) {
                        spdlog::warn("[Market] {} could not score a deferred rating.", rating.key.toString());
                        continue;
                    }

                    assessment->baseFallback = match->baseFallback;
                    invokeRatingCallback(rating.callback, assessment.value(), rating.key);
                }
            }
        }
    }

    void MarketAnalyzer::runCrawl(RefreshRequest request, stop_token stopToken) {
        bool succeeded = false;
        const string scopeID = request.scope.id;
        const SnapshotClock::time_point crawlStartedAt = SnapshotClock::now();
        std::size_t publishedCount = 0;
        std::size_t crawledCount = 0;

        try {
            FetchResult fetchResult = crawlFunction_(request.scope.configuration, stopToken);
            crawledCount = fetchResult.ads.size();

            if (fetchResult.truncated) {
                spdlog::warn("[Market] Scope {} crawl was truncated; keeping the previous snapshots.", scopeID);
            } else {
                MarketSnapshotMap snapshots = buildSnapshots(fetchResult, normalizers_);
                for (auto &[key, snapshot] : snapshots) {
                    cache_.publish(key, std::move(snapshot));
                }

                publishedCount = snapshots.size();
                std::size_t baseCount = 0;
                for (const auto &entry : snapshots) {
                    if (entry.first.variant.empty()) {
                        baseCount++;
                    }
                }
                succeeded = true;
                spdlog::info(
                    "[Market] Scope {} crawl published {} market snapshots ({} exact, {} base) from {} listings.",
                    scopeID,
                    publishedCount,
                    publishedCount - baseCount,
                    baseCount,
                    crawledCount
                );
            }
        } catch (const exception &exc) {
            spdlog::error("[Market] Scope {} refresh failed: {}", scopeID, exc.what());
        } catch (...) {
            spdlog::error("[Market] Scope {} refresh failed with an unknown error.", scopeID);
        }

        lock_guard<mutex> lock(refreshMutex_);
        refreshingScopes_.erase(scopeID);
        if (succeeded) {
            nextRefreshAttempt_.erase(scopeID);
            scopeCrawlStartedAt_[scopeID] = crawlStartedAt;

            deque<DeferredRating> ready;
            {
                lock_guard<mutex> deferLock(deferMutex_);
                const auto deferred = deferredCallbacks_.find(scopeID);
                if (deferred != deferredCallbacks_.end()) {
                    ready.swap(deferred->second);
                    deferredCallbacks_.erase(deferred);
                }
            }
            if (!ready.empty()) {
                readyCallbacks_.push_back(std::move(ready));
            }
        } else {
            size_t discarded = 0;
            {
                lock_guard<mutex> deferLock(deferMutex_);
                const auto deferred = deferredCallbacks_.find(scopeID);
                if (deferred != deferredCallbacks_.end()) {
                    discarded = deferred->second.size();
                    deferredCallbacks_.erase(deferred);
                }
            }

            if (discarded > 0) {
                spdlog::warn(
                    "[Market] Scope {} discarded {} deferred rating(s) after a failed refresh.",
                    scopeID,
                    discarded
                );
            }

            nextRefreshAttempt_[scopeID] = SnapshotClock::now() + refreshFailureCooldown_;
        }
    }
}
