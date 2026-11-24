#include "Schematic.hpp"

#include <algorithm>
#include <charconv>

namespace openstone {
namespace {

constexpr char upperAscii(char letter)
{
    return letter >= 'a' && letter <= 'z' ? letter - 'a' + 'A' : letter;
}

std::int32_t keyCode(std::string_view key)
{
    const auto first = key.find_first_not_of(" \t\r\n");
    if (first == key.npos) return 0;
    key = key.substr(first, key.find_last_not_of(" \t\r\n") - first + 1);

    std::int32_t code = 0;
    const auto result = std::from_chars(key.data(), key.data() + key.size(), code);
    if (result.ec == std::errc{} && result.ptr == key.data() + key.size()) return code;
    if (key.size() == 1) return asciiVirtualKey(static_cast<unsigned char>(key.front()));

    std::string name(key);
    std::ranges::transform(name, name.begin(), upperAscii);
    constexpr std::pair<std::string_view, std::int32_t> names[] = {
        {"SHIFT", 0x10}, {"CTRL", 0x11}, {"CONTROL", 0x11}, {"ALT", 0x12},
        {"ESC", 0x1b}, {"ESCAPE", 0x1b}, {"SPACE", 0x20},
        {"ENTER", 0x0d}, {"RETURN", 0x0d}, {"TAB", 0x09},
        {"LEFT", 0x25}, {"ARROW LEFT", 0x25}, {"UP", 0x26}, {"ARROW UP", 0x26},
        {"RIGHT", 0x27}, {"ARROW RIGHT", 0x27}, {"DOWN", 0x28}, {"ARROW DOWN", 0x28},
    };
    if (const auto* match = std::ranges::find(names, name, &std::pair<std::string_view, std::int32_t>::first);
        match != std::end(names)) {
        return match->second;
    }
    return 0;
}

}

std::int32_t asciiVirtualKey(int key) noexcept
{
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return key;
    if (key >= 'a' && key <= 'z') return upperAscii(static_cast<char>(key));
    switch (key) {
    case ' ': return 0x20;
    case ')': return '0';
    case '!': return '1';
    case '@': return '2';
    case '#': return '3';
    case '$': return '4';
    case '%': return '5';
    case '^': return '6';
    case '&': return '7';
    case '*': return '8';
    case '(': return '9';
    case ';': case ':': return 0xba;
    case '=': case '+': return 0xbb;
    case ',': case '<': return 0xbc;
    case '-': case '_': return 0xbd;
    case '.': case '>': return 0xbe;
    case '/': case '?': return 0xbf;
    case '`': case '~': return 0xc0;
    case '[': case '{': return 0xdb;
    case '\\': case '|': return 0xdc;
    case ']': case '}': return 0xdd;
    case '\'': case '"': return 0xde;
    default: return 0;
    }
}

bool keyIsPressed(const std::bitset<256>& pressed, std::string_view key)
{
    const auto code = keyCode(key);
    if (code <= 0 || static_cast<std::size_t>(code) >= pressed.size()) return false;
    // A generic modifier query also matches either side when the host distinguishes them.
    switch (code) {
    case 0x10: return pressed[code] || pressed[0xa0] || pressed[0xa1];
    case 0x11: return pressed[code] || pressed[0xa2] || pressed[0xa3];
    case 0x12: return pressed[code] || pressed[0xa4] || pressed[0xa5];
    default: return pressed[code];
    }
}

bool Schematic::isKeyPressed(std::string_view key) const
{
    return keyIsPressed(rootGraph().pressedKeys_, key);
}

bool Schematic::sendKeyEvent(const KeyboardEvent& event)
{
    auto& root = rootGraph();
    if (event.code <= 0 || static_cast<std::size_t>(event.code) >= root.pressedKeys_.size()) return false;
    root.pressedKeys_.set(event.code, event.pressed);
    const bool dispatched = root.dispatchKeyEvent(event);
    if (dispatched) root.refreshConnections();
    return dispatched;
}

void Schematic::clearPressedKeys() noexcept
{
    rootGraph().pressedKeys_.reset();
}

bool Schematic::dispatchKeyEvent(const KeyboardEvent& event)
{
    const auto kind = event.pressed ? PrimitiveKind::KeyDown : PrimitiveKind::KeyUp;
    bool dispatched = false;
    for (auto& entry : nodes_) {
        if (entry.kind != kind || !std::get<bool>(readInput(entry.id, 0))) continue;
        const auto codes = std::get<green::IntArray>(readInput(entry.id, 1));
        if (!codes.empty() && !std::ranges::contains(codes, event.code)) continue;
        // Populate all four readouts before any downstream trigger can pull them.
        entry.capturedSignals = {event.code, event.repeat, event.flags, event.character};
        fireOutputs(entry.id);
        dispatched = true;
    }
    for (auto& [id, child] : modules_) dispatched |= child->dispatchKeyEvent(event);
    return dispatched;
}

}
