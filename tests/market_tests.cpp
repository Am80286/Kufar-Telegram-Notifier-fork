#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "market.hpp"

using namespace std;

namespace {
    void check(bool condition, const char *expression, int line) {
        if (!condition) {
            throw runtime_error("Check failed at line " + to_string(line) + ": " + expression);
        }
    }

    bool close(double actual, double expected) {
        return abs(actual - expected) < 1e-9;
    }

    Kufar::FetchResult makeFetchResult() {
        Kufar::FetchResult result;
        for (int index = 0; index < 10; ++index) {
            Kufar::Ad ad{};
            ad.id = index + 1;
            ad.price = 10000;
            result.ads.push_back(ad);
        }
        result.total = result.ads.size();
        result.pagesFetched = 1;
        return result;
    }

    void testPriceStatistics() {
        const auto statistics = Market::analyzePrices({100, 102, 98, 101, 99, 10000}, 1);
        check(statistics.has_value(), "statistics available", __LINE__);
        check(statistics->usableSampleCount == 6, "usable sample count", __LINE__);
        check(statistics->retainedSampleCount == 5, "outlier removed", __LINE__);
        check(close(statistics->median, 100.5), "ordinary median", __LINE__);
        check(close(statistics->firstQuartile, 99.25), "first quartile", __LINE__);
        check(close(statistics->thirdQuartile, 101.75), "third quartile", __LINE__);
        check(close(statistics->lowerFence, 95.5), "lower fence", __LINE__);
        check(close(statistics->upperFence, 105.5), "upper fence", __LINE__);
        check(close(statistics->filteredMedian, 100.0), "filtered median", __LINE__);
    }

    void testInvalidAndZeroIqrSamples() {
        check(!Market::analyzePrices({}, 1).has_value(), "empty sample rejected", __LINE__);
        check(!Market::analyzePrices({0, -1, 100}, 2).has_value(), "insufficient sample rejected", __LINE__);

        const auto statistics = Market::analyzePrices({100, 100, 100, 100, 10000}, 1);
        check(statistics.has_value(), "zero-IQR statistics available", __LINE__);
        check(!statistics->outlierFilteringApplied, "zero-IQR filtering skipped", __LINE__);
        check(statistics->retainedSampleCount == 5, "zero-IQR samples retained", __LINE__);
        check(close(statistics->filteredMedian, 100.0), "zero-IQR median", __LINE__);
    }

    void testPriceScoring() {
        Kufar::FetchResult fetchResult;
        for (const Market::Price price : vector<Market::Price>{100, 102, 98, 101, 99, 10000}) {
            Kufar::Ad ad{};
            ad.price = static_cast<int>(price);
            fetchResult.ads.push_back(ad);
        }

        const auto snapshot = Market::buildSnapshot(fetchResult, Market::SnapshotClock::time_point{}, 1);
        check(snapshot.has_value(), "snapshot available", __LINE__);

        const auto cheap = Market::scorePrice(80, snapshot.value());
        check(cheap.has_value(), "cheap listing scored", __LINE__);
        check(close(cheap->deviationPercent, -20.0), "cheap deviation", __LINE__);
        check(cheap->belowTypicalRange, "cheap listing below typical range", __LINE__);

        const auto expensive = Market::scorePrice(120, snapshot.value());
        check(expensive.has_value(), "expensive listing scored", __LINE__);
        check(close(expensive->deviationPercent, 20.0), "expensive deviation", __LINE__);
        check(expensive->aboveTypicalRange, "expensive listing above typical range", __LINE__);

        const auto outlier = Market::scorePrice(10000, snapshot.value());
        check(outlier.has_value(), "candidate outlier still scored", __LINE__);
        check(close(outlier->deviationPercent, 9900.0), "candidate outlier deviation", __LINE__);
        check(!Market::scorePrice(0, snapshot.value()).has_value(), "unknown price not scored", __LINE__);
    }

