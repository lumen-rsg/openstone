#include "Schematic.hpp"

#include <algorithm>

namespace openstone {
namespace {

constexpr int openInput = 0;
constexpr int widthInput = 1;
constexpr int heightInput = 2;
constexpr int typeInput = 3;
constexpr int titleInput = 4;
constexpr int viewOutput = 0;
constexpr int openOutput = 1;

int clientExtent(int value, int fallback)
{
    return value > 0 ? std::min(value, maximumWindowExtent) : fallback;
}

}

yellow::View Schematic::windowView(const SchematicNode& entry) const
{
    const auto state = entry.window.value_or(WindowState{});
    return {.panel = entry.id,
            .bounds = {0, 0, state.width / windowPixelsPerUnit, state.height / windowPixelsPerUnit},
            .mouseMoves = true, .schematicPath = path_};
}

bool Schematic::setWindowSize(SchematicNode& entry, int width, int height)
{
    auto& state = entry.window ? *entry.window : entry.window.emplace();
    width = std::clamp(width, 1, maximumWindowExtent);
    height = std::clamp(height, 1, maximumWindowExtent);
    if (state.width == width && state.height == height) {
        return false;
    }
    state.width = width;
    state.height = height;
    fireOutput(entry, viewOutput);
    return true;
}

bool Schematic::setWindowVisible(SchematicNode& entry, bool visible)
{
    auto& state = *entry.window;
    if (state.visible == visible) {
        return false;
    }
    state.visible = visible;
    fireOutput(entry, openOutput);
    return true;
}

void Schematic::receiveWindowTrigger(SchematicNode& entry, int input)
{
    auto& state = entry.window ? *entry.window : entry.window.emplace();
    const auto width = [&] { return clientExtent(std::get<std::int32_t>(readInput(entry.id, widthInput)), defaultWindowWidth); };
    const auto height = [&] { return clientExtent(std::get<std::int32_t>(readInput(entry.id, heightInput)), defaultWindowHeight); };
    switch (input) {
    case openInput: {
        const auto requested = std::get<bool>(readInput(entry.id, openInput));
        if (state.requested != requested) {
            state.requested = requested;
            if (requested) {
                static_cast<void>(setWindowSize(entry, width(), height()));
            } else {
                static_cast<void>(setWindowVisible(entry, false));
            }
        }
        break;
    }
    case widthInput:
        static_cast<void>(setWindowSize(entry, width(), state.height));
        break;
    case heightInput:
        static_cast<void>(setWindowSize(entry, state.width, height()));
        break;
    }
}

bool Schematic::reportWindowVisible(int id, bool visible)
{
    const auto entry = std::ranges::find(nodes_, id, &SchematicNode::id);
    if (entry == nodes_.end() || entry->kind != PrimitiveKind::Window || !entry->window || !entry->window->requested) {
        return false;
    }
    entry->window->requested = visible;
    const bool changed = setWindowVisible(*entry, visible);
    if (changed) {
        refreshConnections();
    }
    return changed || !visible;
}

bool Schematic::reportWindowSize(int id, int width, int height)
{
    const auto entry = std::ranges::find(nodes_, id, &SchematicNode::id);
    if (entry == nodes_.end() || entry->kind != PrimitiveKind::Window || !entry->window || !entry->window->requested ||
        width <= 0 || height <= 0) {
        return false;
    }
    if (!setWindowSize(*entry, width, height)) {
        return false;
    }
    refreshConnections();
    return true;
}

void Schematic::appendRequestedWindows(std::vector<WindowPanel>& windows) const
{
    for (const auto& entry : nodes_) {
        if (entry.kind != PrimitiveKind::Window || !entry.window || !entry.window->requested) {
            continue;
        }
        const auto mode = std::get<std::int32_t>(readInput(entry.id, typeInput));
        const auto type = mode == 1 ? WindowType::Resizable : mode == 2 ? WindowType::AppWindow : WindowType::Fixed;
        const auto& state = *entry.window;
        windows.push_back({readPanel(entry.id), state.width, state.height, type,
                           std::get<std::string>(readInput(entry.id, titleInput))});
    }
    for (const auto& [id, child] : modules_) {
        child->appendRequestedWindows(windows);
    }
}

std::vector<WindowPanel> Schematic::requestedWindows() const
{
    std::vector<WindowPanel> windows;
    rootGraph().appendRequestedWindows(windows);
    return windows;
}

}
