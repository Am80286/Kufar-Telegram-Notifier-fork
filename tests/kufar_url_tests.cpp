#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "kufar_url.hpp"
#include "queries.hpp"

using nlohmann::json;
using namespace std;

namespace {
    void check(bool condition, const char *expression, int line) {
        if (!condition) {
            throw runtime_error("Check failed at line " + to_string(line) + ": " + expression);
        }
    }

    void expectError(const function<void()> &action, const string &messagePart) {
        try {
            action();
        } catch (const runtime_error &exception) {
            check(
                string(exception.what()).find(messagePart) != string::npos,
                "exception contains expected text",
                __LINE__
            );
            return;
        }
        throw runtime_error("Expected an exception containing: " + messagePart);
    }

    void testExtraction() {
        const string html =
            "<html><head></head><body>"
            "<script>const marker = 'unrelated';</script>"
            "<script type=\"application/json\" id=\"__NEXT_DATA__\">"
            "{\"props\":{},\"query\":{\"cat\":\"17010\",\"rgn\":\"7\",\"query\":\"iphone\"}}"
            "</script></body></html>";

        const json parameters = Kufar::extractSearchParameters(html);
        check(parameters == json({{"cat", "17010"}, {"rgn", "7"}, {"query", "iphone"}}), "parameters", __LINE__);
        expectError([] { Kufar::extractSearchParameters("<html></html>"); }, "__NEXT_DATA__");
        expectError(
            [] { Kufar::extractSearchParameters("<script id=\"__NEXT_DATA__\">not-json</script>"); },
            "Unable to parse"
        );
        expectError(
            [] { Kufar::extractSearchParameters("<script id=\"__NEXT_DATA__\">{\"props\":{}}</script>"); },
            "resolved search query"
        );
    }

    void testCompleteConversionAndLegacyParser() {
        const string source = "https://www.kufar.by/l/r~minsk/mobilnye-telefony?query=iphone";
        const json parameters = {
            {"query", "iphone"},
            {"ot", "1"},
            {"prc", "r:10000,80000"},
            {"lang", "ru"},
            {"size", "25"},
            {"cur", "BYR"},
            {"cnd", "1"},
            {"cmp", "0"},
            {"dle", "1"},
            {"sde", "0"},
            {"hlv", "1"},
            {"oph", "1"},
            {"ovi", "0"},
            {"pse", "1"},
            {"sort", "prs.a"},
            {"cat", "17010"},
            {"prn", "17000"},
            {"rgn", "7"},
            {"ar", "v.or:22,23,30"},
            {"cursor", "page-token"},
            {"elementType", "popular_categories"},
            {"etext", ""},
            {"utm_source", "test"}
        };

        const json actual = Kufar::convertSearchParameters(source, parameters);
        const json expected = {
            {"source-url", source},
            {"tag", "iphone"},
            {"only-title-search", true},
            {"price", {{"min", 100}, {"max", 800}}},
            {"language", "ru"},
            {"limit", 25},
            {"currency", "BYR"},
            {"condition", 1},
            {"seller-type", 0},
            {"kufar-delivery-required", true},
            {"kufar-payment-required", false},
            {"kufar-halva-required", true},
            {"only-with-photos", true},
            {"only-with-videos", false},
            {"only-with-exchange-available", true},
            {"sort-type", 2},
            {"sub-category", 17010},
            {"category", 17000},
            {"region", 7},
            {"areas", {22, 23, 30}}
        };
        check(actual == expected, "actual == expected", __LINE__);

        const queries::QueryConfiguration parsed = queries::parseQuery(actual);
        check(parsed.kufar.tag == "iphone", "parsed tag", __LINE__);
        check(parsed.kufar.priceRange.priceMin == 100, "parsed minimum price", __LINE__);
        check(parsed.kufar.priceRange.priceMax == 800, "parsed maximum price", __LINE__);
        check(parsed.kufar.subCategory == 17010, "parsed subcategory", __LINE__);
        check(parsed.kufar.areas == vector<int>({22, 23, 30}), "parsed areas", __LINE__);
        check(parsed.kufar.onlyWithVideos == false, "parsed false boolean", __LINE__);
    }

    void testOptionalValues() {
        const json converted = Kufar::convertSearchParameters(
            "https://re.kufar.by/l/minsk/kupit/kvartiru",
            {
                {"query", ""},
                {"lang", ""},
                {"cur", ""},
                {"prc", "r:,50000"},
                {"sort", "lst.d"},
                {"rgn", "all"},
                {"ar", "158"}
            }
        );

        check(!converted.contains("tag"), "empty tag omitted", __LINE__);
        check(!converted.contains("language"), "empty language omitted", __LINE__);
        check(!converted.contains("currency"), "empty currency omitted", __LINE__);
        check(!converted.contains("sort-type"), "default sort omitted", __LINE__);
        check(!converted.contains("region"), "all regions omitted", __LINE__);
        check(converted.at("price") == json({{"max", 500}}), "open price range", __LINE__);
        check(converted.at("areas") == json::array({158}), "single area", __LINE__);
    }

    void testStrictRejection() {
        expectError(
            [] {
                Kufar::convertSearchParameters(
                    "https://www.kufar.by/l/test",
                    {{"cat", "17010"}, {"pb", "5"}, {"phm", "v.or:5625"}}
                );
            },
            "pb, phm"
        );

        const vector<pair<json, string>> invalidParameters = {
            {{{"prc", "r:10050,80000"}}, "whole BYN"},
            {{{"prc", "r:90000,10000"}}, "minimum price"},
            {{{"cnd", "3"}}, "either 1 or 2"},
            {{{"cmp", "2"}}, "either 0 or 1"},
            {{{"sort", "lst.a"}}, "Unsupported Kufar sort type"},
            {{{"ar", "v.or:22,,30"}}, "integer"},
            {{{"rgn", "8"}}, "unknown region"},
            {{{"size", "0"}}, "must be positive"},
            {{{"ot", "true"}}, "either 0 or 1"}
        };

        for (const auto &[parameters, message] : invalidParameters) {
            expectError(
                [&parameters] {
                    Kufar::convertSearchParameters("https://www.kufar.by/l", parameters);
                },
                message
            );
        }
    }

    void testURLValidation() {
        expectError([] { Kufar::importSearchQuery("not-a-url"); }, "Invalid Kufar search URL");
        expectError(
            [] { Kufar::importSearchQuery("http://www.kufar.by/l?query=iphone"); },
            "must use HTTPS"
        );
        expectError(
            [] { Kufar::importSearchQuery("https://example.com/l?query=iphone"); },
            "Unsupported Kufar host"
        );
        expectError(
            [] { Kufar::importSearchQuery("https://www.kufar.by/item/12345"); },
            "search page"
        );
    }
}

int main() {
    const vector<pair<string, function<void()>>> tests = {
        {"extract search parameters", testExtraction},
        {"convert all supported parameters and parse legacy query", testCompleteConversionAndLegacyParser},
        {"convert optional values", testOptionalValues},
        {"strictly reject unsupported and invalid parameters", testStrictRejection},
        {"validate URLs before fetching", testURLValidation}
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
