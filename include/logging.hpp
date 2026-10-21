//
//  logging.hpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 18.10.2026
//

#pragma once
#include <spdlog/spdlog.h>
#include <string>

namespace logging {
void init(const std::string& logfile, spdlog::level::level_enum level = spdlog::level::info);
}