    void testMarketCache() {
        Market::MarketCache cache;
        check(!cache.getSnapshot(7).has_value(), "empty cache", __LINE__);

        Market::MarketSnapshot snapshot;
        snapshot.statistics.filteredMedian = 10000;
        snapshot.statistics.retainedSampleCount = 10;
        cache.publish(7, snapshot);

        auto cached = cache.getSnapshot(7);
        check(cached.has_value(), "published snapshot retrieved", __LINE__);
        check(close(cached->statistics.filteredMedian, 10000), "cached median", __LINE__);

        cached->statistics.filteredMedian = 1;
        check(close(cache.getSnapshot(7)->statistics.filteredMedian, 10000), "cache returns independent copies", __LINE__);

        cache.remove(7);
        check(!cache.getSnapshot(7).has_value(), "snapshot removed", __LINE__);
    }

    void testAnalyzerColdMissAndSingleFlight() {
        atomic<int> crawlCalls = 0;
        mutex crawlMutex;
        condition_variable crawlCondition;
        bool releaseCrawl = false;
        Kufar::KufarConfiguration observedConfiguration;

        Market::MarketAnalyzer analyzer(
            chrono::hours(1),
            chrono::minutes(5),
            [&](const Kufar::KufarConfiguration &configuration) {
                ++crawlCalls;
                {
                    unique_lock<mutex> lock(crawlMutex);
                    observedConfiguration = configuration;
                    crawlCondition.wait(lock, [&] { return releaseCrawl; });
                }
                return makeFetchResult();
            }
        );

        queries::QueryConfiguration query{};
        query.id = 7;
        query.kufar.priceRange.priceMin = 10;
        query.kufar.priceRange.priceMax = 20;
        query.kufar.sortType = Kufar::SortType::ascending;

        Kufar::Ad ad{};
        ad.price = 8000;
        Kufar::Ad unpricedAd = ad;
        unpricedAd.price = 0;

        check(!analyzer.rate(query, unpricedAd).has_value(), "unpriced ad not rated", __LINE__);
        check(crawlCalls.load() == 0, "unpriced ad does not crawl", __LINE__);
        check(!analyzer.rate(query, ad).has_value(), "cold miss returns no assessment", __LINE__);

        const auto crawlDeadline = chrono::steady_clock::now() + chrono::seconds(2);
        while (crawlCalls.load() == 0 && chrono::steady_clock::now() < crawlDeadline) {
            this_thread::sleep_for(chrono::milliseconds(5));
        }
        check(crawlCalls.load() == 1, "cold miss schedules one crawl", __LINE__);

        for (int index = 0; index < 20; ++index) {
            check(!analyzer.rate(query, ad).has_value(), "in-flight cold miss remains unrated", __LINE__);
        }
        check(crawlCalls.load() == 1, "in-flight crawl is deduplicated", __LINE__);

        {
            lock_guard<mutex> lock(crawlMutex);
            releaseCrawl = true;
        }
        crawlCondition.notify_all();

        optional<Market::PriceAssessment> assessment;
        const auto publishDeadline = chrono::steady_clock::now() + chrono::seconds(2);
        while (!assessment.has_value() && chrono::steady_clock::now() < publishDeadline) {
            assessment = analyzer.rate(query, ad);
            if (!assessment.has_value()) {
                this_thread::sleep_for(chrono::milliseconds(5));
            }
        }

        check(assessment.has_value(), "published snapshot rates listing", __LINE__);
        check(close(assessment->baselineMedian, 10000), "analyzer baseline median", __LINE__);
        check(close(assessment->deviationPercent, -20.0), "analyzer deviation", __LINE__);
        check(crawlCalls.load() == 1, "fresh snapshot does not refresh", __LINE__);

        check(!observedConfiguration.priceRange.priceMin.has_value(), "baseline minimum price cleared", __LINE__);
        check(!observedConfiguration.priceRange.priceMax.has_value(), "baseline maximum price cleared", __LINE__);
        check(!observedConfiguration.sortType.has_value(), "baseline sorting cleared", __LINE__);
        check(query.kufar.priceRange.priceMin.has_value(), "notification query not mutated", __LINE__);
        check(query.kufar.sortType.has_value(), "notification sort not mutated", __LINE__);

        for (int index = 0; index < 10; ++index) {
            check(analyzer.rate(query, ad).has_value(), "fresh cache hit", __LINE__);
        }
        check(crawlCalls.load() == 1, "fresh cache hits do not crawl", __LINE__);
    }

