#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cache.hpp"
#include "scheduler.hpp"

using namespace std;
using namespace std::chrono_literals;
using nlohmann::json;

namespace {
    using Clock = chrono::steady_clock;

    void check(bool condition, const char *expression, int line) {
        if (!condition) {
            throw runtime_error("Check failed at line " + to_string(line) + ": " + expression);
        }
    }

    const string CONFIG_PATH =
        (std::filesystem::temp_directory_path() / "kufar-scheduler-tests-config.json").string();
    const string CACHE_PATH =
        (std::filesystem::temp_directory_path() / "kufar-scheduler-tests-cache.json").string();

    Kufar::Ad makeAd(int id, time_t date) {
        Kufar::Ad ad{};
        ad.id = id;
        ad.date = date;
        ad.title = "test listing";
        return ad;
    }

    struct FakeSource {
        vector<Kufar::Ad> cold;
        vector<Kufar::Ad> warm;
        int coldCalls = 0;
        int warmCalls = 0;

        vector<Kufar::Ad> fetchNewest(int limit) {
            ++coldCalls;
            vector<Kufar::Ad> result;
            for (const Kufar::Ad &ad : cold) {
                if (static_cast<int>(result.size()) >= limit) {
                    break;
                }
                result.push_back(ad);
            }
            return result;
        }

        Kufar::FetchResult fetchWindow() {
            ++warmCalls;
            Kufar::FetchResult result;
            result.ads = warm;
            result.pagesFetched = 1;
            return result;
        }
    };

    struct FakeSink : delivery::DeliverySink {
        vector<delivery::DeliveryItem> items;

        bool enqueue(delivery::DeliveryItem item) override {
            items.push_back(std::move(item));
            return true;
        }
    };

    queries::QueryStore makeQueryStore() {
        json document = {{"queries", json::array()}};
        document["queries"].push_back({{"id", 1}, {"tag", "test"}});
        return queries::QueryStore(CONFIG_PATH, document);
    }

    scheduling::SchedulerConfiguration makeConfiguration() {
        scheduling::SchedulerConfiguration configuration;
        configuration.defaultInterval = 60s;
        configuration.minInterval = 60s;
        configuration.maxInterval = 300s;
        configuration.jitterPercent = 0;
        configuration.coldStartListingLimit = 10;
        configuration.maxPages = 10;
        configuration.frontierOverlapSeconds = 600;
        return configuration;
    }

    struct Fixture {
        queries::QueryStore store;
        cache::CacheStore cache;
        FakeSink sink;
        FakeSource source;
        scheduling::PollingScheduler scheduler;
        Clock::time_point now;

        Fixture()
            : store(makeQueryStore()),
              cache(CACHE_PATH, cache::CacheStore::emptyDocument()),
              scheduler(
                  store,
                  cache,
                  sink,
                  makeConfiguration(),
                  [this](const Kufar::KufarConfiguration &, int limit) {
                      return source.fetchNewest(limit);
                  },
                  [this](const Kufar::KufarConfiguration &, const Kufar::FetchOptions &) {
                      return source.fetchWindow();
                  },
                  [this] { return now; },
                  [] { return false; }
              ),
              now(Clock::time_point{} + chrono::seconds(10000)) {
        }
    };

    void testColdStartAndAdaptiveBackoff() {
        Fixture fixture;
        fixture.source.cold = {makeAd(1, 1000), makeAd(2, 1100), makeAd(3, 1200)};

        fixture.scheduler.reconcile(fixture.now);
        fixture.scheduler.stepOnce(fixture.now);

        check(fixture.source.coldCalls == 1, "cold fetch called", __LINE__);
        check(fixture.sink.items.size() == 3, "cold listings enqueued", __LINE__);

        auto schedule = fixture.scheduler.scheduleFor(1);
        check(schedule.has_value(), "schedule created", __LINE__);
        check(schedule->initialized, "schedule initialized", __LINE__);
        check(schedule->frontier == 1200, "cold frontier", __LINE__);
        check(schedule->suppressBefore == 1200, "cold suppress cut", __LINE__);
        check(schedule->currentInterval == 60s, "initial interval", __LINE__);

        const auto persisted = fixture.cache.getFrontier(1);
        check(persisted.has_value() && persisted->frontier == 1200, "frontier persisted", __LINE__);

        // Empty poll grows the interval.
        fixture.source.warm = {};
        fixture.now = schedule->nextDue;
        fixture.scheduler.stepOnce(fixture.now);

        schedule = fixture.scheduler.scheduleFor(1);
        check(fixture.source.warmCalls == 1, "warm fetch called", __LINE__);
        check(schedule->currentInterval == 90s, "empty poll backoff", __LINE__);
    }

