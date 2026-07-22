#pragma once

#include <string>
#include <filesystem>

namespace claudia::config
{
    struct credentials
    {
        std::string username;
        std::string password;
    };

    struct network_settings
    {
        std::string redirect_host;
        // redirect is always enabled - no option to disable
    };

    struct logging_settings
    {
        bool enabled = true;
        std::string level = "info";
    };

    struct ui_settings
    {
        bool skip_config_dialog = false;
    };

    struct fix_settings
    {
        bool fix_cpu_affinity = true;
        bool fix_xinput_detection = true;
        bool fix_disable_punkbuster = true;
    };

    struct settings
    {
        credentials creds;
        network_settings net;
        logging_settings log;
        ui_settings ui;
        fix_settings fix;
    };

    [[nodiscard]] auto initialize(const std::filesystem::path& config_path) -> bool;
    [[nodiscard]] auto load() -> bool;
    [[nodiscard]] auto save() -> bool;
    [[nodiscard]] auto get() -> settings&;
    [[nodiscard]] auto exists() -> bool;
    [[nodiscard]] auto get_path() -> const std::filesystem::path&;
}
