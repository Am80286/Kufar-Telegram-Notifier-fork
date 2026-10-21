//
//  arguments.cpp
//  Kufar Telegram Notifier
//

#include "arguments.hpp"

#include <string>

#include <spdlog/common.h>

#include "CLI11.hpp"

namespace {

constexpr const char *VERSION =
#ifdef APP_VERSION
    APP_VERSION
#else
    "2.6"
#endif
    ;

}  // namespace

namespace arguments {

std::optional<Options> parse(int argc, char **argv, int &exitCode) {
    CLI::App app{
        "Kufar Telegram Notifier — уведомления о новых объявлениях на Kufar через Telegram.",
        "Kufar-Telegram-Notifier"
    };

    auto formatter = app.get_formatter();
    formatter->label("Usage", "Использование");
    formatter->label("OPTIONS", "ОПЦИИ");
    formatter->label("POSITIONALS", "ПОЗИЦИОННЫЕ АРГУМЕНТЫ");
    formatter->label("REQUIRED", "ОБЯЗАТЕЛЬНО");
    formatter->label("Env", "Переменная окружения");
    formatter->label("Needs", "Требуется");
    formatter->label("Excludes", "Несовместимо с");
    formatter->column_width(38);

    app.failure_message([](const CLI::App *, const CLI::Error &e) {
        return std::string("Ошибка аргументов: ") + e.what() + "\nИспользуйте --help для справки.\n";
    });

    app.get_help_ptr()->group("Опции");
    app.get_help_ptr()->description("Показать это сообщение и выйти");

    app.set_version_flag("-V,--version", VERSION, "Показать версию и выйти");
    app.get_version_ptr()->group("Опции");

    Options options;
    bool noLog = false;
    std::string logLevelName = "info";

    auto *configOption = app.add_option(
        "-c,--config",
        options.configPath,
        "Путь к файлу конфигурации (по умолчанию: kufar-configuration.json в текущей папке)"
    );
    configOption->type_name("ФАЙЛ")->group("Опции");

    auto *cacheOption = app.add_option(
        "--cache",
        options.cachePath,
        "Путь к файлу кеша (по умолчанию: cached-data.json в текущей папке; создаётся при отсутствии)"
    );
    cacheOption->type_name("ФАЙЛ")->group("Опции");

    auto *logOption = app.add_option(
        "-l,--log",
        options.logPath,
        "Путь к файлу лога (по умолчанию: kufar-notifier.log в текущей папке)"
    );
    logOption->type_name("ФАЙЛ")->group("Опции");

    auto *noLogOption = app.add_flag("--no-log", noLog, "Не записывать лог в файл (только консоль)");
    noLogOption->group("Опции");
    logOption->excludes(noLogOption);

    auto *logLevelOption = app.add_option(
        "--log-level",
        logLevelName,
        "Уровень логирования: trace, debug, info, warn, error, critical (по умолчанию: info)"
    );
    logLevelOption->type_name("УРОВЕНЬ")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error", "critical"}))
        ->group("Опции");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        exitCode = app.exit(e);
        if (exitCode != 0) {
            // Приводим коды CLI11 (100+) к обычному для ошибок аргументов коду 2.
            exitCode = 2;
        }
        return std::nullopt;
    }

    options.logLevel = spdlog::level::from_str(logLevelName);
    options.logLevelExplicit = app.count("--log-level") > 0;
    options.logToFile = !noLog;

    exitCode = 0;
    return options;
}

}  // namespace arguments
