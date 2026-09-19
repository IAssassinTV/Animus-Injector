#pragma once

namespace animus_injector::hooks
{
    [[nodiscard]] auto initialize() -> bool;
    auto shutdown() -> void;
    [[nodiscard]] auto is_active() -> bool;
}
