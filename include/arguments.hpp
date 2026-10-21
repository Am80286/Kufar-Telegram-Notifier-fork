//
//  arguments.hpp
//  Kufar Telegram Notifier
//

#pragma once

#include <optional>
#include <string>

#include <spdlog/common.h>

namespace arguments {

struct Options {
    std::string configPath;
    std::string cachePath;
    std::string logPath;

    spdlog::level::level_enum logLevel = spdlog::level::info;
    bool logLevelExplicit = false;
    bool logToFile = true;
};

/**
 Разбор аргументов командной строки.

 Возвращает std::nullopt, если программа должна завершиться
 (--help, --version или ошибка разбора). В этом случае exitCode
 содержит код возврата процесса.
 */
std::optional<Options> parse(int argc, char **argv, int &exitCode);

}  // namespace arguments
