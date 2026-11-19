//
//  main.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 02.06.2022.
//

#include <algorithm>
#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "spdlog/spdlog.h"
#include "spdlog/cfg/env.h"

#include <nlohmann/json.hpp>
#include "kufar.hpp"
#include "telegram.hpp"
#include "networking.hpp"
#include "helperfunctions.hpp"
#include "logging.hpp"
#include "arguments.hpp"
#include "queries.hpp"
#include "market.hpp"

using namespace std;
using namespace Kufar;
using namespace Telegram;
using namespace queries;
using nlohmann::json;

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

const string DEFAULT_CACHE_FILE_NAME = "cached-data.json";
const string DEFAULT_CONFIGURATION_FILE_NAME = "kufar-configuration.json";
const string DEFAULT_LOG_FILE_NAME = "kufar-notifier.log";

constexpr auto CACHE_SAVE_INTERVAL = 120s;

constexpr int COLD_START_LISTING_LIMIT = 10;
constexpr int POLL_MAX_PAGES = 10;
constexpr time_t POLL_FRONTIER_OVERLAP_SECONDS = 600;

const int CACHE_VERSION = 2;

static volatile sig_atomic_t gStop = 0;
static void onSignal(int) { 
    gStop = 1; 
}

struct ConfigurationFile {
    string path;
    json contents;
};

struct CacheFile {
    string path;
    json contents;
};

struct Files {
    ConfigurationFile configuration;
    CacheFile cache;
};

using CacheBuckets = map<string, unordered_set<int>>;

struct ProgramConfiguration {
    TelegramConfiguration telegramConfiguration;
    Files files;
    
    int queryDelaySeconds = 5;
    int loopDelaySeconds = 30;
};

void loadJSONConfigurationData(const json &data, ProgramConfiguration &programConfiguration) {
    {
        json telegramData = data.at("telegram");
        programConfiguration.telegramConfiguration.botToken = telegramData.at("bot-token");
        programConfiguration.telegramConfiguration.chatID = telegramData.at("chat-id");
    }
    {
        if (data.contains("delays")) {
            json delaysData = data.at("delays");
            programConfiguration.queryDelaySeconds = delaysData.at("query");
            programConfiguration.loopDelaySeconds = delaysData.at("loop");
        }
    }
}

void printJSONConfigurationData(const ProgramConfiguration &programConfiguration) {
    spdlog::info(
        "\n- Telegram:\n"
        "\t- Токен: {}\n"
        "\t- ID Чата: {}\n",
        maskToken(programConfiguration.telegramConfiguration.botToken),
        programConfiguration.telegramConfiguration.chatID
    );
    
    spdlog::info(
        "\n- Задержки:\n"
        "\t- Перед новым запросом: {}с. \n"
        "\t- После прохода всего списка запросов: {}c.",
        programConfiguration.queryDelaySeconds,
        programConfiguration.loopDelaySeconds
    );
}

json getJSONDataFromPath(const string &JSONFilePath) {
    spdlog::info("[Загрузка файла]: \"{}\"", JSONFilePath);

    if (!fileExists(JSONFilePath)){
        throw runtime_error("Файл не существует по данному пути или к нему нет доступа: \"" + JSONFilePath + "\"");
    }
    
    if (getFileSize(JSONFilePath) > 4000000) {
        throw runtime_error("Размер файла превышает 4МБ: \"" + JSONFilePath + "\"");
    }
        
    try {
       json textFromFile = json::parse(getTextFromFile(JSONFilePath));
       return textFromFile;
    } catch (const exception &exc) {
       throw runtime_error("Невозможно получить данные из файла \"" + JSONFilePath + "\": " + exc.what());
    }

}

/**
  Cache representation in JSON:
  {
      "version": 2,
      "buckets": {
          "default": [1, 2, 3],
          "42": [1, 2]
      }
  }
 */
json cacheBucketsToJSON(const CacheBuckets &cacheBuckets) {
    json bucketsData = json::object();
    for (const auto &[cacheKey, adIDs] : cacheBuckets) {
        bucketsData[cacheKey] = adIDs;
    }

    return json{
        {"version", CACHE_VERSION},
        {"buckets", bucketsData}
    };
}

CacheBuckets loadCacheBuckets(const json &data) {
    CacheBuckets cacheBuckets;

    if (data.is_array()) {
        if (!data.empty()) {
            spdlog::warn(
                "[Кеш] Обнаружен устаревший формат кеша (плоский массив). "
                "Идентификаторы проигнорированы, кеш будет создан заново."
            );
        }
        return cacheBuckets;
    }

    if (!data.is_object() || !data.contains("buckets")) {
        throw runtime_error("Некорректный формат файла кеша: ожидается объект с полем \"buckets\".");
    }

    const int version = data.value("version", CACHE_VERSION);
    if (version > CACHE_VERSION) {
        throw runtime_error("Файл кеша создан более новой версией программы (version " + to_string(version) + ").");
    }

    const json &bucketsData = data.at("buckets");
    if (!bucketsData.is_object()) {
        throw runtime_error("Некорректный формат файла кеша: поле \"buckets\" должно быть объектом.");
    }

    for (const auto &[cacheKey, adIDs] : bucketsData.items()) {
        cacheBuckets[cacheKey] = adIDs.get<unordered_set<int>>();
    }

    return cacheBuckets;
}

