#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "kufar.hpp"
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
            {"cct", "11"},
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
            {"computer-component-type", 11},
            {"region", 7},
            {"areas", {22, 23, 30}}
        };
        check(actual == expected, "actual == expected", __LINE__);

        json queryWithID = actual;
        queryWithID["id"] = 1;
        const queries::QueryConfiguration parsed = queries::parseQuery(queryWithID);
        check(parsed.kufar.tag == "iphone", "parsed tag", __LINE__);
        check(parsed.kufar.priceRange.priceMin == 100, "parsed minimum price", __LINE__);
        check(parsed.kufar.priceRange.priceMax == 800, "parsed maximum price", __LINE__);
        check(parsed.kufar.subCategory == 17010, "parsed subcategory", __LINE__);
        check(parsed.kufar.computerComponentType == Kufar::ComputerComponentType::graphicsCard, "parsed component type", __LINE__);
        check(parsed.kufar.areas == vector<int>({22, 23, 30}), "parsed areas", __LINE__);
        check(parsed.kufar.onlyWithVideos == false, "parsed false boolean", __LINE__);

        // Older configuration files may still contain the removed `limit`
        // field; unknown keys are ignored so they keep loading.
        json legacyQuery = queryWithID;
        legacyQuery["limit"] = 25;
        const queries::QueryConfiguration legacyParsed = queries::parseQuery(legacyQuery);
        check(legacyParsed.kufar.tag == "iphone", "legacy limit key is ignored", __LINE__);

        json intervalQuery = queryWithID;
        intervalQuery["poll-interval-seconds"] = 30;
        const queries::QueryConfiguration intervalParsed = queries::parseQuery(intervalQuery);
        check(intervalParsed.pollIntervalSeconds == 30, "per-query interval parsed", __LINE__);
        check(
            intervalParsed.getDescriptionConcise().find("30") != string::npos,
            "per-query interval is described",
            __LINE__
        );

        json invalidInterval = queryWithID;
        invalidInterval["poll-interval-seconds"] = 0;
        expectError(
            [&invalidInterval] { queries::parseQuery(invalidInterval); },
            "poll-interval-seconds"
        );
    }

    void testAllComputerComponentTypes() {
        const vector<pair<int, string>> componentTypes = {
            {1, "Корпуса"},
            {3, "Кулеры"},
            {5, "Блоки питания, аккумуляторы"},
            {7, "Материнские платы"},
            {8, "Ноутбуки на запчасти"},
            {9, "Оперативная память"},
            {10, "Процессоры"},
            {11, "Видеокарты"},
            {13, "Жесткие диски"},
            {15, "SSD"},
            {17, "Звуковые карты"},
            {19, "Оптические приводы"},
            {21, "Прочие компьютерные комплектующие"}
        };

        for (const auto &[id, label] : componentTypes) {
            const json converted = Kufar::convertSearchParameters(
                "https://www.kufar.by/l/videokarty",
                {{"cat", "16010"}, {"cct", to_string(id)}}
            );
            check(converted.at("computer-component-type") == id, "converted component type", __LINE__);

            json queryWithID = converted;
            queryWithID["id"] = 1;
            const queries::QueryConfiguration parsed = queries::parseQuery(queryWithID);
            check(parsed.kufar.computerComponentType.has_value(), "component type is present", __LINE__);
            check(int(parsed.kufar.computerComponentType.value()) == id, "parsed component type ID", __LINE__);

            const auto componentType = static_cast<Kufar::ComputerComponentType>(id);
            check(Kufar::EnumString::computerComponentType(componentType) == label, "component type label", __LINE__);
        }
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

        // The Kufar web UI's page size is not part of the query schema.
        const json ignoredPageSize = Kufar::convertSearchParameters(
            "https://www.kufar.by/l",
            {{"size", "0"}}
        );
        check(!ignoredPageSize.contains("limit"), "page size is ignored", __LINE__);
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
            {{{"cct", "2"}}, "unknown computer component type"},
            {{{"sort", "lst.a"}}, "Unsupported Kufar sort type"},
            {{{"ar", "v.or:22,,30"}}, "integer"},
            {{{"rgn", "8"}}, "unknown region"},
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

    void testSearchURLBuilding() {
        Kufar::KufarConfiguration configuration;
        configuration.tag = "iphone 13";
        configuration.language = "ru";
        configuration.subCategory = 17010;
        configuration.condition = Kufar::ItemCondition::used;
        configuration.areas = vector<int>({22, 23});

        const string firstPage = Kufar::buildSearchURL(configuration, nullopt, 200);
        check(firstPage.find("size=200&") != string::npos, "page size", __LINE__);
        check(firstPage.find("query=iphone%2013&") != string::npos, "encoded query", __LINE__);
        check(firstPage.find("cat=17010&") != string::npos, "subcategory", __LINE__);
        check(firstPage.find("cnd=1&") != string::npos, "condition", __LINE__);
        check(firstPage.find("ar=v.or:22,23&") != string::npos, "areas", __LINE__);
        check(firstPage.find("cursor=") == string::npos, "no cursor on first page", __LINE__);

        const string nextPage = Kufar::buildSearchURL(configuration, string("abc+def/ghi=="), 200);
        check(
            nextPage.find("cursor=abc%2Bdef%2Fghi%3D%3D&") != string::npos,
            "encoded cursor",
            __LINE__
        );
    }

    void testSearchPageParsing() {
        const string rawJson = R"({
            "ads": [
                {
                    "subject": "iPhone 13",
                    "ad_id": 1001,
                    "list_time": "2026-11-09T09:42:49Z",
                    "price_byn": "310000",
                    "phone_hidden": false,
                    "ad_link": "https://www.kufar.by/item/1001",
                    "category": "16010",
                    "account_parameters": [{"p": "name", "v": "Alice"}],
                    "ad_parameters": [
                        {"pl": "Подкатегория", "vl": "Комплектующие", "p": "category", "v": 16010, "pu": "cat"},
                        {"pl": "Тип", "vl": "Видеокарты", "p": "computers_component_type", "v": "11", "pu": "cct"},
                        {"pl": "Графический процессор", "vl": "GeForce RTX 3060", "p": "computers_component_videocard_cpu", "v": "14", "pu": "ccvc"},
                        {"pl": "Состояние", "vl": "Б/у", "p": "condition", "v": "1", "pu": "cnd"}
                    ],
                    "images": []
                },
                {
                    "subject": "Listing without price",
                    "ad_id": 1002,
                    "list_time": "2026-11-09T09:42:49Z",
                    "price_byn": null,
                    "phone_hidden": true,
                    "ad_link": "https://www.kufar.by/item/1002",
                    "ad_parameters": [
                        {"pl": "Подкатегория", "vl": "Комплектующие", "p": "category", "v": "16010", "pu": "cat"}
                    ]
                }
            ],
            "pagination": {
                "pages": [
                    {"label": "self", "num": 1, "token": null},
                    {"label": "next", "num": 2, "token": "next-token"}
                ]
            },
            "total": 1234
        })";

        const Kufar::SearchPage page = Kufar::parseSearchPage(rawJson, string("iphone"));
        check(page.total == 1234, "page total", __LINE__);
        check(page.ads.size() == 2, "page ads", __LINE__);
        check(page.nextCursor.has_value(), "next cursor present", __LINE__);
        check(page.nextCursor.value() == "next-token", "next cursor value", __LINE__);
        check(page.ads[0].tag == string("iphone"), "advert tag", __LINE__);
        check(page.ads[0].id == 1001, "advert id", __LINE__);
        check(page.ads[0].price == 310000, "advert price in kopecks", __LINE__);
        check(page.ads[0].sellerName == "Alice", "advert seller", __LINE__);
        check(page.ads[0].phoneNumberIsVisible, "visible phone", __LINE__);
        check(page.ads[0].subCategory == 16010, "advert subcategory", __LINE__);
        check(page.ads[0].category == 16000, "advert parent category", __LINE__);
        check(page.ads[0].adParameters.size() == 4, "advert parameter count", __LINE__);
        check(page.ads[0].adParameters[1].name == "computers_component_type", "component parameter name", __LINE__);
        check(page.ads[0].adParameters[1].value == "11", "component parameter value", __LINE__);
        check(page.ads[0].adParameters[1].label == "Видеокарты", "component parameter label", __LINE__);
        check(page.ads[0].adParameters[1].unit == "cct", "component parameter unit", __LINE__);
        check(page.ads[1].price == 0, "missing price becomes zero", __LINE__);
        check(!page.ads[1].phoneNumberIsVisible, "hidden phone", __LINE__);
        check(page.ads[1].subCategory == 16010, "subcategory from ad parameters", __LINE__);
        check(page.ads[1].category == 16000, "parent category from ad parameters", __LINE__);
        check(page.ads[1].adParameters.size() == 1, "fallback parameter count", __LINE__);

        const Kufar::SearchPage empty = Kufar::parseSearchPage(
            R"({"ads":[],"pagination":null,"total":0})"
        );
        check(empty.total == 0, "empty total", __LINE__);
        check(empty.ads.empty(), "empty ads", __LINE__);
        check(!empty.nextCursor.has_value(), "empty next cursor", __LINE__);

        expectError(
            [] { Kufar::parseSearchPage(R"({"total":0})"); },
            "ads array"
        );
        expectError(
            [] { Kufar::parseSearchPage("not-json"); },
            "parse"
        );
    }

    void testPageTimestampBoundary() {
        Kufar::SearchPage page;

        Kufar::Ad older{};
        older.date = 100;
        Kufar::Ad newer{};
        newer.date = 200;
        page.ads = {newer, older}; // newest-first, as Kufar returns them

        check(!Kufar::pagePassedTimestamp(page, 50), "window still above the page", __LINE__);
        check(!Kufar::pagePassedTimestamp(page, 100), "equal timestamp keeps paginating", __LINE__);
        check(Kufar::pagePassedTimestamp(page, 101), "oldest ad is below the boundary", __LINE__);
        check(Kufar::pagePassedTimestamp(page, 150), "boundary inside the page", __LINE__);
        check(!Kufar::pagePassedTimestamp(Kufar::SearchPage{}, 0), "empty page", __LINE__);
    }
}

int main() {
    const vector<pair<string, function<void()>>> tests = {
        {"extract search parameters", testExtraction},
        {"convert all supported parameters and parse legacy query", testCompleteConversionAndLegacyParser},
        {"convert every computer component type", testAllComputerComponentTypes},
        {"convert optional values", testOptionalValues},
        {"strictly reject unsupported and invalid parameters", testStrictRejection},
        {"validate URLs before fetching", testURLValidation},
        {"build search request URLs", testSearchURLBuilding},
        {"parse a search page", testSearchPageParsing},
        {"stop pagination at a timestamp boundary", testPageTimestampBoundary}
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
