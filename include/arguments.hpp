//
//  arguments.hpp
//  Kufar Telegram Notifier
//
//  Created by Am80286 on 21.10.2026
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
 Parses command-line arguments.

 Returns std::nullopt if the program should exit
 (--help, --version, or a parse error). In that case exitCode
 contains the process exit code.
 */
std::optional<Options> parse(int argc, char **argv, int &exitCode);

}  // namespace arguments
