//
//  main.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 02.06.2022.
//

#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <map>
#include <vector>

#include "spdlog/spdlog.h"
#include "spdlog/cfg/env.h"

#include "json.hpp"
#include "kufar.hpp"
#include "telegram.hpp"
#include "networking.hpp"
#include "helperfunctions.hpp"
#include "logging.hpp"
#include "arguments.hpp"

using namespace std;
using namespace Kufar;
using namespace Telegram;
using nlohmann::json;

const string DEFAULT_CACHE_FILE_NAME = "cached-data.json";
const string DEFAULT_CONFIGURATION_FILE_NAME = "kufar-configuration.json";
const string DEFAULT_LOG_FILE_NAME = "kufar-notifier.log";

const int CACHE_VERSION = 2;
const string DEFAULT_CACHE_BUCKET = "default";

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

/**
  Поисковый запрос вместе с настройками его доставки в Telegram.
  messageThreadID отсутствует, если объявления должны идти в основной чат.
 */
struct QueryConfiguration {
    Kufar::KufarConfiguration kufar;
    optional<int> messageThreadID;

    /**
      Ключ кеша — тема (topic), а не сам запрос.
      Несколько запросов могут публиковаться в одну тему, и в этом случае
      объявление не должно отправляться в неё повторно.
      "default" — основной чат / тема "General".
     */
    string cacheKey() const {
        return messageThreadID.has_value() ? to_string(messageThreadID.value()) : DEFAULT_CACHE_BUCKET;
    }
};

/**
  Кеш отправленных объявлений: ключ — тема чата, значение — ID объявлений.
 */
using CacheBuckets = map<string, vector<int>>;

struct ProgramConfiguration {
    vector<QueryConfiguration> queries;
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
        json queriesData = data.at("queries");
        
        for (const json &query : queriesData) {
            KufarConfiguration kufarConfiguration;
            
            kufarConfiguration.onlyTitleSearch = getOptionalValue<bool>(query, "only-title-search");
            kufarConfiguration.tag = getOptionalValue<string>(query, "tag");
            if (query.contains("price")) {
                json queryPriceData = query.at("price");
                kufarConfiguration.priceRange.priceMin = getOptionalValue<int>(queryPriceData, "min");
                kufarConfiguration.priceRange.priceMax = getOptionalValue<int>(queryPriceData, "max");
            }
            
            kufarConfiguration.language = getOptionalValue<string>(query, "language");
            kufarConfiguration.limit = getOptionalValue<int>(query, "limit");
            kufarConfiguration.currency = getOptionalValue<string>(query, "currency");
            kufarConfiguration.condition = getOptionalValue<ItemCondition>(query, "condition");
            kufarConfiguration.sellerType = getOptionalValue<SellerType>(query, "seller-type");
            kufarConfiguration.kufarDeliveryRequired = getOptionalValue<bool>(query, "kufar-delivery-required");
            kufarConfiguration.kufarPaymentRequired = getOptionalValue<bool>(query, "kufar-payment-required");
            kufarConfiguration.kufarHalvaRequired = getOptionalValue<bool>(query, "kufar-halva-required");
            kufarConfiguration.onlyWithPhotos = getOptionalValue<bool>(query, "only-with-photos");
            kufarConfiguration.onlyWithVideos = getOptionalValue<bool>(query, "only-with-videos");
            kufarConfiguration.onlyWithExchangeAvailable = getOptionalValue<bool>(query, "only-with-exchange-available");
            kufarConfiguration.sortType = getOptionalValue<SortType>(query, "sort-type");
            kufarConfiguration.category = getOptionalValue<Category>(query, "category");
            kufarConfiguration.subCategory = getOptionalValue<int>(query, "sub-category");
            kufarConfiguration.region = getOptionalValue<Region>(query, "region");
            kufarConfiguration.areas = getOptionalValue<vector<int>>(query, "areas");

            QueryConfiguration queryConfiguration;
            queryConfiguration.kufar = kufarConfiguration;
            queryConfiguration.messageThreadID = getOptionalValue<int>(query, "message-thread-id");

            if (queryConfiguration.messageThreadID.has_value() && queryConfiguration.messageThreadID.value() <= 0) {
                throw runtime_error(
                    "\"message-thread-id\" должен быть положительным числом (ID темы чата), "
                    "либо отсутствовать, чтобы публиковать в основной чат."
                );
            }

            programConfiguration.queries.push_back(queryConfiguration);
        }
    }
    {
        if (data.contains("delays")) {
            json delaysData = data.at("delays");
            programConfiguration.queryDelaySeconds = delaysData.at("query");
            programConfiguration.loopDelaySeconds = delaysData.at("loop");
        }
    }
}

std::string joinAreas(const std::optional<std::vector<int>> &areas) {
    if (!areas.has_value()) {
        return PROPERTY_UNDEFINED;
    }
    
    std::string joinedAreas;
    const std::vector<int> &values = areas.value();
    
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0) {
            joinedAreas += ", ";
        }
        joinedAreas += EnumString::area(values[i]);
    }
    
    return joinedAreas;
}

