#pragma once

#include <string>
#include <filesystem>

namespace animus_injector::config
{
    enum class game
    {
        acb,
        acr,
        ac3,
        unknown
    };

    struct credentials
    {
        std::string username;
        std::string password;
    };

    struct network_settings
    {
        std::string redirect_host;
        // redirect is always enabled - no option to disable

        // AC3 only: host[:port] the Uplay proxy's friend requests go to; empty = redirect_host
        std::string uplay_proxy;
    };

    struct logging_settings
    {
        bool enabled = true;
        std::string level = "info";
    };

    struct fix_settings
    {
        bool fix_cpu_affinity = true;
        bool fix_xinput_detection = false;
        bool fix_disable_punkbuster = false;
        bool fix_skip_intro_videos = false;
    };

    struct settings
    {
        credentials creds;
        network_settings net;
        logging_settings log;
        fix_settings fix;
    };

    [[nodiscard]] auto initialize(const std::filesystem::path& config_path) -> bool;
    [[nodiscard]] auto was_created() -> bool;
    [[nodiscard]] auto load() -> bool;
    [[nodiscard]] auto save() -> bool;
    [[nodiscard]] auto get() -> settings&;
    [[nodiscard]] auto exists() -> bool;
    [[nodiscard]] auto get_path() -> const std::filesystem::path&;
    [[nodiscard]] auto get_game() -> game;
}