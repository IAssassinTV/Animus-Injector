#include "gui.h"
#include "claudia.h"
#include "config.h"
#include "logger.h"

#include <CommCtrl.h>
#include <shellapi.h>
#include <array>
#include <format>

// check if we are running with admin perms
[[nodiscard]] bool is_elevated()
{
    BOOL elevated = FALSE;
    HANDLE token = nullptr;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);

        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
        {
            elevated = elevation.TokenIsElevated;
        }
        CloseHandle(token);
    }

    return elevated != FALSE;
}

#pragma comment(lib, "comctl32.lib")

namespace claudia::gui
{
    namespace
    {
        // control identifiers
        enum control_id : int
        {
            idc_static_title = 100,
            idc_static_username,
            idc_static_password,
            idc_edit_username,
            idc_edit_password,
            idc_check_remember,
            idc_btn_launch,
            idc_btn_cancel,
            idc_btn_firewall
        };

        // layout constants
        constexpr int DIALOG_WIDTH = 320;
        constexpr int DIALOG_HEIGHT = 235;
        constexpr int MARGIN = 12;
        constexpr int LABEL_WIDTH = 70;
        constexpr int EDIT_HEIGHT = 22;
        constexpr int BUTTON_WIDTH = 80;
        constexpr int BUTTON_HEIGHT = 26;
        constexpr int CHECKBOX_HEIGHT = 20;

        const wchar_t* const WINDOW_CLASS = L"ClaudiaConfigDialog";
        const wchar_t* const WINDOW_TITLE = L"Claudia";

        // state
        HINSTANCE s_instance = nullptr;
        result s_dialog_result = result::cancel;
        HFONT s_font = nullptr;
        HFONT s_title_font = nullptr;

        // forward declarations
        LRESULT CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
        void create_controls(HWND hwnd);
        void center_window(HWND hwnd);
        bool validate_input(HWND hwnd);
        void save_settings(HWND hwnd);
        void setup_firewall_rules();

