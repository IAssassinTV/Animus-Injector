#include "animus_injector.h"
#include "config.h"
#include "logger.h"
#include "hooks.h"
#include "fixes.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace
{
    void init();

    DWORD WINAPI initialize_thread([[maybe_unused]] LPVOID lpParam)
    {
        init();
        return 0;
    }

    [[nodiscard]] std::wstring get_current_exe_name()
    {
        wchar_t module_path[MAX_PATH];
        if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0)
            return L"";

        const wchar_t* filename = wcsrchr(module_path, L'\\');
        filename = filename ? filename + 1 : module_path;

        return std::wstring(filename);
    }

    [[nodiscard]] bool is_target_process()
    {
        const auto exe_name = get_current_exe_name();
        if (exe_name.empty())
            return false;

        for (const auto& game : animus_injector::SUPPORTED_GAMES)
        {
            if (_wcsicmp(exe_name.c_str(), game.data()) == 0)
                return true;
        }

        return false;
    }

    [[nodiscard]] bool has_credential_arguments()
    {
        const std::wstring command_line = GetCommandLineW();
        return command_line.find(L"/onlineUser:") != std::wstring::npos &&
               command_line.find(L"/onlinePassword:") != std::wstring::npos;
    }

    [[nodiscard]] bool relaunch_with_credentials(std::string_view username, std::string_view password)
    {
        wchar_t module_path[MAX_PATH];
        if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0)
            return false;

        const auto base_path = std::filesystem::path(module_path).parent_path();
        const auto game_path = base_path / get_current_exe_name();

        const std::wstring username_wide(username.begin(), username.end());
        const std::wstring password_wide(password.begin(), password.end());

        std::wstring command_line = std::format(
            L"\"{}\" /onlineUser:{} /onlinePassword:{}",
            game_path.wstring(), username_wide, password_wide);

        animus_injector::logger::debug("relaunching game with command-line credentials");

        STARTUPINFOW si = { .cb = sizeof(si) };
        PROCESS_INFORMATION pi{};

        if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, base_path.wstring().c_str(), &si, &pi))
        {
            animus_injector::logger::error(std::format("failed to relaunch game: {}", GetLastError()));
            return false;
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }

    void init()
    {
        const auto base_path = animus_injector::get_base_path();
        const auto config_path = base_path / std::string(CONFIG_FILENAME);

        if (!animus_injector::config::initialize(config_path))
        {
            animus_injector::logger::error("failed to initialize configuration");
            return;
        }

        (void)animus_injector::config::load();

        const auto& settings = animus_injector::config::get();
        const auto log_path = base_path / std::string(LOG_FILENAME);

        if (!animus_injector::logger::initialize(log_path, settings.log.enabled, settings.log.level))
        {
            animus_injector::logger::error("failed to initialize logger");
            return;
        }

        animus_injector::logger::info("starting initialization");
        animus_injector::logger::info(std::format("base path: {}", base_path.string()));

        // first launch: generate the per-game defaults, then exit so the user
        // can review AnimusInjector.ini before actually starting the game.
        if (animus_injector::config::was_created())
        {
            animus_injector::logger::info(
                "first launch: created AnimusInjector.ini with defaults; "
                "aborting launch - review it and start the game again");
            ExitProcess(0);
            return;
        }

        // ACB authenticates over the command line; relaunch with the configured
        // credentials if this instance was started without them.
        if (animus_injector::config::get_game() == animus_injector::config::game::acb &&
            !has_credential_arguments())
        {
            if (settings.creds.username.empty() || settings.creds.password.empty())
            {
                animus_injector::logger::error(
                    "ACB credentials missing: add [Credentials] Username/Password to "
                    "AnimusInjector.ini or launch ACBMP.exe with /onlineUser: and /onlinePassword:");
                ExitProcess(1);
                return;
            }

            animus_injector::logger::info("relaunching ACBMP.exe with AnimusInjector.ini credentials");
            if (relaunch_with_credentials(settings.creds.username, settings.creds.password))
            {
                animus_injector::logger::info("game relaunched; exiting current instance");
                ExitProcess(0);
            }
            else
            {
                animus_injector::logger::error("failed to relaunch game with credentials");
            }
        }

        if (!animus_injector::hooks::initialize())
        {
            animus_injector::logger::error("failed to initialize hooks");
            return;
        }

        if (!animus_injector::fixes::initialize())
        {
            animus_injector::logger::warn("some game fixes could not be applied");
        }

        animus_injector::logger::info("initialization complete");
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, [[maybe_unused]] LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        // early exit if not the target process
        if (!is_target_process())
            return TRUE;

        // Wine's XInputEnable() starts a worker thread and waits for it.
        // Run ACR initialization only on a dedicated thread, after DllMain
        // releases the loader lock, so that XInput's worker can start.
        if (_wcsicmp(get_current_exe_name().c_str(), animus_injector::GAME_ACR.data()) == 0)
        {
            if (HANDLE thread = CreateThread(nullptr, 0, initialize_thread, nullptr, 0, nullptr))
                CloseHandle(thread);
        }
        else
        {
            init();
        }
        break;

    case DLL_PROCESS_DETACH:
        animus_injector::fixes::shutdown();
        animus_injector::hooks::shutdown();
        animus_injector::logger::shutdown();
        break;
    }
    return TRUE;
}

namespace animus_injector
{
    std::filesystem::path get_base_path()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    std::wstring get_game_executable()
    {
        return get_current_exe_name();
    }
}
