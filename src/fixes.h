#pragma once

namespace claudia::fixes
{
    [[nodiscard]] auto initialize() -> bool;
    auto shutdown() -> void;
    [[nodiscard]] auto is_active() -> bool;
}
