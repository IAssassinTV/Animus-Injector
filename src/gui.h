#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include <string_view>

namespace claudia::gui
{
    enum class result
    {
        launch,
        cancel,
        error
    };

    [[nodiscard]] auto initialize(HINSTANCE instance) -> bool;
    [[nodiscard]] auto show_config_dialog() -> result;
    
    auto show_error(std::string_view title, std::string_view message) -> void;
    auto show_info(std::string_view title, std::string_view message) -> void;
}
