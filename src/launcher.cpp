#include "launcher.h"
#include "claudia.h"
#include "logger.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <format>
#include <string>

namespace claudia::launcher
{
    namespace
    {
        const wchar_t* const GAME_EXECUTABLE = L"ACBMP.exe";
    }

    auto game_exists() -> bool
    {
        const auto base_path = claudia::get_base_path();
        const auto game_path = base_path / GAME_EXECUTABLE;
        
        if (std::filesystem::exists(game_path))
        {
            logger::info(std::format("game found: {}", game_path.string()));
            return true;
        }
        
        return false;
    }

    auto has_credentials() -> bool
    {
        // check if launched with /onlineUser and /onlinePassword params
        const std::wstring cmd_line = GetCommandLineW();
        return cmd_line.find(L"/onlineUser:") != std::wstring::npos &&
               cmd_line.find(L"/onlinePassword:") != std::wstring::npos;
    }

    auto launch_game(std::string_view username, std::string_view password) -> bool
    {
        const auto base_path = claudia::get_base_path();
        const auto game_path = base_path / GAME_EXECUTABLE;

        // build command line with credentials
        auto cmd_line = std::format(
            "\"{}\" /onlineUser:{} /onlinePassword:{}",
            game_path.string(), username, password
        );

        logger::debug(std::format("launching: {}", cmd_line));

        STARTUPINFOA si = { .cb = sizeof(si) };
        PROCESS_INFORMATION pi{};

        if (!CreateProcessA(
                nullptr,
                cmd_line.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                base_path.string().c_str(),
                &si,
                &pi))
        {
            logger::error(std::format("CreateProcess failed: {}", GetLastError()));
            return false;
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return true;
    }
}
