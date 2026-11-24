#pragma once

#include <cstdint>
#include <string>

namespace openstone {

struct KeyboardEvent {
    std::int32_t code = 0; // Windows virtual-key code on every platform.
    bool pressed = false;
    std::int32_t repeat = 1;
    std::int32_t flags = 0; // High word of a Windows keystroke message.
    std::string character;
};

// Maps a logical ASCII key to its Windows virtual-key code.
[[nodiscard]] std::int32_t asciiVirtualKey(int key) noexcept;

}
