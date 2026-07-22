#include "config.h"
#include "claudia.h"
#include "logger.h"

#define INIPP_IMPLEMENTATION
#include <inipp/inipp.h>

#include <fstream>
#include <format>

namespace claudia::config
{
    namespace
    {
        settings s_settings;
        std::filesystem::path s_config_path;
        bool s_initialized = false;

        [[nodiscard]] constexpr bool parse_bool(std::string_view str) noexcept
        {
            return str == "true" || str == "1" || str == "yes";
        }
    }

    auto initialize(const std::filesystem::path& config_path) -> bool
    {
        s_config_path = config_path;
        s_initialized = true;

        // set defaults
        s_settings = {
            .creds = { .username = "", .password = "" },
            .net = { .redirect_host = std::string(DEFAULT_REDIRECT_HOST) },
            .log = { .enabled = true, .level = "info" },
            .ui = { .skip_config_dialog = false },
            .fix = { .fix_cpu_affinity = true, .fix_xinput_detection = true, .fix_disable_punkbuster = true }
        };

        return true;
    }

    auto load() -> bool
    {
        if (!s_initialized || !exists())
            return false;

        try
        {
            std::ifstream file(s_config_path);
            if (!file.is_open())
                return false;

            inipp::Ini<char> ini;
            ini.parse(file);

            // credentials
            inipp::get_value(ini.sections["Credentials"], "Username", s_settings.creds.username);
            inipp::get_value(ini.sections["Credentials"], "Password", s_settings.creds.password);

            // network (redirect is always enabled, only host is configurable)
            inipp::get_value(ini.sections["Network"], "RedirectHost", s_settings.net.redirect_host);

            // logging
            if (std::string enabled_str; 
                inipp::get_value(ini.sections["Logging"], "Enabled", enabled_str))
            {
                s_settings.log.enabled = parse_bool(enabled_str);
            }
            inipp::get_value(ini.sections["Logging"], "Level", s_settings.log.level);

            // ui
            if (std::string skip_str;
                inipp::get_value(ini.sections["UI"], "SkipConfigDialog", skip_str))
            {
                s_settings.ui.skip_config_dialog = parse_bool(skip_str);
            }

            // fixes
            if (std::string cpu_str;
                inipp::get_value(ini.sections["Fixes"], "FixCpuAffinity", cpu_str))
            {
                s_settings.fix.fix_cpu_affinity = parse_bool(cpu_str);
            }
            if (std::string xinput_str;
                inipp::get_value(ini.sections["Fixes"], "FixXInputDetection", xinput_str))
            {
                s_settings.fix.fix_xinput_detection = parse_bool(xinput_str);
            }
            if (std::string pb_str;
                inipp::get_value(ini.sections["Fixes"], "FixDisablePunkBuster", pb_str))
            {
                s_settings.fix.fix_disable_punkbuster = parse_bool(pb_str);
            }

            return true;
        }
        catch (const std::exception& e)
        {
            logger::error(std::format("failed to load config: {}", e.what()));
            return false;
        }
    }

    auto save() -> bool
    {
        if (!s_initialized)
            return false;

        try
        {
            std::ofstream file(s_config_path);
            if (!file.is_open())
                return false;

            file << "[Credentials]\n";
            file << std::format("Username={}\n", s_settings.creds.username);
            file << std::format("Password={}\n", s_settings.creds.password);
            file << '\n';

            file << "[Network]\n";
            file << std::format("RedirectHost={}\n", s_settings.net.redirect_host);
            file << '\n';

            file << "[Logging]\n";
            file << std::format("Enabled={}\n", s_settings.log.enabled ? "true" : "false");
            file << std::format("Level={}\n", s_settings.log.level);
            file << '\n';

            file << "[UI]\n";
            file << std::format("SkipConfigDialog={}\n", s_settings.ui.skip_config_dialog ? "true" : "false");
            file << '\n';

            file << "[Fixes]\n";
            file << std::format("FixCpuAffinity={}\n", s_settings.fix.fix_cpu_affinity ? "true" : "false");
            file << std::format("FixXInputDetection={}\n", s_settings.fix.fix_xinput_detection ? "true" : "false");
            file << std::format("FixDisablePunkBuster={}\n", s_settings.fix.fix_disable_punkbuster ? "true" : "false");

            return true;
        }
        catch (const std::exception& e)
        {
            logger::error(std::format("failed to save config: {}", e.what()));
            return false;
        }
    }

    auto get() -> settings&
    {
        return s_settings;
    }

    auto exists() -> bool
    {
        return std::filesystem::exists(s_config_path);
    }

    auto get_path() -> const std::filesystem::path&
    {
        return s_config_path;
    }
}
