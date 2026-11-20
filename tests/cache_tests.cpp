#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cache.hpp"
#include "helperfunctions.hpp"

using namespace std;
using nlohmann::json;

namespace {
    void check(bool condition, const char *expression, int line) {
        if (!condition) {
            throw runtime_error("Check failed at line " + to_string(line) + ": " + expression);
        }
    }

    const string TEST_CACHE_PATH =
        (std::filesystem::temp_directory_path() / "kufar-cache-tests.json").string();

    cache::CacheStore makeStore(const json &document = cache::CacheStore::emptyDocument()) {
        return cache::CacheStore(TEST_CACHE_PATH, document);
    }

    void testClaimsAndDelivered() {
        cache::CacheStore store = makeStore({
            {"version", 2},
            {"buckets", {{"default", {1, 2}}, {"42", {3}}}}
        });

        check(!store.tryClaim("default", 1), "delivered ads cannot be claimed", __LINE__);
        check(store.tryClaim("default", 9), "new ad is claimed", __LINE__);
        check(!store.tryClaim("default", 9), "pending ads cannot be claimed twice", __LINE__);

        store.markDelivered("default", 9);
        check(!store.tryClaim("default", 9), "delivered claim stays delivered", __LINE__);

        check(store.tryClaim("42", 1), "the same ad is independent per topic", __LINE__);
        store.releaseClaim("42", 1);
        check(store.tryClaim("42", 1), "released claim can be claimed again", __LINE__);
    }

    void testFrontiersAndSerialization() {
        cache::CacheStore store = makeStore();

        check(!store.getFrontier(7).has_value(), "new store has no frontier", __LINE__);

        store.setFrontier(7, cache::FrontierState{1000, 900});
        store.tryClaim("default", 11);
        store.markDelivered("default", 11);

        const json document = store.toJSON();
        check(document.at("version") == cache::CacheStore::VERSION, "version", __LINE__);
        check(document.at("buckets").at("default") == json::array({11}), "delivered bucket", __LINE__);
        check(document.at("frontiers").at("7").at("frontier") == 1000, "frontier", __LINE__);
        check(document.at("frontiers").at("7").at("suppress-before") == 900, "suppress cut", __LINE__);

        cache::CacheStore restored = makeStore(document);
        const auto frontier = restored.getFrontier(7);
        check(frontier.has_value(), "restored frontier", __LINE__);
        check(frontier->frontier == 1000, "restored frontier value", __LINE__);
        check(frontier->suppressBefore == 900, "restored suppress value", __LINE__);
        check(!restored.tryClaim("default", 11), "restored delivered ad", __LINE__);
        check(restored.tryClaim("default", 12), "restored store can claim new ads", __LINE__);
    }

    void testSaveAndReload() {
        std::remove(TEST_CACHE_PATH.c_str());

        {
            cache::CacheStore store = makeStore();
            store.tryClaim("default", 100);
            store.markDelivered("default", 100);
            store.setFrontier(3, cache::FrontierState{123, 120});
            store.saveNow();
        }

        const json document = json::parse(getTextFromFile(TEST_CACHE_PATH));
        check(document.at("version") == cache::CacheStore::VERSION, "saved version", __LINE__);

        cache::CacheStore restored = makeStore(document);
        check(!restored.tryClaim("default", 100), "saved delivered ad", __LINE__);
        const auto frontier = restored.getFrontier(3);
        check(frontier.has_value() && frontier->frontier == 123, "saved frontier", __LINE__);

        std::remove(TEST_CACHE_PATH.c_str());
    }

    void testLegacyAndVersionValidation() {
        cache::CacheStore legacy = makeStore(json::array());
        check(legacy.toJSON().at("buckets").empty(), "flat legacy cache is ignored", __LINE__);

        try {
            makeStore({{"version", cache::CacheStore::VERSION + 1}, {"buckets", json::object()}});
            throw runtime_error("future cache version was accepted");
        } catch (const runtime_error &exc) {
            check(string(exc.what()).find("более новой версией") != string::npos, "future version error", __LINE__);
        }
    }
}

int main() {
    const vector<pair<string, function<void()>>> tests = {
        {"claims and delivered ads", testClaimsAndDelivered},
        {"frontiers and serialization", testFrontiersAndSerialization},
        {"save and reload", testSaveAndReload},
        {"legacy and version validation", testLegacyAndVersionValidation}
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
