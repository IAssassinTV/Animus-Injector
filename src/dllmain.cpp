#include "claudia.h"
#include "config.h"
#include "logger.h"
#include "gui.h"
#include "hooks.h"
#include "fixes.h"
#include "launcher.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <filesystem>
#include <format>
#include <string>

namespace
{
    HMODULE g_module = nullptr;

    void init()
    {
        const auto base_path = claudia::get_base_path();
        const auto config_path = base_path / std::string(CONFIG_FILENAME);

        if (!claudia::config::initialize(config_path))
        {
            claudia::gui::show_error("Claudia", "Failed to initialize configuration.");
            return;
        }

        (void)claudia::config::load();

        const auto& settings = claudia::config::get();
        const auto log_path = base_path / "claudia.log";

        if (!claudia::logger::initialize(log_path, settings.log.enabled, settings.log.level))
        {
            claudia::gui::show_error("Claudia", "Failed to initialize logger.");
            return;
        }

        claudia::logger::info("starting initialization");
        claudia::logger::info(std::format("base path: {}", base_path.string()));

        if (!claudia::launcher::game_exists())
        {
            claudia::logger::error("game executable not found");
            claudia::gui::show_error("Claudia", "Missing multiplayer game files.\n\nACBMP.exe was not found.");
            ExitProcess(1);
            return;
        }

        // if we do have credentials, then initialize hooks and fixes
        if (claudia::launcher::has_credentials())
        {
            claudia::logger::info("credentials found in command line");

            if (!claudia::hooks::initialize())
            {
                claudia::logger::error("failed to initialize hooks");
                claudia::gui::show_error("Claudia", "Failed to initialize network hooks.");
                return;
            }

            if (!claudia::fixes::initialize())
            {
                claudia::logger::warn("some game fixes could not be applied");
            }

            claudia::logger::info("initialization complete");
            return;
        }

        // no credentials, check if we should skip dialog
        if (settings.ui.skip_config_dialog && 
            !settings.creds.username.empty() && 
            !settings.creds.password.empty())
        {
            claudia::logger::info("skipping config dialog - launching with saved credentials");
        }
        else
        {
            claudia::logger::info("showing config dialog");

            if (!claudia::gui::initialize(g_module))
            {
                claudia::logger::error("failed to initialize gui");
                claudia::gui::show_error("Claudia", "Failed to initialize user interface.");
                ExitProcess(1);
                return;
            }

            const auto result = claudia::gui::show_config_dialog();

            if (result == claudia::gui::result::cancel)
            {
                claudia::logger::info("user cancelled");
                ExitProcess(0);
                return;
            }

            if (result == claudia::gui::result::error)
            {
                claudia::logger::error("dialog error");
                ExitProcess(1);
                return;
            }
        }

        // reload config to get any changes
        (void)claudia::config::load();
        const auto& updated_settings = claudia::config::get();

        claudia::logger::info("relaunching game with credentials");

        if (claudia::launcher::launch_game(updated_settings.creds.username, 
                                           updated_settings.creds.password))
        {
            claudia::logger::info("game relaunched exiting current instance");
            ExitProcess(0);
        }
        else
        {
            claudia::gui::show_error("Claudia", "Failed to launch the game.");
            ExitProcess(1);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, [[maybe_unused]] LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        g_module = hModule;
        init();
        break;

    case DLL_PROCESS_DETACH:
        claudia::fixes::shutdown();
        claudia::hooks::shutdown();
        claudia::logger::shutdown();
        break;
    }
    return TRUE;
}

namespace claudia
{
    std::filesystem::path get_base_path()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }
}
