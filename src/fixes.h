#pragma once

namespace animus_injector::fixes
{
    [[nodiscard]] auto initialize() -> bool;
    auto shutdown() -> void;
    [[nodiscard]] auto is_active() -> bool;
}
