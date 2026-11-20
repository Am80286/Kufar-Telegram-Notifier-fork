#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cache.hpp"
#include "delivery.hpp"

using namespace std;
using namespace std::chrono_literals;
using nlohmann::json;

namespace {
    void check(bool condition, const char *expression, int line) {
        if (!condition) {
            throw runtime_error("Check failed at line " + to_string(line) + ": " + expression);
        }
    }

    const string TEST_CACHE_PATH =
        (std::filesystem::temp_directory_path() / "kufar-delivery-tests.json").string();

    bool delivered(cache::CacheStore &store, const string &topic, int id) {
        const json document = store.toJSON();
        const auto topicIt = document.at("buckets").find(topic);
        if (topicIt == document.at("buckets").end()) {
            return false;
        }

        for (const auto &value : *topicIt) {
            if (value.get<int>() == id) {
                return true;
            }
        }
        return false;
    }

    bool waitFor(const function<bool()> &predicate, chrono::milliseconds timeout = 5s) {
        const auto deadline = chrono::steady_clock::now() + timeout;
        while (chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            this_thread::sleep_for(5ms);
        }
        return predicate();
    }

    delivery::DeliveryItem makeItem(int id, const string &topic = "default") {
        delivery::DeliveryItem item;
        item.queryID = 1;
        item.cacheKey = topic;
        item.ad.id = id;
        item.ad.title = "test listing";
        return item;
    }

    delivery::DeliveryOptions fastOptions() {
        delivery::DeliveryOptions options;
        options.messagesPerMinute = 0;
        options.retryBackoff = chrono::milliseconds(0);
        return options;
    }

    void testSuccessfulDelivery() {
        cache::CacheStore store(TEST_CACHE_PATH, cache::CacheStore::emptyDocument());
        atomic<int> calls{0};
        mutex orderMutex;
        vector<int> order;
        delivery::DeliveryOptions options = fastOptions();

        delivery::DeliveryWorker worker(
            [&](const delivery::DeliveryItem &item) {
                ++calls;
                lock_guard<mutex> lock(orderMutex);
                order.push_back(item.ad.id);
            },
            store,
            [](queries::QueryID) { return true; },
            options
        );

        check(store.tryClaim("default", 1), "claim before enqueue", __LINE__);
        check(worker.enqueue(makeItem(1)), "enqueue", __LINE__);
        check(waitFor([&] { return delivered(store, "default", 1); }), "delivered", __LINE__);

        check(calls == 1, "send called once", __LINE__);
        check(order == vector<int>({1}), "delivery order", __LINE__);
        check(!store.tryClaim("default", 1), "delivered ad stays claimed", __LINE__);

        worker.stopAndDrain(0ms);
    }

    void testTransientRetry() {
        cache::CacheStore store(TEST_CACHE_PATH, cache::CacheStore::emptyDocument());
        atomic<int> attempts{0};
        delivery::DeliveryOptions options = fastOptions();
        options.maxAttempts = 3;

        delivery::DeliveryWorker worker(
            [&](const delivery::DeliveryItem &) {
                if (++attempts == 1) {
                    throw delivery::SendException(
                        delivery::SendFailureKind::Transient,
                        "temporary failure"
                    );
                }
            },
            store,
            [](queries::QueryID) { return true; },
            options
        );

        check(store.tryClaim("default", 2), "claim before enqueue", __LINE__);
        check(worker.enqueue(makeItem(2)), "enqueue", __LINE__);
        check(waitFor([&] { return delivered(store, "default", 2); }), "delivered after retry", __LINE__);
        check(attempts == 2, "retried once", __LINE__);

        worker.stopAndDrain(0ms);
    }

