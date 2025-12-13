#pragma once

#include <string_view>

namespace claudia::launcher
{
    [[nodiscard]] auto game_exists() -> bool;
    [[nodiscard]] auto has_credentials() -> bool;
    [[nodiscard]] auto launch_game(std::string_view username, std::string_view password) -> bool;
}
