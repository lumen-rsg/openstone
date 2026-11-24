#include "Schematic.hpp"
#include "ViewRegion.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace openstone {
namespace {

constexpr int viewInput = 0;
constexpr int fontInput = 2;
constexpr int textColorInput = 3;
constexpr int editBackgroundInput = 4;
constexpr int setInput = 5;
constexpr int finishOnlyInput = 6;
constexpr int startInput = 7;
constexpr int multilineInput = 8;
constexpr int passwordInput = 9;

constexpr int viewOutput = 0;
constexpr int textOutput = 1;
constexpr int editingOutput = 2;

std::uint64_t nextEditSession()
{
    // Focus-loss notifications can arrive after the editor has switched projects.
    static std::atomic<std::uint64_t> next{0};
    return ++next;
}

}

green::Value Schematic::readEditControl(const SchematicNode& entry, int output) const
{
    switch (output) {
    case viewOutput:
        return readInput(entry.id, viewInput);
    case textOutput:
        if (entry.editControl && entry.editControl->text) {
            return *entry.editControl->text;
        }
        return readInput(entry.id, setInput);
    case editingOutput:
        return entry.editControl && entry.editControl->session != 0;
    default:
        throw std::logic_error("Edit Control output has no value");
    }
}

yellow::EditControlCommand Schematic::readEditControlDrawing(const SchematicNode& entry, yellow::Area area) const
{
    return {{path_, entry.id}, area,
            std::get<yellow::Font>(readInput(entry.id, fontInput)),
            std::get<yellow::Color>(readInput(entry.id, textColorInput)),
            std::get<yellow::Color>(readInput(entry.id, editBackgroundInput)),
            std::get<std::string>(readEditControl(entry, textOutput)),
            std::get<bool>(readInput(entry.id, multilineInput)),
            std::get<bool>(readInput(entry.id, passwordInput))};
}

void Schematic::receiveEditControlTrigger(SchematicNode& entry, int input)
{
    switch (input) {
    case setInput: {
        auto text = std::get<std::string>(readInput(entry.id, setInput));
        auto& state = entry.editControl ? *entry.editControl : entry.editControl.emplace();
        state.text = std::move(text);
        state.pendingTextPulse = false;
        fireOutput(entry, textOutput);
        break;
    }
    case startInput:
        rootGraph().editControlStart_ = yellow::PanelControlId{path_, entry.id};
        break;
    case viewInput:
        fireOutput(entry, viewOutput);
        break;
    }
}

std::optional<yellow::PanelControlId> Schematic::takeEditControlStart()
{
    return std::exchange(rootGraph().editControlStart_, std::nullopt);
}

SchematicNode* Schematic::findEditControl(int id)
{
    const auto found = std::ranges::find(nodes_, id, &SchematicNode::id);
    return found != nodes_.end() &&
           (found->kind == PrimitiveKind::EditControl || (found->rubyCode && found->rubyCode->running))
        ? &*found : nullptr;
}

void Schematic::createRubyEdit(SchematicNode& entry, int input, green::RubyValue callbackId, RubyEditOptions options)
{
    const auto view = std::get<yellow::View>(readInput(entry.id, input));
    const auto* owner = panelOwner(view);
    if (!owner || std::ranges::any_of(view.drawRoute, [](const auto& boundary) {
            return boundary.kind == yellow::DrawBoundaryKind::Loop;
        })) {
        return;
    }
    const auto bounds = yellow::regionOf(owner->readPanel(view.panel).view.bounds);
    const auto geometry = yellow::combineClip(yellow::mapRegion(view.mapping, options.area), bounds, 0);
    if (yellow::regionEmpty(view.mapping.clip ? yellow::combineClip(*view.mapping.clip, geometry, 0) : geometry)) {
        return;
    }

    const auto token = nextEditSession();
    entry.rubyCode->edits.emplace(token, RubyEditControl{
        input, {view.schematicPath, view.panel}, std::move(callbackId),
        {{path_, entry.id}, options.area, std::move(options.font), options.textColor, options.background,
         std::move(options.text), options.multiline, false, token},
    });
}

void Schematic::appendRubyEditControls(const SchematicNode& entry, int input, const yellow::View& view,
                                       std::vector<yellow::PanelDrawing>& drawings) const
{
    const yellow::PanelControlId target{view.schematicPath, view.panel};
    for (const auto& [token, edit] : entry.rubyCode->edits) {
        if (edit.input == input && edit.panel == target) {
            drawings.push_back({edit.command, view.drawOrder, view.mapping});
        }
    }
}

void Schematic::dispatchRubyEdit(SchematicNode& entry, bool finished, const green::RubyValue& callbackId,
                                 std::string_view text)
{
    auto& code = *entry.rubyCode;
    const auto now = rubyNow();
    std::optional<RubyReply> reply;
    try {
        updateRubyContext(entry);
        reply = code.instance->editEvent(finished, callbackId, text, rubySeconds(now));
    } catch (const std::exception& error) {
        code.error = true;
        code.status = error.what();
        return;
    }
    if (reply) publishRuby(entry, std::move(*reply), now);
}

std::uint64_t Schematic::beginEditControl(int id, std::uint64_t rubySession)
{
    auto* entry = findEditControl(id);
    if (!entry) {
        return 0;
    }
    if (entry->rubyCode) return entry->rubyCode->edits.contains(rubySession) ? rubySession : 0;
    if (rubySession) return 0;

    auto& state = entry->editControl ? *entry->editControl : entry->editControl.emplace();
    if (state.session) {
        return state.session;
    }
    const auto session = nextEditSession();
    state.session = session;
    try {
        fireOutput(*entry, editingOutput);
        refreshConnections();
    } catch (...) {
        state.session = 0;
        throw;
    }
    return session;
}

bool Schematic::changeEditControl(int id, std::uint64_t session, std::string text)
{
    auto* entry = findEditControl(id);
    if (!entry || !session) return false;
    if (entry->rubyCode) {
        auto& edits = entry->rubyCode->edits;
        const auto found = edits.find(session);
        if (found == edits.end() || found->second.command.text == text) return false;
        auto& edit = found->second;
        edit.command.text = std::move(text);
        dispatchRubyEdit(*entry, false, edit.callbackId, edit.command.text);
        refreshConnections();
        return true;
    }
    if (!entry->editControl || entry->editControl->session != session ||
        std::get<std::string>(readEditControl(*entry, textOutput)) == text) {
        return false;
    }
    auto& state = *entry->editControl;
    state.text = std::move(text);
    state.pendingTextPulse = std::get<bool>(readInput(id, finishOnlyInput));
    if (!state.pendingTextPulse) {
        fireOutput(*entry, textOutput);
        refreshConnections();
    }
    return true;
}

bool Schematic::finishEditControl(int id, std::uint64_t session)
{
    auto* entry = findEditControl(id);
    if (!entry || !session) return false;
    if (entry->rubyCode) {
        // Remove the old field before its callback so it can open a new one immediately.
        const auto edit = entry->rubyCode->edits.extract(session);
        if (edit.empty()) return false;
        dispatchRubyEdit(*entry, true, edit.mapped().callbackId, edit.mapped().command.text);
        refreshConnections();
        return true;
    }
    if (!entry->editControl || entry->editControl->session != session) return false;
    auto& state = *entry->editControl;
    state.session = 0;
    const bool publishText = std::exchange(state.pendingTextPulse, false);
    fireOutput(*entry, editingOutput);
    if (publishText) {
        fireOutput(*entry, textOutput);
    }
    refreshConnections();
    return true;
}

}