    void testPermanentFailureReleasesClaim() {
        cache::CacheStore store(TEST_CACHE_PATH, cache::CacheStore::emptyDocument());
        atomic<int> attempts{0};

        delivery::DeliveryWorker worker(
            [&](const delivery::DeliveryItem &) {
                ++attempts;
                throw delivery::SendException(
                    delivery::SendFailureKind::Permanent,
                    "bad media"
                );
            },
            store,
            [](queries::QueryID) { return true; },
            fastOptions()
        );

        check(store.tryClaim("default", 3), "claim before enqueue", __LINE__);
        check(worker.enqueue(makeItem(3)), "enqueue", __LINE__);

        const bool released = waitFor([&] {
            if (store.tryClaim("default", 3)) {
                store.releaseClaim("default", 3);
                return true;
            }
            return false;
        });

        check(released, "claim released after permanent failure", __LINE__);
        check(attempts == 1, "permanent failure is not retried", __LINE__);
        check(!delivered(store, "default", 3), "failed ad is not marked delivered", __LINE__);

        worker.stopAndDrain(0ms);
    }

    void testRateLimitRetry() {
        cache::CacheStore store(TEST_CACHE_PATH, cache::CacheStore::emptyDocument());
        atomic<int> attempts{0};

        delivery::DeliveryWorker worker(
            [&](const delivery::DeliveryItem &) {
                if (++attempts == 1) {
                    throw delivery::SendException(
                        delivery::SendFailureKind::RateLimited,
                        "flood",
                        0s
                    );
                }
            },
            store,
            [](queries::QueryID) { return true; },
            fastOptions()
        );

        check(store.tryClaim("default", 4), "claim before enqueue", __LINE__);
        check(worker.enqueue(makeItem(4)), "enqueue", __LINE__);
        check(waitFor([&] { return delivered(store, "default", 4); }), "delivered after 429", __LINE__);
        check(attempts == 2, "retried after rate limit", __LINE__);

        worker.stopAndDrain(0ms);
    }

    void testStopAndDrain() {
        cache::CacheStore store(TEST_CACHE_PATH, cache::CacheStore::emptyDocument());
        atomic<int> calls{0};

        delivery::DeliveryWorker worker(
            [&](const delivery::DeliveryItem &) { ++calls; },
            store,
            [](queries::QueryID) { return true; },
            fastOptions()
        );

        check(store.tryClaim("default", 6), "claim first", __LINE__);
        check(store.tryClaim("default", 7), "claim second", __LINE__);
        check(worker.enqueue(makeItem(6)), "enqueue first", __LINE__);
        check(worker.enqueue(makeItem(7)), "enqueue second", __LINE__);

        worker.stopAndDrain(2s);

        check(calls == 2, "queued items were drained", __LINE__);
        check(delivered(store, "default", 6), "first drained item delivered", __LINE__);
        check(delivered(store, "default", 7), "second drained item delivered", __LINE__);
        check(!worker.enqueue(makeItem(8)), "enqueue is rejected after stop", __LINE__);
    }

    void testDeletedQueryIsSkipped() {
        cache::CacheStore store(TEST_CACHE_PATH, cache::CacheStore::emptyDocument());
        atomic<int> calls{0};

        delivery::DeliveryWorker worker(
            [&](const delivery::DeliveryItem &) { ++calls; },
            store,
            [](queries::QueryID) { return false; },
            fastOptions()
        );

        check(store.tryClaim("default", 5), "claim before enqueue", __LINE__);
        check(worker.enqueue(makeItem(5)), "enqueue", __LINE__);

        const bool released = waitFor([&] {
            if (store.tryClaim("default", 5)) {
                store.releaseClaim("default", 5);
                return true;
            }
            return false;
        });

        check(released, "claim released for a deleted query", __LINE__);
        check(calls == 0, "deleted query was not sent", __LINE__);
        check(!delivered(store, "default", 5), "deleted query was not cached", __LINE__);

        worker.stopAndDrain(0ms);
    }
}

int main() {
    const vector<pair<string, function<void()>>> tests = {
        {"successful delivery", testSuccessfulDelivery},
        {"transient retry", testTransientRetry},
        {"permanent failure releases claim", testPermanentFailureReleasesClaim},
        {"rate limit retry", testRateLimitRetry},
        {"stop and drain", testStopAndDrain},
        {"deleted query is skipped", testDeletedQueryIsSkipped}
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