    void testAnalyzerServesStaleDuringRefresh() {
        atomic<int> crawlCalls = 0;
        mutex crawlMutex;
        condition_variable crawlCondition;
        bool releaseSecondCrawl = false;

        Market::MarketAnalyzer analyzer(
            chrono::seconds(0),
            chrono::minutes(5),
            [&](const Kufar::KufarConfiguration &) {
                const int call = ++crawlCalls;
                if (call == 2) {
                    unique_lock<mutex> lock(crawlMutex);
                    crawlCondition.wait(lock, [&] { return releaseSecondCrawl; });
                }
                return makeFetchResult();
            }
        );

        queries::QueryConfiguration query{};
        query.id = 8;
        Kufar::Ad ad{};
        ad.price = 8000;

        optional<Market::PriceAssessment> assessment;
        const auto firstPublishDeadline = chrono::steady_clock::now() + chrono::seconds(2);
        while (!assessment.has_value() && chrono::steady_clock::now() < firstPublishDeadline) {
            assessment = analyzer.rate(query, ad);
            if (!assessment.has_value()) {
                this_thread::sleep_for(chrono::milliseconds(5));
            }
        }
        check(assessment.has_value(), "initial snapshot published", __LINE__);

        const auto refreshDeadline = chrono::steady_clock::now() + chrono::seconds(2);
        while (crawlCalls.load() < 2 && chrono::steady_clock::now() < refreshDeadline) {
            this_thread::sleep_for(chrono::milliseconds(5));
        }
        check(crawlCalls.load() == 2, "stale snapshot schedules refresh", __LINE__);

        for (int index = 0; index < 20; ++index) {
            const auto staleAssessment = analyzer.rate(query, ad);
            check(staleAssessment.has_value(), "stale snapshot remains usable", __LINE__);
            check(close(staleAssessment->deviationPercent, -20.0), "stale snapshot deviation", __LINE__);
        }
        check(crawlCalls.load() == 2, "stale refresh is single-flight", __LINE__);

        {
            lock_guard<mutex> lock(crawlMutex);
            releaseSecondCrawl = true;
        }
        crawlCondition.notify_all();
    }

    void testDeferredRatingValidation() {
        atomic<int> crawlCalls = 0;
        Market::MarketAnalyzer analyzer(
            chrono::hours(1),
            chrono::minutes(5),
            [&](const Kufar::KufarConfiguration &) {
                ++crawlCalls;
                return makeFetchResult();
            }
        );

        queries::QueryConfiguration query{};
        query.id = 9;
        Kufar::Ad ad{};
        ad.price = 8000;
        Kufar::Ad zeroPriced = ad;
        zeroPriced.price = 0;
        Kufar::Ad negativePriced = ad;
        negativePriced.price = -1;

        const auto callback = [](Market::PriceAssessment) {};
        check(
            analyzer.rateDeferred(query, zeroPriced, callback) == Market::DeferredRatingStatus::BadPrice,
            "zero price deferred rating rejected",
            __LINE__
        );
        check(
            analyzer.rateDeferred(query, negativePriced, callback) == Market::DeferredRatingStatus::BadPrice,
            "negative price deferred rating rejected",
            __LINE__
        );
        check(
            analyzer.rateDeferred(query, ad, Market::RatingCallback()) == Market::DeferredRatingStatus::BadCallback,
            "empty callback rejected",
            __LINE__
        );
        check(
            analyzer.rateDeferred(query, ad, callback) == Market::DeferredRatingStatus::NotRefreshing,
            "cold cache without refresh rejected",
            __LINE__
        );
        check(crawlCalls.load() == 0, "invalid deferred ratings do not crawl", __LINE__);
    }

