//
//  main.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 02.06.2022.
//

#include <algorithm>
#include <chrono>
#include <optional>
#include <signal.h>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>

#include <nlohmann/json.hpp>

#include "arguments.hpp"
#include "cache.hpp"
#include "delivery.hpp"
#include "helperfunctions.hpp"
#include "kufar.hpp"
#include "logging.hpp"
#include "market.hpp"
#include "networking.hpp"
#include "queries.hpp"
#include "request_gate.hpp"
#include "scheduler.hpp"
#include "telegram.hpp"
#include "telegram_sender.hpp"

using namespace std;
using namespace std::chrono_literals;
using nlohmann::json;

const string DEFAULT_CACHE_FILE_NAME = "cached-data.json";
const string DEFAULT_CONFIGURATION_FILE_NAME = "kufar-configuration.json";
const string DEFAULT_LOG_FILE_NAME = "kufar-notifier.log";

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

struct ProgramConfiguration {
    Telegram::TelegramConfiguration telegramConfiguration;
    Files files;

    scheduling::SchedulerConfiguration scheduler;
    Networking::RequestGateConfig kufarRequest;
    // Retained only for backwards-compatible configuration parsing
    int queryDelaySeconds = 5;
    int loopDelaySeconds = 30;
};

scheduling::SchedulerConfiguration parseSchedulerConfiguration(const json &data, int loopDelaySeconds) {
    scheduling::SchedulerConfiguration configuration;

    if (data.contains("scheduler")) {
        const json &schedulerData = data.at("scheduler");
        if (!schedulerData.is_object()) {
            throw runtime_error("Поле \"scheduler\" должно быть объектом.");
        }

        const auto readPositiveSeconds = [&schedulerData](const char *key, int fallback) {
            if (!schedulerData.contains(key)) {
                return fallback;
            }

            const int value = schedulerData.at(key).get<int>();
            if (value <= 0) {
                throw runtime_error(
                    string("Поле \"scheduler.") + key + "\" должно быть положительным числом."
                );
            }
            return value;
        };

        const int defaultInterval = readPositiveSeconds(
            "default-interval-seconds",
            static_cast<int>(configuration.defaultInterval.count())
        );
        const int minInterval = readPositiveSeconds(
            "min-interval-seconds",
            defaultInterval
        );
        const int maxInterval = readPositiveSeconds(
            "max-interval-seconds",
            max(300, minInterval)
        );

        configuration.defaultInterval = chrono::seconds(defaultInterval);
        configuration.minInterval = chrono::seconds(minInterval);
        configuration.maxInterval = chrono::seconds(maxInterval);
        configuration.jitterPercent = schedulerData.value(
            "jitter-percent",
            configuration.jitterPercent
        );

        if (configuration.maxInterval < configuration.minInterval) {
            throw runtime_error(
                "Поле \"scheduler.max-interval-seconds\" не может быть меньше "
                "\"scheduler.min-interval-seconds\"."
            );
        }
        if (configuration.jitterPercent < 0 || configuration.jitterPercent > 100) {
            throw runtime_error("Поле \"scheduler.jitter-percent\" должно быть от 0 до 100.");
        }
    } else if (loopDelaySeconds > 0) {
        // Backwards compatibility: older configs effectively polled each query once per outer loop
        configuration.defaultInterval = chrono::seconds(loopDelaySeconds);
        configuration.minInterval = chrono::seconds(loopDelaySeconds);
        configuration.maxInterval = chrono::seconds(max(loopDelaySeconds, 300));
    }

    return configuration;
}

