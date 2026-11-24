#include "Schematic.hpp"

namespace openstone {
namespace {

std::string helpLines(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    while (!text.empty()) {
        if (text.starts_with("\\n") || text.starts_with("\r\n")) {
            text.remove_prefix(2);
            result += '\n';
        } else {
            result += text.front() == '\r' ? '\n' : text.front();
            text.remove_prefix(1);
        }
    }
    return result;
}

}

ModuleHelpText Schematic::readModuleHelp(int id) const
{
    const auto* contents = module(id);
    if (!contents) return {};
    for (const auto& entry : contents->nodes_) {
        if (entry.kind != PrimitiveKind::TooltipHelp) continue;
        const auto text = helpLines(std::get<std::string>(contents->readInput(entry.id, 0)));
        if (text.empty()) continue;
        const auto newline = text.find('\n');
        return {text.substr(0, newline), newline == text.npos ? std::string{} : text.substr(newline + 1)};
    }
    return {};
}

bool Schematic::redrawsSuspended() const noexcept
{
    const auto& root = rootGraph();
    return root.hostRedrawsSuspended_ || root.presetRedrawDepth_ != 0 ||
           (root.location_->pluginHost && !root.pluginEditorOpen_);
}

void Schematic::publishRedrawSuspension(bool previous)
{
    if (redrawsSuspended() != previous) rootGraph().notifyPrimitiveOutputs(PrimitiveKind::RedrawsSuspended);
}

void Schematic::setRedrawsSuspended(bool suspended)
{
    auto& root = rootGraph();
    if (root.hostRedrawsSuspended_ == suspended) return;
    const bool previous = root.redrawsSuspended();
    root.hostRedrawsSuspended_ = suspended;
    root.publishRedrawSuspension(previous);
    root.refreshConnections();
}

void Schematic::withPresetRedrawsSuspended(const std::function<void()>& action)
{
    auto& root = rootGraph();
    const bool previous = root.redrawsSuspended();
    ++root.presetRedrawDepth_;
    std::exception_ptr error;
    try {
        root.publishRedrawSuspension(previous);
        action();
    } catch (...) {
        error = std::current_exception();
    }
    --root.presetRedrawDepth_;
    try {
        root.publishRedrawSuspension(true);
    } catch (...) {
        if (!error) error = std::current_exception();
    }
    if (error) std::rethrow_exception(error);
}

green::Value Schematic::readPanelUtility(const SchematicNode& entry) const
{
    using enum PrimitiveKind;
    if (entry.kind == RedrawsSuspended) return redrawsSuspended();
    const bool toPixels = entry.kind == GridToPixel || entry.kind == GridToPixelArea;
    const float factor = toPixels ? windowPixelsPerUnit : 1.F / windowPixelsPerUnit;
    const auto input = readInput(entry.id, 0);
    if (const auto* scalar = std::get_if<float>(&input)) {
        return green::normalizeFloat(*scalar * factor);
    }
    const auto area = std::get<yellow::Area>(input);
    return yellow::Area{area.x * factor, area.y * factor, area.width * factor, area.height * factor};
}

void Schematic::receivePanelUtility(SchematicNode& entry)
{
    if (entry.kind == PrimitiveKind::RedrawLimiter) {
        if (redrawsSuspended()) return;
        const auto& root = rootGraph();
        const auto& clock = root.systemInfo_.monotonicClock;
        const auto now = root.tickDispatchTime_ ? *root.tickDispatchTime_ :
                         clock ? clock() : TickTime{std::chrono::steady_clock::now()};
        if (entry.previousTriggerTime && now < *entry.previousTriggerTime + std::chrono::milliseconds(10)) return;
        entry.previousTriggerTime = now;
    }
    fireOutputs(entry.id);
}

}