    void testDeferredRatingImmediateAndStaleSnapshot() {
        atomic<int> crawlCalls = 0;
        Market::MarketAnalyzer analyzer(
            chrono::milliseconds(100),
            chrono::minutes(5),
            [&](const Kufar::KufarConfiguration &) {
                ++crawlCalls;
                return makeFetchResult();
            }
        );

        queries::QueryConfiguration query{};
        query.id = 10;
        Kufar::Ad ad{};
        ad.price = 8000;

        optional<Market::PriceAssessment> assessment;
        const auto publishDeadline = chrono::steady_clock::now() + chrono::seconds(2);
        while (!assessment.has_value() && chrono::steady_clock::now() < publishDeadline) {
            assessment = analyzer.rate(query, ad);
            if (!assessment.has_value()) {
                this_thread::sleep_for(chrono::milliseconds(5));
            }
        }
        check(assessment.has_value(), "deferred test snapshot published", __LINE__);

        this_thread::sleep_for(chrono::milliseconds(150));

        atomic<int> callbackCalls = 0;
        atomic<bool> callbackFailed = false;
        const auto status = analyzer.rateDeferred(
            query,
            ad,
            [&](Market::PriceAssessment value) {
                ++callbackCalls;
                callbackFailed = !close(value.deviationPercent, -20.0);
            }
        );
        check(status == Market::DeferredRatingStatus::NotDeferred, "stale snapshot rated immediately", __LINE__);
        check(callbackCalls.load() == 1, "immediate callback invoked once", __LINE__);
        check(!callbackFailed.load(), "immediate callback assessment", __LINE__);

        const auto throwingStatus = analyzer.rateDeferred(
            query,
            ad,
            [](Market::PriceAssessment) {
                throw runtime_error("expected test callback failure");
            }
        );
        check(throwingStatus == Market::DeferredRatingStatus::NotDeferred, "callback exception swallowed", __LINE__);
        check(analyzer.rate(query, ad).has_value(), "analyzer usable after callback failure", __LINE__);
    }

    void testDeferredCallbackFailureIsolation() {
        atomic<int> crawlCalls = 0;
        mutex crawlMutex;
        condition_variable crawlCondition;
        bool releaseCrawl = false;

        Market::MarketAnalyzer analyzer(
            chrono::hours(1),
            chrono::minutes(5),
            [&](const Kufar::KufarConfiguration &) {
                ++crawlCalls;
                unique_lock<mutex> lock(crawlMutex);
                crawlCondition.wait(lock, [&] { return releaseCrawl; });
                return makeFetchResult();
            }
        );

        queries::QueryConfiguration query{};
        query.id = 11;
        Kufar::Ad ad{};
        ad.price = 8000;

        check(!analyzer.rate(query, ad).has_value(), "callback test cold miss", __LINE__);
        const auto crawlDeadline = chrono::steady_clock::now() + chrono::seconds(2);
        while (crawlCalls.load() == 0 && chrono::steady_clock::now() < crawlDeadline) {
            this_thread::sleep_for(chrono::milliseconds(5));
        }
        check(crawlCalls.load() == 1, "callback test crawl started", __LINE__);

        atomic<int> throwingCalls = 0;
        atomic<int> successfulCalls = 0;
        check(
            analyzer.rateDeferred(query, ad, [&](Market::PriceAssessment) {
                ++throwingCalls;
                throw runtime_error("expected worker callback failure");
            }) == Market::DeferredRatingStatus::AddSuccess,
            "throwing callback deferred",
            __LINE__
        );
        check(
            analyzer.rateDeferred(query, ad, [&](Market::PriceAssessment) {
                ++successfulCalls;
            }) == Market::DeferredRatingStatus::AddSuccess,
            "successful callback deferred",
            __LINE__
        );

        {
            lock_guard<mutex> lock(crawlMutex);
            releaseCrawl = true;
        }
        crawlCondition.notify_all();

        const auto callbackDeadline = chrono::steady_clock::now() + chrono::seconds(2);
        while (successfulCalls.load() == 0 && chrono::steady_clock::now() < callbackDeadline) {
            this_thread::sleep_for(chrono::milliseconds(5));
        }
        check(throwingCalls.load() == 1, "throwing callback invoked once", __LINE__);
        check(successfulCalls.load() == 1, "later callback still invoked", __LINE__);
        check(analyzer.rate(query, ad).has_value(), "worker survives callback exception", __LINE__);
    }

