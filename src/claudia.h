#pragma once

#include <string>
#include <string_view>
#include <filesystem>

// version info
inline constexpr std::string_view CLAUDIA_VERSION = "0.1.2";
inline constexpr std::string_view CLAUDIA_NAME = "Claudia";

// network defaults
inline constexpr std::string_view ORIGINAL_HOST = "onlineconfigservice.ubi.com";
inline constexpr std::string_view DEFAULT_REDIRECT_HOST = "animusnetwork.com";

// file names
inline constexpr std::string_view CONFIG_FILENAME = "claudia.ini";

namespace claudia
{
    [[nodiscard]] std::filesystem::path get_base_path();
}
