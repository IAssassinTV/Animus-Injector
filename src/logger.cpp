#include "logger.h"
#include "animus_injector.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <format>
#include <memory>
#include <unordered_map>

namespace animus_injector::logger
{
    namespace
    {
        std::shared_ptr<spdlog::logger> s_logger;
        bool s_enabled = false;
        
        constexpr auto get_log_level(std::string_view level) -> spdlog::level::level_enum
        {
            if (level == "debug") return spdlog::level::debug;
            if (level == "info") return spdlog::level::info;
            if (level == "warn" || level == "warning") return spdlog::level::warn;
            if (level == "error") return spdlog::level::err;
            return spdlog::level::info;
        }
    }

    auto initialize(const std::filesystem::path& log_path, bool enabled, std::string_view level) -> bool
    {
        s_enabled = enabled;

        if (!s_enabled)
            return true;

        try
        {
            s_logger = spdlog::basic_logger_mt("AnimusInjector", log_path.string(), true);
            s_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
            s_logger->flush_on(spdlog::level::debug);
            s_logger->set_level(get_log_level(level));

            s_logger->info("logger initialized");
            s_logger->info(std::format("{} v{}", ANIMUS_INJECTOR_NAME, ANIMUS_INJECTOR_VERSION));
            s_logger->info(std::format("log level: {}", level));

            return true;
        }
        catch (const spdlog::spdlog_ex&)
        {
            return false;
        }
    }

    auto shutdown() -> void
    {
        if (s_logger)
        {
            s_logger->info("logger shutting down");
            s_logger->flush();
            spdlog::drop("AnimusInjector");
            s_logger.reset();
        }
    }

    auto debug(std::string_view message) -> void
    {
        if (s_enabled && s_logger)
            s_logger->debug(message);
    }

    auto info(std::string_view message) -> void
    {
        if (s_enabled && s_logger)
            s_logger->info(message);
    }

    auto warn(std::string_view message) -> void
    {
        if (s_enabled && s_logger)
            s_logger->warn(message);
    }

    auto error(std::string_view message) -> void
    {
        if (s_enabled && s_logger)
            s_logger->error(message);
    }
}