void printJSONConfigurationData(const ProgramConfiguration &programConfiguration) {
    spdlog::info(
        "\n- Telegram:\n"
        "\t- Токен: {}\n"
        "\t- ID Чата: {}\n",
        maskToken(programConfiguration.telegramConfiguration.botToken),
        programConfiguration.telegramConfiguration.chatID
    );
    
    for (const auto &query : programConfiguration.queries) {
        spdlog::info(
            "\n- Добавлен запрос:\n"
            "\t- Название: {}\n"
            "\t- ID темы чата: {}\n"
            "\t- Поиск только по заголовку: {}\n"
            "\t- Цена:\n"
            "\t\t- Минимальная: {} BYN\n"
            "\t\t- Максимальная: {} BYN\n"
            "\t- Язык: {}\n"
            "\t- Макс. кол-во объявлений за один запрос: {}\n"
            "\t- Валюта: {}\n"
            "\t- Состояние: {}\n"
            "\t- Продавец: {}\n"
            "\t- Только с Kufar Доставкой: {}\n"
            "\t- Только с Kufar Оплатой: {}\n"
            "\t- Только с Kufar Рассрочкой (Халва): {}\n"
            "\t- Только с фото: {}\n"
            "\t- Только с видео: {}\n"
            "\t- Только с возможностью обмена: {}\n"
            "\t- Тип сортировки: {}\n"
            "\t- Категория: {}\n"
            "\t- Подкатегория: {}\n"
            "\t- Город: {}\n"
            "\t- Район: {}\n",
            toString(query.kufar.tag),
            (query.messageThreadID.has_value() ? toString(query.messageThreadID) : "[основной чат]"),
            toString(query.kufar.onlyTitleSearch),
            toString(query.kufar.priceRange.priceMin),
            toString(query.kufar.priceRange.priceMax),
            toString(query.kufar.language),
            toString(query.kufar.limit),
            toString(query.kufar.currency),
            (query.kufar.condition.has_value() ? EnumString::itemCondition(query.kufar.condition.value()) : PROPERTY_UNDEFINED),
            (query.kufar.sellerType.has_value() ? EnumString::sellerType(query.kufar.sellerType.value()) : PROPERTY_UNDEFINED),
            toString(query.kufar.kufarDeliveryRequired),
            toString(query.kufar.kufarPaymentRequired),
            toString(query.kufar.kufarHalvaRequired),
            toString(query.kufar.onlyWithPhotos),
            toString(query.kufar.onlyWithVideos),
            toString(query.kufar.onlyWithExchangeAvailable),
            (query.kufar.sortType.has_value() ? EnumString::sortType(query.kufar.sortType.value()) : PROPERTY_UNDEFINED),
            (query.kufar.category.has_value() ? EnumString::category(query.kufar.category.value()) : PROPERTY_UNDEFINED),
            (query.kufar.subCategory.has_value() ? EnumString::subCategory(query.kufar.subCategory.value()) : PROPERTY_UNDEFINED),
            (query.kufar.region.has_value() ? EnumString::region(query.kufar.region.value()) : PROPERTY_UNDEFINED),
            joinAreas(query.kufar.areas)
        );
    }

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
  Представление кеша в JSON:
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
        // Устаревший формат (плоский массив). Автоматически не переносим:
        // лучше начать с чистого кеша, чем пропустить новые объявления.
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
        cacheBuckets[cacheKey] = adIDs.get<vector<int>>();
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

/**
  Загрузка файлов конфигурации и кеша.
  Отсутствующий файл кеша создаётся автоматически.
 */

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

    arguments::Options options = parsedOptions.value();

    // По умолчанию файлы ищутся в текущей рабочей папке.
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
        // Явно указанный в командной строке уровень важнее SPDLOG_LEVEL.
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

    try {
        programConfiguration.files = loadFiles(options);
        loadJSONConfigurationData(programConfiguration.files.configuration.contents, programConfiguration);
        printJSONConfigurationData(programConfiguration);
        cacheBuckets = loadCacheBuckets(programConfiguration.files.cache.contents);
    } catch (const exception &exc) {
        spdlog::error("{}", exc.what());
        return 1;
    }

    while (true) {
        for (const auto &requestConfiguration : programConfiguration.queries) {
            const string cacheKey = requestConfiguration.cacheKey();
            vector<int> &viewedAds = cacheBuckets[cacheKey];
            
            try {
                for (const auto &advert : getAds(requestConfiguration.kufar)) {
                    if (!vectorContains(viewedAds, advert.id)) {
                        spdlog::info(
                            "[New]: Adding [Title: {}], [ID: {}], [Tag: {}], [Topic: {}], [Link: {}]",
                            advert.title,
                            advert.id,
                            advert.tag.value_or(PROPERTY_UNDEFINED),
                            cacheKey,
                            advert.link
                        );

                        try {
                            sendAdvert(programConfiguration.telegramConfiguration, advert, requestConfiguration.messageThreadID);
                            // В кеш попадают только действительно отправленные объявления.
                            viewedAds.push_back(advert.id);
                            saveFile(programConfiguration.files.cache.path, cacheBucketsToJSON(cacheBuckets).dump());
                        } catch (const exception &exc) {
                            spdlog::error("(sendAdvert) [Topic: {}]: {}", cacheKey, exc.what());
                        }
                        
                    } else {
                        //spdlog::debug("[Already was!]");
                    }
                    usleep(300000); // 0.3s
                }
            } catch (const exception &exc) {
                spdlog::error("(getAds): {}", exc.what());
            }
            DEBUG_MSG("(QueryDelay) Sleeping for: {}s.", programConfiguration.queryDelaySeconds);
            sleep(programConfiguration.queryDelaySeconds);
        }
        DEBUG_MSG("(LoopDelay) Sleeping for: {}s.", programConfiguration.loopDelaySeconds);
        sleep(programConfiguration.loopDelaySeconds);
    }
    return 0;
}
