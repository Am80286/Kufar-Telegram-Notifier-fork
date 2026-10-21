//
//  logging.cpp
//  Kufar Telegram Notifier
//
//  Created by Am286 on 18.10.2026
//

#include "logging.hpp"
#include <chrono>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace logging {

void init(const std::string& logfile, spdlog::level::level_enum level) {
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>(spdlog::color_mode::always);
    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logfile, 5 * 1024 * 1024, 3);  // 5 MB per file, keep 3
    auto logger = std::make_shared<spdlog::logger>("app", spdlog::sinks_init_list{console, file});

    logger->set_level(level);
    logger->flush_on(spdlog::level::warn);  // flush immediately on warn+

    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    spdlog::flush_every(std::chrono::seconds(1));  // periodically flush buffered file logs
}

}  // namespace logging