void loadJSONConfigurationData(const json &data, ProgramConfiguration &programConfiguration) {
    {
        const json telegramData = data.at("telegram");
        programConfiguration.telegramConfiguration.botToken = telegramData.at("bot-token");
        programConfiguration.telegramConfiguration.chatID = telegramData.at("chat-id");
    }

    if (data.contains("delays")) {
        const json &delaysData = data.at("delays");
        programConfiguration.queryDelaySeconds = delaysData.value(
            "query",
            programConfiguration.queryDelaySeconds
        );
        programConfiguration.loopDelaySeconds = delaysData.value(
            "loop",
            programConfiguration.loopDelaySeconds
        );
    }

    if (data.contains("delays") && data.at("delays").contains("query")) {
        spdlog::warn(
            "[Конфигурация] Поле \"delays.query\" устарело и игнорируется. "
            "Используйте \"scheduler\" и \"poll-interval-seconds\"."
        );
    }

    programConfiguration.scheduler = parseSchedulerConfiguration(
        data,
        programConfiguration.loopDelaySeconds
    );

    if (!data.contains("scheduler") && data.contains("delays")) {
        spdlog::warn(
            "[Конфигурация] Раздел \"scheduler\" отсутствует; используется запасной "
            "интервал {}с из \"delays.loop\".",
            programConfiguration.scheduler.defaultInterval.count()
        );
    }

    if (data.contains("kufar")) {
        const json &kufarData = data.at("kufar");
        if (!kufarData.is_object()) {
            throw runtime_error("Поле \"kufar\" должно быть объектом.");
        }

        programConfiguration.kufarRequest.requestsPerSecond = kufarData.value(
            "requests-per-second",
            programConfiguration.kufarRequest.requestsPerSecond
        );
        programConfiguration.kufarRequest.burst = kufarData.value(
            "burst",
            programConfiguration.kufarRequest.burst
        );
        programConfiguration.kufarRequest.cooldown = chrono::seconds(
            kufarData.value(
                "cooldown-seconds",
                static_cast<int>(programConfiguration.kufarRequest.cooldown.count())
            )
        );

        if (programConfiguration.kufarRequest.requestsPerSecond <= 0.0) {
            throw runtime_error("\"kufar.requests-per-second\" должен быть положительным.");
        }
        if (programConfiguration.kufarRequest.burst <= 0) {
            throw runtime_error("\"kufar.burst\" должен быть положительным.");
        }
        if (programConfiguration.kufarRequest.cooldown.count() <= 0) {
            throw runtime_error("\"kufar.cooldown-seconds\" должен быть положительным.");
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
        "\n- Планировщик:\n"
        "\t- Интервал по умолчанию: {}с.\n"
        "\t- Минимальный интервал: {}с.\n"
        "\t- Максимальный интервал: {}с.\n"
        "\t- Джиттер: {}%.",
        programConfiguration.scheduler.defaultInterval.count(),
        programConfiguration.scheduler.minInterval.count(),
        programConfiguration.scheduler.maxInterval.count(),
        programConfiguration.scheduler.jitterPercent
    );

    spdlog::info(
        "\n- Kufar:\n"
        "\t- Запросов в секунду: {}\n"
        "\t- Burst: {}\n"
        "\t- Cooldown: {}с.",
        programConfiguration.kufarRequest.requestsPerSecond,
        programConfiguration.kufarRequest.burst,
        programConfiguration.kufarRequest.cooldown.count()
    );
}

json getJSONDataFromPath(const string &JSONFilePath) {
    spdlog::info("[Загрузка файла]: \"{}\"", JSONFilePath);

    if (!fileExists(JSONFilePath)) {
        throw runtime_error("Файл не существует по данному пути или к нему нет доступа: \"" + JSONFilePath + "\"");
    }

    if (getFileSize(JSONFilePath) > 4000000) {
        throw runtime_error("Размер файла превышает 4МБ: \"" + JSONFilePath + "\"");
    }

    try {
        return json::parse(getTextFromFile(JSONFilePath));
    } catch (const exception &exc) {
        throw runtime_error("Невозможно получить данные из файла \"" + JSONFilePath + "\": " + exc.what());
    }
}

void createEmptyCacheFile(const string &path) {
    spdlog::info("[Кеш] Файл не найден, создаю новый: \"{}\"", path);
    saveFile(path, cache::CacheStore::emptyDocument().dump(4));
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
    optional<cache::CacheStore> cacheStorage;
    optional<queries::QueryStore> queryStoreStorage;
    optional<Networking::RequestGate> requestGateStorage;

    try {
        programConfiguration.files = loadFiles(options);
        loadJSONConfigurationData(programConfiguration.files.configuration.contents, programConfiguration);
        printJSONConfigurationData(programConfiguration);

        requestGateStorage.emplace(
            programConfiguration.kufarRequest,
            [] { return gStop != 0; }
        );
        Networking::setRequestGate(&*requestGateStorage);

        cacheStorage.emplace(options.cachePath, programConfiguration.files.cache.contents);
        queryStoreStorage.emplace(
            programConfiguration.files.configuration.path,
            programConfiguration.files.configuration.contents
        );
    } catch (const exception &exc) {
        spdlog::error("{}", exc.what());
        return 1;
    }

    cache::CacheStore &cacheStore = *cacheStorage;
    queries::QueryStore &queryStore = *queryStoreStorage;

    for (const queries::QueryConfiguration &query : queryStore.getQueries()) {
        spdlog::info(
            "\n- Добавлен запрос:\n {}",
            query.getDescription()
        );
    }

    Telegram::Client telegramClient(programConfiguration.telegramConfiguration, queryStore);

    Market::MarketAnalyzer marketAnalyzer;

    delivery::SendFunction send = delivery::makeTelegramSender(telegramClient, marketAnalyzer);
    delivery::DeliveryWorker deliveryWorker(
        send,
        cacheStore,
        [&queryStore](queries::QueryID id) { return queryStore.contains(id); }
    );

    telegramClient.startPolling();

    scheduling::PollingScheduler scheduler(
        queryStore,
        cacheStore,
        deliveryWorker,
        programConfiguration.scheduler,
        scheduling::NewestFetcher(),
        scheduling::WindowFetcher(),
        scheduling::NowFunction(),
        [] { return gStop != 0; }
    );

    try {
        scheduler.run();
    } catch (const exception &exc) {
        spdlog::error("[Poll] Scheduler stopped with an error: {}", exc.what());
    }

    deliveryWorker.stopAndDrain(5s);
    cacheStore.saveNow();

    return 0;
}
