#pragma once

namespace claudia::hooks
{
    [[nodiscard]] auto initialize() -> bool;
    auto shutdown() -> void;
    [[nodiscard]] auto is_active() -> bool;
}