    void testHitResetsAndFrontierAdvances() {
        Fixture fixture;
        fixture.source.cold = {makeAd(1, 1000), makeAd(2, 1100)};

        fixture.scheduler.reconcile(fixture.now);
        fixture.scheduler.stepOnce(fixture.now);

        auto schedule = fixture.scheduler.scheduleFor(1);
        fixture.source.warm = {makeAd(3, 1300)};
        fixture.now = schedule->nextDue;
        fixture.scheduler.stepOnce(fixture.now);

        schedule = fixture.scheduler.scheduleFor(1);
        check(fixture.sink.items.size() == 3, "new warm listing enqueued", __LINE__);
        check(fixture.sink.items.back().ad.id == 3, "warm listing id", __LINE__);
        check(schedule->frontier == 1300, "frontier advanced", __LINE__);
        check(schedule->currentInterval == 60s, "hit resets interval", __LINE__);

        // The same listing in the overlap window is not enqueued twice.
        fixture.source.warm = {makeAd(3, 1300)};
        fixture.now = schedule->nextDue;
        fixture.scheduler.stepOnce(fixture.now);

        check(fixture.sink.items.size() == 3, "duplicate claim skipped", __LINE__);
    }

    void testBackoffCap() {
        Fixture fixture;
        fixture.source.cold = {makeAd(1, 1000)};

        fixture.scheduler.reconcile(fixture.now);
        fixture.scheduler.stepOnce(fixture.now);

        fixture.source.warm = {};
        for (int index = 0; index < 8; index++) {
            const auto schedule = fixture.scheduler.scheduleFor(1);
            fixture.now = schedule->nextDue;
            fixture.scheduler.stepOnce(fixture.now);
        }

        const auto schedule = fixture.scheduler.scheduleFor(1);
        check(schedule->currentInterval == 300s, "backoff capped", __LINE__);
    }

    void testFrontierSurvivesNewScheduler() {
        Fixture fixture;
        fixture.source.cold = {makeAd(1, 1000), makeAd(2, 1500)};

        fixture.scheduler.reconcile(fixture.now);
        fixture.scheduler.stepOnce(fixture.now);

        FakeSink secondSink;
        FakeSource secondSource;
        scheduling::PollingScheduler secondScheduler(
            fixture.store,
            fixture.cache,
            secondSink,
            makeConfiguration(),
            [&](const Kufar::KufarConfiguration &, int limit) {
                return secondSource.fetchNewest(limit);
            },
            [&](const Kufar::KufarConfiguration &, const Kufar::FetchOptions &) {
                return secondSource.fetchWindow();
            },
            [&] { return fixture.now; },
            [] { return false; }
        );

        secondScheduler.reconcile(fixture.now);
        const auto schedule = secondScheduler.scheduleFor(1);
        check(schedule.has_value(), "restored schedule", __LINE__);
        check(schedule->initialized, "restored schedule is warm", __LINE__);
        check(schedule->frontier == 1500, "restored frontier", __LINE__);
        check(secondSource.coldCalls == 0, "no cold fetch after restore", __LINE__);
    }

    void testQueryRemovalAndAddition() {
        Fixture fixture;
        fixture.source.cold = {makeAd(1, 1000)};

        fixture.scheduler.reconcile(fixture.now);
        fixture.scheduler.stepOnce(fixture.now);
        check(fixture.scheduler.scheduleFor(1).has_value(), "initial schedule", __LINE__);

        fixture.store.remove(1);
        fixture.scheduler.reconcile(fixture.now);
        check(!fixture.scheduler.scheduleFor(1).has_value(), "removed schedule dropped", __LINE__);

        json newQuery = {{"tag", "second"}};
        fixture.store.add(newQuery);
        fixture.scheduler.reconcile(fixture.now);
        check(fixture.scheduler.scheduleFor(newQuery.at("id").get<queries::QueryID>()).has_value(), "added schedule", __LINE__);
    }
}

int main() {
    const vector<pair<string, function<void()>>> tests = {
        {"cold start and adaptive backoff", testColdStartAndAdaptiveBackoff},
        {"hit resets interval and advances frontier", testHitResetsAndFrontierAdvances},
        {"backoff cap", testBackoffCap},
        {"frontier survives new scheduler", testFrontierSurvivesNewScheduler},
        {"query removal and addition", testQueryRemovalAndAddition}
    };

    int failures = 0;
    for (const auto &[name, test] : tests) {
        try {
            test();
            cout << "[PASS] " << name << '\n';
        } catch (const exception &exception) {
            failures++;
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
