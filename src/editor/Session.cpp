#include "Session.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace openstone {

void Session::requestSchematicClose()
{
    if (closeState_ != CloseState::Open) return;
    closeState_ = CloseState::Emitting;
    lifecycleError_ = {};
    // Arm every Hold before any output fires so Done can arrive synchronously from another module.
    root_->armCloseHolds(lifecycleError_);
    root_->fireLifecycleTriggers(PrimitiveKind::AboutToClose, root_->nodes_, lifecycleError_);
    closeState_ = CloseState::Requested;
}

bool Session::schematicReadyToClose() const
{
    return closeState_ == CloseState::Requested && !root_->hasCloseHolds();
}

bool Session::openSchematic(std::unique_ptr<Schematic> replacement, std::function<void()> beforeReplace,
                            std::string filePath)
{
    if (!replacement || replacement->parent_) {
        throw std::invalid_argument("Choose a root schematic to open");
    }
    if (pendingSchematic_) {
        throw std::logic_error("A schematic is already waiting to open");
    }
    replacement->location_->filePath = std::move(filePath);
    replacement->rebuildConnectedRuntimes(true, true);
    replacement->refreshConnections(false);
    pendingSchematic_.emplace(std::move(replacement), std::move(beforeReplace));
    requestSchematicClose();
    return completeSchematicOpen();
}

bool Session::completeSchematicOpen()
{
    if (!pendingSchematic_ || !schematicReadyToClose()) return false;

    std::uint64_t nextGroup = 1;
    const auto visit = [&](const auto& self, const Schematic& level) -> void {
        nextGroup = std::max(nextGroup, level.syncGroup_ + 1);
        for (const auto& [id, module] : level.modules_) self(self, *module);
    };
    visit(visit, *pendingSchematic_->graph);

    std::map<Path, Viewport> viewports{{Path{}, Viewport{}}};
    auto pending = std::exchange(pendingSchematic_, std::nullopt);
    if (pending->beforeReplace) pending->beforeReplace();
    auto midiBackend = root_->midiDevices_.backend;
    auto hostMidi = std::move(root_->hostMidiOutput_);
    pending->graph->location_->pluginHost = std::move(root_->location_->pluginHost);
    pending->graph->pluginHostTransport_ = root_->pluginHostTransport_;
    pending->graph->pluginEditorOpen_ = root_->pluginEditorOpen_;
    pending->graph->hostRedrawsSuspended_ = root_->hostRedrawsSuspended_;
    pending->graph->pluginHostCallbacks_ = std::move(root_->pluginHostCallbacks_);
    pending->graph->reportedPluginLatency_ = root_->reportedPluginLatency_;
    pending->graph->audioControls_ = std::move(root_->audioControls_);
    pending->graph->settingsStore_ = std::move(root_->settingsStore_);
    if (!pending->graph->serialBackend_) {
        pending->graph->serialBackend_ = root_->serialBackend_;
        pending->graph->serialPorts_ = root_->serialPorts_;
    }
    root_ = std::move(pending->graph);
    root_->rubyClockStart_ = root_->sequenceNow();
    if (hostMidi) root_->setHostMidiOutput(std::move(hostMidi));
    if (midiBackend && !root_->midiDevices_.backend) root_->setMidiDeviceBackend(std::move(midiBackend));
    closeState_ = CloseState::Open;
    activeGraph_ = root_.get();
    path_.clear();
    viewports_ = std::move(viewports);
    clipboard_.reset();
    clipboardSource_.clear();
    painterSource_.clear();
    nextSyncGroup_ = nextGroup;
    clearHistory();

    try {
        static_cast<void>(root_->refreshMidiDevices(true, false));
        root_->refreshConnections(false);
        root_->refreshPluginLatency();
    } catch (...) {
        lifecycleError_ = std::current_exception();
    }
    root_->fireLifecycleTriggers(PrimitiveKind::AfterLoad, root_->nodes_, lifecycleError_);
    return true;
}

const Schematic& Session::graphAt(std::size_t depth) const
{
    if (depth > path_.size()) {
        throw std::out_of_range("Schematic level is outside the current module path");
    }
    const Schematic* graph = root_.get();
    for (std::size_t i = 0; i < depth; ++i) {
        graph = graph->module(path_[i]);
    }
    return *graph;
}

std::string Session::levelName(std::size_t depth) const
{
    if (depth == 0) {
        return "Main";
    }
    const auto& parent = graphAt(depth - 1);
    const int module = path_.at(depth - 1);
    return std::ranges::find(parent.nodes(), module, &SchematicNode::id)->moduleName;
}

const Viewport& Session::viewport(std::size_t depth) const
{
    if (depth > path_.size()) {
        throw std::out_of_range("Viewport level is outside the current module path");
    }
    if (depth == path_.size()) {
        return activeViewport();
    }
    return viewports_.at(std::vector(path_.begin(), path_.begin() + depth));
}

const Viewport& Session::activeViewport() const
{
    return viewports_.at(path_);
}

bool Session::enterModule(int nodeId)
{
    auto* child = graph().module(nodeId);
    if (!child) {
        return false;
    }
    auto next = path_;
    next.push_back(nodeId);
    viewports_.try_emplace(next);
    path_ = std::move(next);
    activeGraph_ = child;
    return true;
}

bool Session::navigate(std::size_t depth)
{
    if (depth >= path_.size()) {
        return false;
    }
    auto* graph = root_.get();
    for (std::size_t i = 0; i < depth; ++i) {
        graph = graph->module(path_[i]);
    }
    path_.resize(depth);
    activeGraph_ = graph;
    return true;
}

void Session::pan(double screenX, double screenY) noexcept
{
    const auto& view = viewports_.at(path_);
    centerOn(view.x - screenX / view.zoom, view.y - screenY / view.zoom);
}

void Session::zoomAt(double factor, double offsetX, double offsetY) noexcept
{
    if (!std::isfinite(factor) || factor <= 0 || !std::isfinite(offsetX) || !std::isfinite(offsetY)) {
        return;
    }

    auto& view = viewports_.at(path_);
    const double zoom = std::clamp(view.zoom * factor, 0.2, 4.0);
    const double anchorShift = 1 / view.zoom - 1 / zoom;
    centerOn(view.x + offsetX * anchorShift, view.y + offsetY * anchorShift);
    view.zoom = zoom;
}

void Session::centerOn(double worldX, double worldY) noexcept
{
    if (std::isfinite(worldX) && std::isfinite(worldY)) {
        constexpr double limit = 1'000'000;
        auto& view = viewports_.at(path_);
        view.x = std::clamp(worldX, -limit, limit);
        view.y = std::clamp(worldY, -limit, limit);
    }
}

void Session::resetView() noexcept
{
    viewports_.at(path_) = {};
}

}