void createEmptyCacheFile(const string &path) {
    spdlog::info("[Кеш] Файл не найден, создаю новый: \"{}\"", path);

    ofstream cacheFile(path, ofstream::trunc);
    if (!cacheFile) {
        throw runtime_error("Не удалось создать файл кеша: \"" + path + "\"");
    }
    cacheFile << cacheBucketsToJSON(CacheBuckets()).dump();
    cacheFile.close();

    if (!cacheFile) {
        throw runtime_error("Не удалось записать файл кеша: \"" + path + "\"");
    }
}

Files loadFiles(const arguments::Options &options) {
    Files files;
    files.configuration.path = options.configPath;
    files.cache.path = options.cachePath;

    files.configuration.contents = getJSONDataFromPath(files.configuration.path);

    if (!fileExists(files.cache.path)) {
        createEmptyCacheFile(files.cache.path);
    }
    files.cache.contents = getJSONDataFromPath(files.cache.path);

    return files;
}

int main(int argc, char **argv) {
    int exitCode = 0;
    optional<arguments::Options> parsedOptions = arguments::parse(argc, argv, exitCode);
    if (!parsedOptions.has_value()) {
        return exitCode;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    arguments::Options options = parsedOptions.value();

    // By default, files are looked up in the current working directory.
    if (options.configPath.empty()) {
        options.configPath = DEFAULT_CONFIGURATION_FILE_NAME;
    }
    if (options.cachePath.empty()) {
        options.cachePath = DEFAULT_CACHE_FILE_NAME;
    }
    if (options.logToFile && options.logPath.empty()) {
        options.logPath = DEFAULT_LOG_FILE_NAME;
    }

    try {
        logging::init(options.logPath, options.logLevel, options.logToFile);
    } catch (const spdlog::spdlog_ex &exc) {
        spdlog::warn("Не удалось инициализировать файловый логгер ({}): {}", options.logPath, exc.what());
        options.logToFile = false;
        logging::init("", options.logLevel, false);
    }

    spdlog::cfg::load_env_levels();
    if (options.logLevelExplicit) {
        spdlog::set_level(options.logLevel);
    }

    spdlog::info("Starting logger");
    spdlog::info("Файл конфигурации: \"{}\"", options.configPath);
    spdlog::info("Файл кеша: \"{}\"", options.cachePath);
    if (options.logToFile) {
        spdlog::info("Файл лога: \"{}\"", options.logPath);
    }

    ProgramConfiguration programConfiguration;
    CacheBuckets cacheBuckets;
    optional<QueryStore> queryStoreStorage;

    try {
        programConfiguration.files = loadFiles(options);
        loadJSONConfigurationData(programConfiguration.files.configuration.contents, programConfiguration);
        printJSONConfigurationData(programConfiguration);
        cacheBuckets = loadCacheBuckets(programConfiguration.files.cache.contents);
        queryStoreStorage.emplace(programConfiguration.files.configuration.path, programConfiguration.files.configuration.contents);
    } catch (const exception &exc) {
        spdlog::error("{}", exc.what());
        return 1;
    }

    QueryStore &queryStore = *queryStoreStorage;
    
    for (const QueryConfiguration &query : queryStore.getQueries()) {
        spdlog::info(
            "\n- Добавлен запрос:\n {}", 
            query.getDescription()
        );
    }

    Telegram::Client telegramClient(programConfiguration.telegramConfiguration, queryStore);
    Market::MarketAnalyzer marketAnalyzer;
    telegramClient.startPolling();

    Clock::time_point lastCacheSave = Clock::now();

    bool cacheDirty = false;

    auto saveCache = [&](bool force = false) {
        if (!cacheDirty) 
            return;
        if (!force && Clock::now() - lastCacheSave < CACHE_SAVE_INTERVAL) 
            return;

        saveFile(programConfiguration.files.cache.path, cacheBucketsToJSON(cacheBuckets).dump());
        lastCacheSave = Clock::now();
        cacheDirty = false;
    };

    auto interruptibleSleep = [&](int seconds) {
        for (int i = 0; i < seconds && !gStop; ++i) {
            sleep(1);
            saveCache();
        }
    };
    
    struct QueryPollState {
        time_t frontier = 0;
        time_t suppressBefore = 0;
        bool overlapEnabled = false;
    };

    unordered_map<QueryID, QueryPollState> pollStates;

    while (!gStop) {
        for (const auto &requestConfiguration : queryStore.getQueries()) {
            if (gStop)
                break;

            const QueryID queryID = requestConfiguration.id;
            const string cacheKey = requestConfiguration.cacheKey();
            unordered_set<int> &viewedAds = cacheBuckets[cacheKey];

            const bool chronological = !requestConfiguration.kufar.sortType.has_value();
            const auto stateIt = pollStates.find(queryID);
            const bool coldStart = stateIt == pollStates.end();
            const bool useCatchUp = chronological && !coldStart;

            time_t windowStart = 0;
            if (useCatchUp) {
                const time_t overlap = stateIt->second.overlapEnabled ? POLL_FRONTIER_OVERLAP_SECONDS : 0;
                windowStart = max(stateIt->second.frontier - overlap, stateIt->second.suppressBefore);
            }

            vector<Ad> fetched;
            size_t processCount = 0;
            bool fetchSucceeded = true;

            try {
                if (!useCatchUp) {
                    fetched = getSearchPage(
                        requestConfiguration.kufar,
                        nullopt,
                        COLD_START_LISTING_LIMIT
                    ).ads;
                    processCount = fetched.size();

                    if (coldStart && chronological) {
                        spdlog::debug(
                            "[Poll] Query {} cold start: considering the newest {} listing(s).",
                            queryID,
                            processCount
                        );
                    }
                } else {
                    FetchOptions options;
                    options.pageSize = DEFAULT_PAGE_SIZE;
                    options.maxPages = POLL_MAX_PAGES;
                    options.newerThan = windowStart;
                    options.shouldStop = [] { return gStop != 0; };

                    FetchResult result = getAllAds(requestConfiguration.kufar, options);
                    fetched = std::move(result.ads);
                    processCount = fetched.size();

                    if (result.truncated && !gStop) {
                        spdlog::warn(
                            "[Poll] Query {} reached the page limit ({} pages); "
                            "older listings in this window were skipped.",
                            queryID,
                            POLL_MAX_PAGES
                        );
                    }
                }
            } catch (const exception &exc) {
                spdlog::error("(poll) [Query: {}]: {}", queryID, exc.what());
                fetchSucceeded = false;
                fetched.clear();
                processCount = 0;
            }

            bool interrupted = gStop != 0;
            bool allSent = true;

            for (size_t index = 0; index < processCount; index++) {
                if (gStop) {
                    interrupted = true;
                    break;
                }

                const Ad &advert = fetched[index];

                // Listings below the polling window are history that the pagination happened to include on its last page
                if (useCatchUp && advert.date < windowStart)
                    continue;

                if (viewedAds.contains(advert.id))
                    continue;

                spdlog::info(
                    "[New]: Adding [Title: {}], [ID: {}], [Tag: {}], [Topic: {}], [Link: {}]",
                    advert.title,
                    advert.id,
                    advert.tag.value_or(PROPERTY_UNDEFINED),
                    cacheKey,
                    advert.link
                );

                try {
                    const auto priceAssessment = marketAnalyzer.rate(advert);
                    const auto adMessage = telegramClient.sendAdvert(advert, requestConfiguration.messageThreadID, priceAssessment);

                    viewedAds.insert(advert.id);
                    cacheDirty = true;

                    if (!priceAssessment.has_value() && adMessage != nullptr) {
                        marketAnalyzer.rateDeferred(
                            advert,
                            [&telegramClient, adMessage, advert](Market::PriceAssessment newAssessment) {
                                telegramClient.editAdvertAssessment(adMessage, advert, newAssessment);
                            }
                        );
                    }
                } catch (const exception &exc) {
                    allSent = false;
                    spdlog::error("(sendAdvert) [Query: {}] [Topic: {}]: {}", queryID, cacheKey, exc.what());
                }

                usleep(300000); // 0.3s
            }

            if (fetchSucceeded && chronological && !interrupted && allSent) {
                time_t newest = coldStart ? 0 : stateIt->second.frontier;
                for (const Ad &advert : fetched) {
                    newest = max(newest, advert.date);
                }

                if (newest > 0) {
                    QueryPollState &state = pollStates[queryID];
                    state.frontier = newest;

                    if (coldStart) {
                        state.suppressBefore = newest;
                    } else {
                        state.overlapEnabled = true;
                    }
                }
            }

            DEBUG_MSG("(QueryDelay) Sleeping for: {}s.", programConfiguration.queryDelaySeconds);
            interruptibleSleep(programConfiguration.queryDelaySeconds);
        }

        DEBUG_MSG("(LoopDelay) Sleeping for: {}s.", programConfiguration.loopDelaySeconds);
        interruptibleSleep(programConfiguration.loopDelaySeconds);
    }

    saveCache(true);
    return 0;
}
