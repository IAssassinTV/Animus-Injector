#pragma once

#include <string>
#include <string_view>
#include <filesystem>

namespace animus_injector::logger
{
    [[nodiscard]] auto initialize(const std::filesystem::path& log_path, 
                                  bool enabled, 
                                  std::string_view level) -> bool;
    auto shutdown() -> void;
    
    auto debug(std::string_view message) -> void;
    auto info(std::string_view message) -> void;
    auto warn(std::string_view message) -> void;
    auto error(std::string_view message) -> void;
}