        LRESULT CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            switch (msg)
            {
            case WM_CREATE:
                create_controls(hwnd);
                return 0;

            case WM_COMMAND:
                switch (LOWORD(wparam))
                {
                case idc_btn_launch:
                    if (validate_input(hwnd))
                    {
                        save_settings(hwnd);
                        s_dialog_result = result::launch;
                        DestroyWindow(hwnd);
                    }
                    return 0;

                case idc_btn_cancel:
                    s_dialog_result = result::cancel;
                    DestroyWindow(hwnd);
                    return 0;

                case idc_btn_firewall:
                    setup_firewall_rules();
                    return 0;
                }
                break;

            case WM_CLOSE:
                s_dialog_result = result::cancel;
                DestroyWindow(hwnd);
                return 0;

            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }

            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        void create_controls(HWND hwnd)
        {
            int y = MARGIN;
            const int edit_width = DIALOG_WIDTH - MARGIN * 2 - LABEL_WIDTH - 24;

            // title label
            if (HWND h_title = CreateWindowW(
                    L"STATIC",
                    L"Enter your account credentials",
                    WS_CHILD | WS_VISIBLE | SS_CENTER,
                    MARGIN, y, DIALOG_WIDTH - MARGIN * 2 - 16, 20,
                    hwnd, reinterpret_cast<HMENU>(idc_static_title),
                    s_instance, nullptr))
            {
                SendMessageW(h_title, WM_SETFONT, reinterpret_cast<WPARAM>(s_title_font), TRUE);
            }
            y += 30;

            // username label
            if (HWND h_label = CreateWindowW(
                    L"STATIC", L"Username:",
                    WS_CHILD | WS_VISIBLE | SS_RIGHT,
                    MARGIN, y + 2, LABEL_WIDTH, EDIT_HEIGHT,
                    hwnd, reinterpret_cast<HMENU>(idc_static_username),
                    s_instance, nullptr))
            {
                SendMessageW(h_label, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
            }

            // username edit
            HWND h_edit_user = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                MARGIN + LABEL_WIDTH + 8, y, edit_width, EDIT_HEIGHT,
                hwnd, reinterpret_cast<HMENU>(idc_edit_username),
                s_instance, nullptr);
            
            if (h_edit_user)
            {
                SendMessageW(h_edit_user, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
                SendMessageW(h_edit_user, EM_SETLIMITTEXT, 64, 0);

                if (const auto& settings = config::get(); !settings.creds.username.empty())
                {
                    SetWindowTextA(h_edit_user, settings.creds.username.c_str());
                }
            }
            y += EDIT_HEIGHT + 10;

            // password label
            if (HWND h_label = CreateWindowW(
                    L"STATIC", L"Password:",
                    WS_CHILD | WS_VISIBLE | SS_RIGHT,
                    MARGIN, y + 2, LABEL_WIDTH, EDIT_HEIGHT,
                    hwnd, reinterpret_cast<HMENU>(idc_static_password),
                    s_instance, nullptr))
            {
                SendMessageW(h_label, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
            }

            // password edit
            HWND h_edit_pass = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
                MARGIN + LABEL_WIDTH + 8, y, edit_width, EDIT_HEIGHT,
                hwnd, reinterpret_cast<HMENU>(idc_edit_password),
                s_instance, nullptr);
            
            if (h_edit_pass)
            {
                SendMessageW(h_edit_pass, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
                SendMessageW(h_edit_pass, EM_SETLIMITTEXT, 64, 0);

                if (const auto& settings = config::get(); !settings.creds.password.empty())
                {
                    SetWindowTextA(h_edit_pass, settings.creds.password.c_str());
                }
            }
            y += EDIT_HEIGHT + 12;

            // remember checkbox
            if (HWND h_check = CreateWindowW(
                    L"BUTTON", L"Don't ask again",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    MARGIN + LABEL_WIDTH + 8, y, 120, CHECKBOX_HEIGHT,
                    hwnd, reinterpret_cast<HMENU>(idc_check_remember),
                    s_instance, nullptr))
            {
                SendMessageW(h_check, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);

                const auto& settings = config::get();
                SendMessageW(h_check, BM_SETCHECK,
                             settings.ui.skip_config_dialog ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            y += CHECKBOX_HEIGHT + 8;

            // firewall setup button
            if (HWND h_btn = CreateWindowW(
                    L"BUTTON", L"Setup Firewall...",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    MARGIN + LABEL_WIDTH + 8, y, 120, BUTTON_HEIGHT,
                    hwnd, reinterpret_cast<HMENU>(idc_btn_firewall),
                    s_instance, nullptr))
            {
                SendMessageW(h_btn, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
            }

            // button positioning
            const int btn_y = DIALOG_HEIGHT - BUTTON_HEIGHT - MARGIN - 32;
            const int total_btn_width = BUTTON_WIDTH * 2 + 8;
            const int btn_x = DIALOG_WIDTH - MARGIN - total_btn_width - 8;

            // cancel button
            if (HWND h_btn = CreateWindowW(
                    L"BUTTON", L"Cancel",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    btn_x, btn_y, BUTTON_WIDTH, BUTTON_HEIGHT,
                    hwnd, reinterpret_cast<HMENU>(idc_btn_cancel),
                    s_instance, nullptr))
            {
                SendMessageW(h_btn, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
            }

            // launch button
            if (HWND h_btn = CreateWindowW(
                    L"BUTTON", L"Launch",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    btn_x + BUTTON_WIDTH + 8, btn_y, BUTTON_WIDTH, BUTTON_HEIGHT,
                    hwnd, reinterpret_cast<HMENU>(idc_btn_launch),
                    s_instance, nullptr))
            {
                SendMessageW(h_btn, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
            }

            // credit text at the bottom left
            if (HWND h_credit = CreateWindowW(
                    L"STATIC", L"made by siohaza",
                    WS_CHILD | WS_VISIBLE,
                    MARGIN, btn_y + 4, 110, 18,
                    hwnd, nullptr,
                    s_instance, nullptr))
            {
                SendMessageW(h_credit, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
            }

            SetFocus(h_edit_user);
        }

        void center_window(HWND hwnd)
        {
            RECT rc;
            GetWindowRect(hwnd, &rc);

            const int width = rc.right - rc.left;
            const int height = rc.bottom - rc.top;
            const int screen_width = GetSystemMetrics(SM_CXSCREEN);
            const int screen_height = GetSystemMetrics(SM_CYSCREEN);

            SetWindowPos(hwnd, nullptr,
                         (screen_width - width) / 2,
                         (screen_height - height) / 2,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }

        bool validate_input(HWND hwnd)
        {
            std::array<char, 65> username{};
            std::array<char, 65> password{};

            GetDlgItemTextA(hwnd, idc_edit_username, username.data(), static_cast<int>(username.size()));
            GetDlgItemTextA(hwnd, idc_edit_password, password.data(), static_cast<int>(password.size()));

            if (username[0] == '\0')
            {
                show_error("Error", "Please enter your username.");
                SetFocus(GetDlgItem(hwnd, idc_edit_username));
                return false;
            }

            if (password[0] == '\0')
            {
                show_error("Error", "Please enter your password.");
                SetFocus(GetDlgItem(hwnd, idc_edit_password));
                return false;
            }

            return true;
        }

        void save_settings(HWND hwnd)
        {
            std::array<char, 65> username{};
            std::array<char, 65> password{};

            GetDlgItemTextA(hwnd, idc_edit_username, username.data(), static_cast<int>(username.size()));
            GetDlgItemTextA(hwnd, idc_edit_password, password.data(), static_cast<int>(password.size()));

            auto& settings = config::get();
            settings.creds.username = username.data();
            settings.creds.password = password.data();

            const LRESULT check_state = SendDlgItemMessageW(hwnd, idc_check_remember, BM_GETCHECK, 0, 0);
            settings.ui.skip_config_dialog = (check_state == BST_CHECKED);

            (void)config::save();
            logger::info("settings saved");
        }

        void setup_firewall_rules()
        {
            // get the path to ACBMP.exe
            const auto base_path = claudia::get_base_path();
            const auto game_path = base_path / L"ACBMP.exe";

            if (!std::filesystem::exists(game_path))
            {
                show_error("Error", "ACBMP.exe not found. Cannot configure firewall.");
                return;
            }

            logger::info("attempting firewall setup");

            // check if running as admin
            if (!is_elevated())
            {
                logger::info("not running as admin - cannot configure firewall");
                show_info("Admin permissions are required!",
                    "To configure firewall rules, run the game as Administrator once.\n\n"
                    "Right click on ACBMP.exe -> Run as administrator");
                return;
            }

            logger::info("running as admin, configuring firewall...");

            // delete existing rules first
            std::wstring delete_cmd = L"netsh advfirewall firewall delete rule name=\"_ACBMP_P2P\"";
            _wsystem(delete_cmd.c_str());

            // add inbound rule
            std::wstring in_cmd = std::format(
                L"netsh advfirewall firewall add rule name=\"_ACBMP_P2P\" dir=in action=allow program=\"{}\" enable=yes profile=any",
                game_path.wstring()
            );
            int in_result = _wsystem(in_cmd.c_str());

            // add outbound rule
            std::wstring out_cmd = std::format(
                L"netsh advfirewall firewall add rule name=\"_ACBMP_P2P\" dir=out action=allow program=\"{}\" enable=yes profile=any",
                game_path.wstring()
            );
            int out_result = _wsystem(out_cmd.c_str());

            if (in_result == 0 && out_result == 0)
            {
                logger::info("firewall rules configured successfully");
                show_info("Success", "Firewall rules have been configured. Enjoy the game!");
            }
            else
            {
                logger::error(std::format("firewall setup failed: in={}, out={}", in_result, out_result));
                show_error("Error", "Failed to configure firewall rules.");
            }
        }
    }

    auto initialize(HINSTANCE instance) -> bool
    {
        s_instance = instance;

        INITCOMMONCONTROLSEX icc = {
            .dwSize = sizeof(icc),
            .dwICC = ICC_WIN95_CLASSES
        };
        InitCommonControlsEx(&icc);

        NONCLIENTMETRICSW ncm = { .cbSize = sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);

        s_font = CreateFontIndirectW(&ncm.lfMessageFont);

        ncm.lfMessageFont.lfWeight = FW_BOLD;
        ncm.lfMessageFont.lfHeight = -14;
        s_title_font = CreateFontIndirectW(&ncm.lfMessageFont);

        logger::info("gui initialized");
        return true;
    }

    auto show_config_dialog() -> result
    {
        s_dialog_result = result::cancel;

        WNDCLASSEXW wc = {
            .cbSize = sizeof(wc),
            .style = CS_HREDRAW | CS_VREDRAW,
            .lpfnWndProc = dialog_proc,
            .hInstance = s_instance,
            .hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),  // IDI_APPLICATION
            .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),  // IDC_ARROW
            .hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1),
            .lpszClassName = WINDOW_CLASS
        };

        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            logger::error("failed to register window class");
            return result::error;
        }

        HWND hwnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            WINDOW_CLASS,
            WINDOW_TITLE,
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT,
            DIALOG_WIDTH, DIALOG_HEIGHT,
            nullptr, nullptr, s_instance, nullptr);

        if (!hwnd)
        {
            logger::error("failed to create dialog window");
            return result::error;
        }

        center_window(hwnd);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0))
        {
            if (!IsDialogMessageW(hwnd, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (!IsWindow(hwnd))
                break;
        }

        return s_dialog_result;
    }

    auto show_error(std::string_view title, std::string_view message) -> void
    {
        MessageBoxA(nullptr, message.data(), title.data(), MB_OK | MB_ICONERROR | MB_TOPMOST);
    }

    auto show_info(std::string_view title, std::string_view message) -> void
    {
        MessageBoxA(nullptr, message.data(), title.data(), MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }
}