    void testFailedRefreshDiscardsDeferredRatings() {
        atomic<int> crawlCalls = 0;
        mutex crawlMutex;
        condition_variable crawlCondition;
        bool releaseFirstCrawl = false;

        Market::MarketAnalyzer analyzer(
            chrono::hours(1),
            chrono::seconds(0),
            [&](const Kufar::KufarConfiguration &) {
                const int call = ++crawlCalls;
                if (call == 1) {
                    unique_lock<mutex> lock(crawlMutex);
                    crawlCondition.wait(lock, [&] { return releaseFirstCrawl; });
                    throw runtime_error("expected test crawl failure");
                }
                return makeFetchResult();
            }
        );

        queries::QueryConfiguration query{};
        query.id = 12;
        Kufar::Ad ad{};
        ad.price = 8000;

        check(!analyzer.rate(query, ad).has_value(), "failure test cold miss", __LINE__);
        const auto crawlDeadline = chrono::steady_clock::now() + chrono::seconds(2);
        while (crawlCalls.load() == 0 && chrono::steady_clock::now() < crawlDeadline) {
            this_thread::sleep_for(chrono::milliseconds(5));
        }
        check(crawlCalls.load() == 1, "failure test crawl started", __LINE__);

        atomic<int> deferredCalls = 0;
        check(
            analyzer.rateDeferred(query, ad, [&](Market::PriceAssessment) {
                ++deferredCalls;
            }) == Market::DeferredRatingStatus::AddSuccess,
            "callback deferred before failed crawl",
            __LINE__
        );

        {
            lock_guard<mutex> lock(crawlMutex);
            releaseFirstCrawl = true;
        }
        crawlCondition.notify_all();

        optional<Market::PriceAssessment> assessment;
        const auto retryDeadline = chrono::steady_clock::now() + chrono::seconds(2);
        while (!assessment.has_value() && chrono::steady_clock::now() < retryDeadline) {
            assessment = analyzer.rate(query, ad);
            if (!assessment.has_value()) {
                this_thread::sleep_for(chrono::milliseconds(5));
            }
        }
        check(assessment.has_value(), "later crawl recovers snapshot", __LINE__);
        check(crawlCalls.load() == 2, "failed crawl retried", __LINE__);

        this_thread::sleep_for(chrono::milliseconds(50));
        check(deferredCalls.load() == 0, "failed crawl discards deferred callback", __LINE__);
    }
}

int main() {
    const vector<pair<string, function<void()>>> tests = {
        {"price statistics", testPriceStatistics},
        {"invalid and zero-IQR samples", testInvalidAndZeroIqrSamples},
        {"price scoring", testPriceScoring},
        {"market cache", testMarketCache},
        {"analyzer cold miss and single-flight", testAnalyzerColdMissAndSingleFlight},
        {"analyzer serves stale during refresh", testAnalyzerServesStaleDuringRefresh},
        {"deferred rating validation", testDeferredRatingValidation},
        {"deferred rating immediate callback", testDeferredRatingImmediateAndStaleSnapshot},
        {"deferred callback failure isolation", testDeferredCallbackFailureIsolation},
        {"failed refresh discards deferred ratings", testFailedRefreshDiscardsDeferredRatings}
    };

    int failures = 0;
    for (const auto &[name, test] : tests) {
        try {
            test();
            cout << "[PASS] " << name << '\n';
        } catch (const exception &exception) {
            ++failures;
            cerr << "[FAIL] " << name << ": " << exception.what() << '\n';
        }
    }

    if (failures > 0) {
        cerr << failures << " test(s) failed.\n";
        return 1;
    }

    cout << tests.size() << " test(s) passed.\n";
    return 0;
}
