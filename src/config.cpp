#include "config.h"
#include "animus_injector.h"
#include "logger.h"

#define INIPP_IMPLEMENTATION
#include <inipp/inipp.h>

#include <cwchar>
#include <fstream>
#include <format>
#include <string_view>

namespace animus_injector::config
{
    namespace
    {
        settings s_settings;
        std::filesystem::path s_config_path;
        game s_game = game::unknown;
        bool s_initialized = false;
        bool s_created = false;

        [[nodiscard]] constexpr bool parse_bool(std::string_view str) noexcept
        {
            return str == "true" || str == "1" || str == "yes";
        }

        [[nodiscard]] game detect_game()
        {
            const auto exe = animus_injector::get_game_executable();
            if (exe.empty())
                return game::unknown;

            if (_wcsicmp(exe.c_str(), animus_injector::GAME_ACB.data()) == 0)
                return game::acb;
            if (_wcsicmp(exe.c_str(), animus_injector::GAME_ACR.data()) == 0)
                return game::acr;
            if (_wcsicmp(exe.c_str(), animus_injector::GAME_AC3.data()) == 0)
                return game::ac3;

            return game::unknown;
        }

        [[nodiscard]] settings make_defaults(game g)
        {
            settings s{};
            s.net.redirect_host = std::string(DEFAULT_REDIRECT_HOST);
            s.log.enabled = true;
            s.log.level = "info";
            s.fix.fix_cpu_affinity = true;

            switch (g)
            {
            case game::acb:
                s.fix.fix_xinput_detection = true;
                s.fix.fix_disable_punkbuster = true;
                break;
            case game::acr:
                s.fix.fix_xinput_detection = true;
                break;
            case game::ac3:
                s.fix.fix_skip_intro_videos = true;
                break;
            default:
                s.fix.fix_xinput_detection = true;
                s.fix.fix_disable_punkbuster = true;
                s.fix.fix_skip_intro_videos = true;
                break;
            }

            return s;
        }

        [[nodiscard]] bool fix_key_active(game g, std::string_view key) noexcept
        {
            if (key == "FixCpuAffinity")
                return true;
            if (key == "FixXInputDetection")
                return g == game::acb || g == game::acr || g == game::unknown;
            if (key == "FixDisablePunkBuster")
                return g == game::acb || g == game::unknown;
            if (key == "FixSkipIntroVideos")
                return g == game::ac3 || g == game::unknown;
            return false;
        }
    }

    auto initialize(const std::filesystem::path& config_path) -> bool
    {
        s_config_path = config_path;
        s_game = detect_game();
        s_settings = make_defaults(s_game);
        s_initialized = true;

        // remember whether the file was freshly created so the launcher can
        // abort on first run (the user must review the generated defaults first)
        s_created = !exists();
        if (s_created)
            return save();

        return true;
    }

    auto was_created() -> bool
    {
        return s_created;
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

            // credentials (ACB launches multiplayer via these command-line args)
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

            // fixes (only keys that apply to the detected game are honoured)
            if (fix_key_active(s_game, "FixCpuAffinity"))
            {
                if (std::string value;
                    inipp::get_value(ini.sections["Fixes"], "FixCpuAffinity", value))
                {
                    s_settings.fix.fix_cpu_affinity = parse_bool(value);
                }
            }
            if (fix_key_active(s_game, "FixXInputDetection"))
            {
                if (std::string value;
                    inipp::get_value(ini.sections["Fixes"], "FixXInputDetection", value))
                {
                    s_settings.fix.fix_xinput_detection = parse_bool(value);
                }
            }
            if (fix_key_active(s_game, "FixDisablePunkBuster"))
            {
                if (std::string value;
                    inipp::get_value(ini.sections["Fixes"], "FixDisablePunkBuster", value))
                {
                    s_settings.fix.fix_disable_punkbuster = parse_bool(value);
                }
            }
            if (fix_key_active(s_game, "FixSkipIntroVideos"))
            {
                if (std::string value;
                    inipp::get_value(ini.sections["Fixes"], "FixSkipIntroVideos", value))
                {
                    s_settings.fix.fix_skip_intro_videos = parse_bool(value);
                }
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

            // credentials are only meaningful to ACB's command-line relaunch
            if (s_game == game::acb)
            {
                file << "[Credentials]\n";
                file << std::format("Username={}\n", s_settings.creds.username);
                file << std::format("Password={}\n", s_settings.creds.password);
                file << '\n';
            }

            file << "[Network]\n";
            file << std::format("RedirectHost={}\n", s_settings.net.redirect_host);
            file << '\n';

            file << "[Logging]\n";
            file << std::format("Enabled={}\n", s_settings.log.enabled ? "true" : "false");
            file << std::format("Level={}\n", s_settings.log.level);
            file << '\n';

            file << "[Fixes]\n";
            if (fix_key_active(s_game, "FixCpuAffinity"))
                file << std::format("FixCpuAffinity={}\n", s_settings.fix.fix_cpu_affinity ? "true" : "false");
            if (fix_key_active(s_game, "FixXInputDetection"))
                file << std::format("FixXInputDetection={}\n", s_settings.fix.fix_xinput_detection ? "true" : "false");
            if (fix_key_active(s_game, "FixDisablePunkBuster"))
                file << std::format("FixDisablePunkBuster={}\n", s_settings.fix.fix_disable_punkbuster ? "true" : "false");
            if (fix_key_active(s_game, "FixSkipIntroVideos"))
                file << std::format("FixSkipIntroVideos={}\n", s_settings.fix.fix_skip_intro_videos ? "true" : "false");

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

    auto get_game() -> game
    {
        return s_game;
    }
}