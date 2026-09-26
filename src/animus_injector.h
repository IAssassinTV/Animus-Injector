#pragma once

#include <array>
#include <string>
#include <string_view>
#include <filesystem>

// version info
inline constexpr std::string_view ANIMUS_INJECTOR_VERSION = "1.0.5";
inline constexpr std::string_view ANIMUS_INJECTOR_NAME = "Animus Injector";

// network defaults
inline constexpr std::string_view ORIGINAL_HOST = "onlineconfigservice.ubi.com";
inline constexpr std::string_view DEFAULT_REDIRECT_HOST = "animusnetwork.com";
// AC3: the Uplay proxy's friend service on the game server (TCP, same port number as its UDP auth server)
inline constexpr std::string_view DEFAULT_UPLAY_PROXY = "animusnetwork.com:21006";

// file names
inline constexpr std::string_view CONFIG_FILENAME = "AnimusInjector.ini";
inline constexpr std::string_view LOG_FILENAME = "AnimusInjector.log";

namespace animus_injector
{
    // supported game executables
    inline constexpr std::wstring_view GAME_ACB = L"ACBMP.exe";
    inline constexpr std::wstring_view GAME_ACR = L"ACRMP.exe";
    inline constexpr std::wstring_view GAME_AC3 = L"AC3MP.exe";

    inline constexpr std::array<std::wstring_view, 3> SUPPORTED_GAMES = {
        GAME_ACB, GAME_ACR, GAME_AC3
    };

    [[nodiscard]] std::filesystem::path get_base_path();
    [[nodiscard]] std::wstring get_game_executable();
}