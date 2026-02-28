#include "core/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace glory {

void Log::init() {
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern("%^[%T] [%l] %v%$");

    auto logger = std::make_shared<spdlog::logger>("Glory", consoleSink);
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::trace);

    spdlog::set_default_logger(logger);
    spdlog::info("Logger initialized");
}

} // namespace glory
