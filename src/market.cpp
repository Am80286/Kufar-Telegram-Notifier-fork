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
#include <utility>

#include <spdlog/spdlog.h>

#include "market.hpp"
#include "kufar.hpp"
#include "queries.hpp"

namespace Market {
    using namespace std;
    using namespace queries;
    using namespace Kufar;

    namespace {
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

        bool invokeRatingCallback(const RatingCallback &callback, const PriceAssessment &assessment, QueryID queryId) {
            if (!callback) {
                spdlog::error("[Market] Query {} has an empty deferred rating callback.", queryId);
                return false;
            }

            try {
                callback(assessment);
                return true;
            } catch (const exception &exc) {
                spdlog::error("[Market] Query {} deferred rating callback failed: {}", queryId, exc.what());
            } catch (...) {
                spdlog::error("[Market] Query {} deferred rating callback failed with an unknown error.", queryId);
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

    optional<MarketSnapshot> MarketCache::getSnapshot(QueryID key) const {
        lock_guard<std::mutex> lock(mtx_);

        const auto it = snapshots_.find(key);
        if (it == snapshots_.end())
            return nullopt;

        return it->second;
    }

    void MarketCache::publish(QueryID queryId, MarketSnapshot snapshot) {
        lock_guard<std::mutex> lock(mtx_);
        snapshots_.insert_or_assign(queryId, std::move(snapshot));
    }

    void MarketCache::remove(QueryID queryId) {
        lock_guard<std::mutex> lock(mtx_);
        snapshots_.erase(queryId);
    }

    MarketAnalyzer::MarketAnalyzer(
        SnapshotClock::duration snapshotTTL,
        SnapshotClock::duration refreshFailureCooldown,
        CrawlFunction crawlFunction
    ) : snapshotTTL_(snapshotTTL),
        refreshFailureCooldown_(refreshFailureCooldown),
        crawlFunction_(std::move(crawlFunction)) {
        if (snapshotTTL_ < SnapshotClock::duration::zero()) {
            throw invalid_argument("Market snapshot TTL must not be negative.");
        }
        if (refreshFailureCooldown_ < SnapshotClock::duration::zero()) {
            throw invalid_argument("Market refresh failure cooldown must not be negative.");
        }
        if (!crawlFunction_) {
            crawlFunction_ = [](const KufarConfiguration &configuration) {
                return getAllAds(configuration);
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

    optional<PriceAssessment> MarketAnalyzer::rate(const QueryConfiguration &queryConfiguration, const Ad &ad) {
        if (ad.price <= 0) {
            return nullopt;
        }

        optional<MarketSnapshot> snapshot = cache_.getSnapshot(queryConfiguration.id);
        const SnapshotClock::time_point now = SnapshotClock::now();
        const bool refreshNeeded = !snapshot.has_value() || now - snapshot->fetchedAt >= snapshotTTL_;

        if (refreshNeeded) {
            requestRefresh(queryConfiguration, now);
        }

        if (!snapshot.has_value()) {
            return nullopt;
        }

        return scorePrice(static_cast<Price>(ad.price), snapshot.value());
    }

    DeferredRatingStatus MarketAnalyzer::rateDeferred(const QueryConfiguration &queryConfiguration, const Ad &ad, RatingCallback callback) {
        if (ad.price <= 0) {
            return DeferredRatingStatus::BadPrice;
        }
        if (!callback) {
            return DeferredRatingStatus::BadCallback;
        }

        optional<MarketSnapshot> snapshot;
        {
            lock_guard<mutex> refreshLock(refreshMutex_);
            if (refreshing_.contains(queryConfiguration.id)) {
                lock_guard<mutex> deferLock(deferMutex_);
                deque<DeferredRating> &deferred = deferredCallbacks_[queryConfiguration.id];
                if (deferred.size() >= DEFAULT_DEFERRED_RATING_COUNT_LIMIT) {
                    deferred.pop_front();
                }

                DeferredRating rating;
                rating.id = queryConfiguration.id;
                rating.callback = std::move(callback);
                rating.price = ad.price;
                deferred.push_back(std::move(rating));

                return DeferredRatingStatus::AddSuccess;
            }

            snapshot = cache_.getSnapshot(queryConfiguration.id);
        }

        if (!snapshot.has_value()) {
            return DeferredRatingStatus::NotRefreshing;
        }

        const optional<PriceAssessment> assessment = scorePrice(static_cast<Price>(ad.price), snapshot.value());
        if (!assessment.has_value()) {
            return DeferredRatingStatus::RatingUnavailable;
        }

        invokeRatingCallback(callback, assessment.value(), queryConfiguration.id);
        return DeferredRatingStatus::NotDeferred;
    }

    void MarketAnalyzer::requestRefresh(const QueryConfiguration &queryConfiguration, SnapshotClock::time_point now) {
        {
            lock_guard<mutex> lock(refreshMutex_);
            if (refreshing_.contains(queryConfiguration.id)) {
                return;
            }

            // Time of check time of use shit 
            const optional<MarketSnapshot> currentSnapshot = cache_.getSnapshot(queryConfiguration.id);
            if (currentSnapshot.has_value() && now - currentSnapshot->fetchedAt < snapshotTTL_) {
                return;
            }

            const auto retry = nextRefreshAttempt_.find(queryConfiguration.id);
            if (retry != nextRefreshAttempt_.end() && now < retry->second) {
                return;
            }

            refreshing_.insert(queryConfiguration.id);
            refreshQueue_.push_back(queryConfiguration);
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

            QueryConfiguration queryConfiguration = std::move(refreshQueue_.front());
            refreshQueue_.pop_front();
            lock.unlock();

            runCrawl(std::move(queryConfiguration));

            while (!readyCallbacks_.empty()) {
                deque<DeferredRating> toRun = std::move(readyCallbacks_.front());
                readyCallbacks_.pop_front();
                if (toRun.empty()) {
                    continue;
                }

                const optional<MarketSnapshot> snapshot = cache_.getSnapshot(toRun.front().id);
                while (!toRun.empty()) {
                    DeferredRating rating = std::move(toRun.front());
                    toRun.pop_front();

                    if (!snapshot.has_value()) {
                        spdlog::warn("[Market] Query {} has no snapshot for a deferred rating.", rating.id);
                        continue;
                    }

                    const optional<PriceAssessment> assessment = scorePrice(static_cast<Price>(rating.price), snapshot.value());
                    if (!assessment.has_value()) {
                        spdlog::warn("[Market] Query {} could not score a deferred rating.", rating.id);
                        continue;
                    }

                    invokeRatingCallback(rating.callback, assessment.value(), rating.id);
                }
            }
        }
    }

    void MarketAnalyzer::runCrawl(QueryConfiguration queryConfiguration) {
        bool published = false;

        try {
            queryConfiguration.kufar.priceRange = PriceRange();
            queryConfiguration.kufar.sortType.reset();

            FetchResult fetchResult = crawlFunction_(queryConfiguration.kufar);
            if (fetchResult.truncated) {
                spdlog::warn( "[Market] Query {} crawl was truncated; keeping the previous snapshot.", queryConfiguration.id);
            } else {
                optional<MarketSnapshot> snapshot = buildSnapshot(fetchResult);
                if (snapshot.has_value()) {
                    cache_.publish(queryConfiguration.id, snapshot.value());
                    published = true;
                    spdlog::info(
                        "[Market] Query {} snapshot published: {} listings, {} usable prices, {} retained prices.",
                        queryConfiguration.id,
                        snapshot->crawledListingCount,
                        snapshot->statistics.usableSampleCount,
                        snapshot->statistics.retainedSampleCount
                    );
                } else {
                    spdlog::warn( "[Market] Query {} has insufficient usable prices; keeping the previous snapshot.", queryConfiguration.id);
                }
            }
        } catch (const exception &exc) {
            spdlog::error("[Market] Query {} refresh failed: {}", queryConfiguration.id, exc.what());
        } catch (...) {
            spdlog::error("[Market] Query {} refresh failed with an unknown error.", queryConfiguration.id);
        }

        lock_guard<mutex> lock(refreshMutex_);
        refreshing_.erase(queryConfiguration.id);
        if (published) {
            nextRefreshAttempt_.erase(queryConfiguration.id);

            deque<DeferredRating> ready;
            {
                lock_guard<mutex> deferLock(deferMutex_);
                const auto deferred = deferredCallbacks_.find(queryConfiguration.id);
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
                const auto deferred = deferredCallbacks_.find(queryConfiguration.id);
                if (deferred != deferredCallbacks_.end()) {
                    discarded = deferred->second.size();
                    deferredCallbacks_.erase(deferred);
                }
            }

            if (discarded > 0) {
                spdlog::warn(
                    "[Market] Query {} discarded {} deferred rating(s) after a failed refresh.",
                    queryConfiguration.id,
                    discarded
                );
            }

            nextRefreshAttempt_[queryConfiguration.id] = SnapshotClock::now() + refreshFailureCooldown_;
        }
    }
}
