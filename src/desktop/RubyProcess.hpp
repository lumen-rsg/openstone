#pragma once

#include "RubyCode.hpp"

namespace openstone::desktop {

enum class RubySoundMode { Async, Sync, Loop };
using RubySoundBackend = std::function<void(std::optional<std::string> file, RubySoundMode mode)>;

[[nodiscard]] bool rubyAvailable();
[[nodiscard]] RubyBackend makeRubyBackend(RubySoundBackend sound = {});

}
