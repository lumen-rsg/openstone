#include "Schematic.hpp"

#include <algorithm>
#include <cmath>

namespace openstone {

const PresentationState& Schematic::presentation() const
{
    return rootGraph().presentation_;
}

Schematic* Schematic::presentationOwner(std::span<const int> path)
{
    auto* owner = &rootGraph();
    for (const int id : path) {
        owner = owner->module(id);
        if (!owner) return nullptr;
    }
    return owner;
}

bool Schematic::inPresentation() const
{
    const auto& scope = presentation().panelPath;
    return scope && path_.size() >= scope->size() && std::equal(scope->begin(), scope->end(), path_.begin());
}

std::optional<yellow::Color> Schematic::presentationBackground() const
{
    for (const auto& entry : nodes_) {
        if (entry.kind == PrimitiveKind::PanelBackgroundColor &&
            std::ranges::any_of(links_, [&](const SchematicLink& link) { return link.target == entry.id; })) {
            return std::get<yellow::Color>(readInput(entry.id, 0));
        }
    }
    return std::nullopt;
}

void Schematic::setPresentationPanel(std::optional<std::vector<int>> path)
{
    auto& state = rootGraph().presentation_;
    if (state.panelPath == path || (path && !presentationOwner(*path))) return;
    const auto old = std::move(state);
    state = {.panelPath = std::move(path)};
    if (old.panelPath) {
        if (auto* owner = presentationOwner(*old.panelPath)) {
            if (old.fullScreen) owner->notifyPrimitiveOutputs(PrimitiveKind::FullScreen);
            if (old.scale != 1) owner->notifyPrimitiveOutputs(PrimitiveKind::PanelZoom);
        }
    }
}

bool Schematic::reportPresentation(std::span<const int> path, bool fullScreen, float scale)
{
    auto& root = rootGraph();
    auto& state = root.presentation_;
    if (!state.panelPath || !std::ranges::equal(*state.panelPath, path)) return false;
    auto* owner = presentationOwner(path);
    if (!owner) return false;
    const bool fullScreenChanged = fullScreen != state.fullScreen;
    const bool scaleChanged = std::isfinite(scale) && scale > 0 && scale != state.scale;
    if (!fullScreenChanged && !scaleChanged) return false;
    state.fullScreen = fullScreen;
    if (scaleChanged) state.scale = scale;
    if (fullScreenChanged) owner->notifyPrimitiveOutputs(PrimitiveKind::FullScreen);
    if (scaleChanged) owner->notifyPrimitiveOutputs(PrimitiveKind::PanelZoom);
    root.refreshConnections();
    return true;
}

green::Value Schematic::readPresentationControl(PrimitiveKind kind) const
{
    if (kind == PrimitiveKind::FullScreen) return inPresentation() && presentation().fullScreen;
    return inPresentation() ? presentation().scale : 1.0F;
}

void Schematic::receivePresentationControl(const SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    if (entry.kind == ExeQuit && rootGraph().systemInfo_.quitApplication) {
        rootGraph().systemInfo_.quitApplication();
        return;
    }
    if (!inPresentation()) return;
    auto& state = rootGraph().presentation_;
    switch (entry.kind) {
    case FullScreen:
        ++state.fullScreenToggles;
        break;
    case PanelZoom:
        if (input == 1) {
            const auto scale = std::get<float>(readInput(entry.id, 0));
            if (std::isfinite(scale) && scale >= 0) state.requestedScale = scale;
        }
        break;
    case ExeQuit:
    case FocusMode:
        setPresentationPanel(std::nullopt);
        break;
    default:
        break;
    }
}

}
