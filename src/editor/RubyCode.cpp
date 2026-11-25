#include "Schematic.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace openstone {
namespace {

using green::Type;
constexpr std::pair<std::string_view, Type> rubyTypes[]{
    {"Trigger", Type::Trigger}, {"Boolean", Type::Boolean}, {"Int", Type::Int}, {"Float", Type::Float},
    {"String", Type::String}, {"IntArray", Type::IntArray},
    {"FloatArray", Type::FloatArray}, {"StringArray", Type::StringArray}, {"RubyValue", Type::RubyValue},
    {"MIDI", Type::MIDI}, {"View", Type::View}, {"Color", Type::Color}, {"Bitmap", Type::Bitmap},
    {"Font", Type::Font}, {"StringFormat", Type::StringFormat},
};
constexpr std::string_view whitespace = " \t\r";

std::string_view word(std::string_view& line)
{
    const auto first = line.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        line = {};
        return {};
    }
    line.remove_prefix(first);
    const auto end = std::min(line.find_first_of(whitespace), line.size());
    const auto token = line.substr(0, end);
    line.remove_prefix(end);
    return token;
}

int portIndex(std::span<const RubyPort> ports, const RubyPort& wanted)
{
    const auto found = std::ranges::find(ports, wanted);
    return found == ports.end() ? -1 : static_cast<int>(found - ports.begin());
}

}

bool supportsRubyPort(Type type, bool output)
{
    return (!output || type != Type::View) &&
           std::ranges::any_of(rubyTypes, [type](const auto& entry) { return entry.second == type; });
}

RubyBoxSize clampRubyBoxSize(RubyBoxSize size)
{
    return {std::clamp(size.width, 420, 1800), std::clamp(size.height, 220, 1600)};
}

RubyPorts rubyPorts(std::string_view source)
{
    RubyPorts ports;
    bool declared = false;
    // Connector declarations live in the comment header, before the first line of Ruby.
    while (!source.empty()) {
        const auto end = std::min(source.find('\n'), source.size());
        auto line = source.substr(0, end);
        source.remove_prefix(std::min(end + 1, source.size()));
        const auto first = line.find_first_not_of(whitespace);
        if (first == std::string_view::npos) continue;
        if (line[first] != '#') break;
        line.remove_prefix(first + 1);
        const auto direction = word(line);
        if (direction != "input" && direction != "output") continue;
        declared = true;
        const auto type = std::ranges::find(rubyTypes, word(line), [](const auto& pair) { return pair.first; });
        const auto label = word(line);
        auto& group = direction == "input" ? ports.inputs : ports.outputs;
        if (type == std::end(rubyTypes) || label.empty() || !word(line).empty()) {
            throw std::invalid_argument("Use # input TYPE name or # output TYPE name in the Ruby header");
        }
        if (type->second == Type::View && direction == "output") {
            throw std::invalid_argument("Ruby View connectors are inputs");
        }
        if (group.size() == maxRubyPorts || std::ranges::any_of(group, [label](const auto& port) { return port.name == label; })) {
            throw std::invalid_argument("Ruby ports must have unique names and allow up to 32 per side");
        }
        group.push_back({std::string(label), type->second});
    }
    if (!declared) {
        ports.inputs.push_back({"value", Type::Float});
        ports.outputs.push_back({"result", Type::Float});
    }
    return ports;
}

RubyCodeState rubyCode(std::string draft, std::string source, std::optional<RubyPorts> manualPorts)
{
    auto ports = manualPorts ? std::move(*manualPorts) : rubyPorts(source);
    RubyCodeState code{std::move(draft), std::move(source), std::move(ports)};
    code.manualPorts = manualPorts.has_value();
    code.outputs.resize(code.ports.outputs.size());
    return code;
}

void Schematic::setRubyDraft(int id, std::string draft)
{
    auto& code = node(id).rubyCode;
    if (!code) throw std::invalid_argument("Choose a Ruby node");
    code->draft = std::move(draft);
}

void Schematic::runRuby(int id)
{
    auto& entry = node(id);
    if (!entry.rubyCode) throw std::invalid_argument("Choose a Ruby node");
    const auto& backend = rootGraph().systemInfo_.ruby;
    if (!backend) throw std::runtime_error("Ruby is unavailable in this host");
    const auto& previous = *entry.rubyCode;
    auto next = rubyCode(previous.draft, previous.draft,
                        previous.manualPorts ? std::optional{previous.ports} : std::nullopt);
    next.boxSize = previous.boxSize;
    std::vector<std::optional<green::Value>> values;
    for (const auto& input : next.ports.inputs) {
        const int before = portIndex(previous.ports.inputs, input);
        if (before < 0 || input.type == Type::Trigger || input.type == Type::View || !hasValueInput(id, before)) {
            values.push_back(std::nullopt);
        }
        else values.push_back(readInput(id, before));
    }
    const auto now = rubyNow();
    next.instance = backend(next.ports, rootGraph().location_);
    next.savedState = previous.savedState;
    next.instance->setContext({rootGraph().pressedKeys_, {}});
    auto reply = next.instance->start(next.source, values, rubySeconds(now), next.savedState);
    next.running = true;
    next.drawsView = reply.drawsView;
    installRubyCode(entry, std::move(next));
    publishRuby(entry, std::move(reply), now, !entry.rubyCode->savedState);
    refreshConnections();
}

void Schematic::applyRubyPorts(int id)
{
    auto& entry = node(id);
    if (!entry.rubyCode) throw std::invalid_argument("Choose a Ruby node");
    const auto& code = *entry.rubyCode;
    auto next = rubyCode(code.draft, code.draft);
    if (!code.manualPorts && code.ports == next.ports) return;
    next.savedState = code.savedState;
    next.boxSize = code.boxSize;
    installRubyCode(entry, std::move(next));
    refreshConnections();
}

void Schematic::resizeRubyBox(int id, RubyBoxSize size)
{
    auto& code = node(id).rubyCode;
    if (!code) throw std::invalid_argument("Choose a Ruby node");
    code->boxSize = clampRubyBoxSize(size);
}

void Schematic::editRubyPort(int id, bool output, int index, RubyPortEdit edit, int value, std::string name)
{
    auto& entry = node(id);
    if (!entry.rubyCode) throw std::invalid_argument("Choose a Ruby node");
    const auto& previous = *entry.rubyCode;
    auto ports = previous.ports;
    auto& side = output ? ports.outputs : ports.inputs;
    const int count = static_cast<int>(side.size());
    if (edit != RubyPortEdit::Count && (index < 0 || index >= count)) {
        throw std::invalid_argument("Choose a Ruby connector");
    }

    // The index map tracks connector identity even when the label or order changes.
    std::vector<int> origins(side.size());
    std::iota(origins.begin(), origins.end(), 0);
    const auto insert = [&](int at) {
        std::string label;
        for (int number = 1; ; ++number) {
            label = (output ? "out" : "in") + std::to_string(number);
            if (std::ranges::none_of(side, [&](const auto& item) { return item.name == label; })) break;
        }
        side.insert(side.begin() + at, {std::move(label), Type::Float});
        origins.insert(origins.begin() + at, -1);
    };
    switch (edit) {
    case RubyPortEdit::Count:
        if (value < 0 || value > maxRubyPorts) throw std::invalid_argument("Ruby allows up to 32 connectors per side");
        if (value < count) {
            side.resize(value);
            origins.resize(value);
        } else {
            for (int i = count; i < value; ++i) insert(i);
        }
        break;
    case RubyPortEdit::Name:
        if (name.empty() || name.size() > 128 || name.find_first_of("\r\n") != std::string::npos ||
            std::ranges::any_of(side, [&](const auto& item) { return &item != &side[index] && item.name == name; })) {
            throw std::invalid_argument("Give the Ruby connector a unique name");
        }
        side[index].name = std::move(name);
        break;
    case RubyPortEdit::Type: {
        const auto type = static_cast<Type>(value);
        if (value < 0 || value > 255 || !supportsRubyPort(type, output)) throw std::invalid_argument("Choose a supported Ruby connector type");
        if (side[index].type != type) {
            side[index].type = type;
            origins[index] = -1;
        }
        break;
    }
    case RubyPortEdit::Insert:
        if (count == maxRubyPorts) throw std::invalid_argument("Ruby allows up to 32 connectors per side");
        insert(index + 1);
        break;
    case RubyPortEdit::Remove:
        side.erase(side.begin() + index);
        origins.erase(origins.begin() + index);
        break;
    case RubyPortEdit::Move:
        if (value < 0 || value >= count) return;
        std::swap(side[index], side[value]);
        std::swap(origins[index], origins[value]);
        break;
    }
    if (ports == previous.ports) return;
    std::vector<int> oldToNew(count, -1);
    for (int i = 0; std::cmp_less(i, origins.size()); ++i) {
        if (origins[i] >= 0) oldToNew[origins[i]] = i;
    }

    auto next = rubyCode(previous.draft, previous.source, std::move(ports));
    next.boxSize = previous.boxSize;
    next.savedState = previous.savedState;
    installRubyCode(entry, std::move(next), output, oldToNew);
    refreshConnections();
}

void Schematic::installRubyCode(SchematicNode& entry, RubyCodeState next,
                                 std::optional<bool> outputSide, std::span<const int> oldToNew)
{
    const int id = entry.id;
    auto& previous = *entry.rubyCode;
    const auto oldInstance = previous.instance;
    if (next.ports != previous.ports) {
        const auto remap = [&](bool output, int old) {
            if (entry.codeSourcePort(output) == old) {
                return static_cast<int>(output ? next.ports.outputs.size() : next.ports.inputs.size());
            }
            if (outputSide == output) return oldToNew[old];
            return output ? portIndex(next.ports.outputs, previous.ports.outputs.at(old))
                          : portIndex(next.ports.inputs, previous.ports.inputs.at(old));
        };
        const auto remapTriggers = [&](const auto& current, bool output, std::size_t count) {
            std::vector<std::uint64_t> result(count + entry.codeSourcePorts);
            for (int old = 0; std::cmp_less(old, current.size()); ++old) {
                if (const auto port = remap(output, old); port >= 0) result[port] = current[old];
            }
            return result;
        };
        auto inputs = remapTriggers(entry.inputTriggers, false, next.ports.inputs.size());
        auto outputs = remapTriggers(entry.outputTriggers, true, next.ports.outputs.size());
        std::vector<SchematicLink> wires;
        std::map<SchematicLink, std::uint64_t> counts;
        for (auto link : links_) {
            const auto old = link;
            if (link.target == id) link.input = remap(false, link.input);
            if (link.source == id) link.output = remap(true, link.output);
            if (link.input < 0 || link.output < 0) continue;
            wires.push_back(link);
            counts.emplace(link, linkTriggers_.at(old));
        }
        const auto swap = [&] {
            std::swap(previous, next);
            entry.inputTriggers.swap(inputs);
            entry.outputTriggers.swap(outputs);
            links_.swap(wires);
            linkTriggers_.swap(counts);
        };
        swap();
        try {
            rebuildConnectedRuntimes(true, true);
        } catch (...) {
            swap();
            throw;
        }
        ++rootGraph().drawingTopology_;
    } else {
        if (previous.drawsView || previous.usesRedraw || next.drawsView) ++rootGraph().drawingTopology_;
        previous = std::move(next);
    }
    if (oldInstance) oldInstance->stopSound();
    clearRubyEvents(oldInstance);
    clearRubyDropList(id);
    clearRubyMessageBoxes(id);
    if (const auto port = entry.codeSourcePort(true)) fireOutput(entry, *port);
}

void Schematic::stopRuby(int id)
{
    auto& code = node(id).rubyCode;
    if (!code) throw std::invalid_argument("Choose a Ruby node");
    if (code->instance) code->instance->stopSound();
    clearRubyEvents(code->instance);
    clearRubyDropList(id);
    clearRubyMessageBoxes(id);
    if (code->running && (code->drawsView || code->usesRedraw)) ++rootGraph().drawingTopology_;
    code->running = false;
    code->usesRedraw = false;
    code->mouseCapture.reset();
    code->edits.clear();
    code->cursorBalance = 0;
    code->drawError.clear();
    if (!code->savesState) code->instance.reset();
    code->status.clear();
    code->error = false;
}

void Schematic::appendRubyDrawing(const SchematicNode& entry, int input, const yellow::View& view,
                                  std::vector<yellow::PanelDrawing>& drawings, bool publish) const
{
    const auto& code = *entry.rubyCode;
    if (!code.running || !code.drawsView || view.mapping.fullyClipped()) return;
    auto* drawError = publish ? &const_cast<Schematic&>(*this).node(entry.id).rubyCode->drawError : nullptr;
    try {
        updateRubyContext(entry);
        auto commands = code.instance->draw(input, view.bounds, rubySeconds(rubyNow()));
        for (auto& command : commands) {
            std::visit([&](auto& shape) {
                drawings.push_back({std::move(shape), view.drawOrder, view.mapping});
            }, command);
        }
        if (drawError) drawError->clear();
    } catch (const std::exception& error) {
        if (drawError) *drawError = error.what();
    }
}

TickTime Schematic::rubyNow() const
{
    const auto& root = rootGraph();
    if (root.rubyDispatchTime_) return *root.rubyDispatchTime_;
    if (root.tickDispatchTime_) return *root.tickDispatchTime_;
    return root.sequenceNow();
}

double Schematic::rubySeconds(TickTime now) const
{
    return std::max(0.0, (now - rootGraph().rubyClockStart_).count());
}

void Schematic::receiveRuby(SchematicNode& entry, int input)
{
    if (!entry.rubyCode->running || entry.rubyCode->ports.inputs.at(input).type == Type::View) return;
    RubyAction action{RubyActionKind::Input, static_cast<std::size_t>(input)};
    if (entry.rubyCode->ports.inputs.at(input).type != Type::Trigger) action.value = readInput(entry.id, input);
    dispatchRuby(entry, action);
}

void Schematic::updateRubyContext(const SchematicNode& entry) const
{
    const auto& code = *entry.rubyCode;
    RubyContext context{rootGraph().pressedKeys_, {}};
    context.viewSizes.resize(code.ports.inputs.size());
    for (int port = 0; std::cmp_less(port, code.ports.inputs.size()); ++port) {
        if (code.ports.inputs[port].type != Type::View) continue;
        const auto view = std::get<yellow::View>(readInput(entry.id, port));
        if (view.panel != 0) context.viewSizes[port] = green::Point{view.bounds.width, view.bounds.height};
    }
    code.instance->setContext(std::move(context));
}

void Schematic::dispatchRuby(SchematicNode& entry, const RubyAction& action)
{
    auto& code = *entry.rubyCode;
    const auto now = rubyNow();
    code.stateDirty |= code.savesState;
    RubyReply reply;
    try {
        using enum RubyActionKind;
        updateRubyContext(entry);
        if (action.kind == Output) code.instance->storeOutput(action.port, action.value);
        else if (action.kind == Method) reply = code.instance->method(action.method, action.value, rubySeconds(now));
        else reply = code.instance->event(action.port, action.value, rubySeconds(now));
    } catch (const std::exception& error) {
        code.status = error.what();
        code.error = true;
        return;
    }
    if (action.kind == RubyActionKind::Output) {
        code.outputs.at(action.port) = action.value;
        fireOutput(entry, action.port);
    } else {
        publishRuby(entry, std::move(reply), now);
    }
}

void Schematic::publishRuby(SchematicNode& entry, RubyReply reply, TickTime now, bool mayChangeState)
{
    auto& root = rootGraph();
    auto& code = *entry.rubyCode;
    if (code.drawsView != reply.drawsView) {
        code.drawsView = reply.drawsView;
        code.drawError.clear();
        ++root.drawingTopology_;
    }
    code.handlesMouse = reply.handlesMouse;
    if (!code.handlesMouse) code.mouseCapture.reset();
    code.savesState = reply.savesState;
    code.stateDirty |= mayChangeState && code.savesState;
    code.status = std::move(reply.result);
    code.error = false;
    for (auto& action : reply.actions) {
        using enum RubyActionKind;
        if (action.kind == ClearEvents) {
            clearRubyEvents(code.instance);
        } else if (action.kind == Redraw) {
            const auto view = std::get<yellow::View>(readInput(entry.id, static_cast<int>(action.port)));
            code.usesRedraw |= requestRedraw(view, action.area.value_or(view.bounds));
        } else if (action.kind == CreateEdit) {
            createRubyEdit(entry, static_cast<int>(action.port), std::get<green::RubyValue>(*action.value),
                          std::move(*action.edit));
        } else if (action.kind == CreateDropList) {
            createRubyDropList(entry, static_cast<int>(action.port), std::get<green::RubyValue>(*action.value),
                               std::move(*action.dropList));
        } else if (action.kind == MessageBox) {
            createRubyMessageBox(entry, std::get<green::RubyValue>(*action.value), std::move(*action.messageBox));
        } else if (action.kind == ShowCursor) {
            auto& balance = code.mouseCapture ? code.mouseCapture->cursorBalance : code.cursorBalance;
            const bool show = std::get<bool>(*action.value);
            if (show && balance < std::numeric_limits<std::int32_t>::max()) ++balance;
            else if (!show && balance > std::numeric_limits<std::int32_t>::min()) --balance;
        } else if (action.kind == Output && !action.timestamp) {
            code.outputs.at(action.port) = std::move(action.value);
            fireOutput(entry, action.port);
        } else {
            // For self-inputs without a timestamp, queue at the current time to avoid re-entering Ruby.
            auto deadline = now;
            if (action.timestamp) {
                if (!std::isfinite(*action.timestamp)) throw std::invalid_argument("Ruby event time must be finite");
                deadline = std::max(now, root.rubyClockStart_ + TickTime::duration{*action.timestamp});
            }
            root.rubyEvents_.emplace(deadline, ScheduledRubyEvent{code.instance, path_, entry.id, std::move(action)});
        }
    }
}

void Schematic::clearRubyEvents(const std::shared_ptr<RubyInstance>& owner)
{
    if (!owner) return;
    std::erase_if(rootGraph().rubyEvents_, [&](const auto& scheduled) {
        return scheduled.second.owner.lock() == owner;
    });
}

void Schematic::clearRubyBranch(const SchematicNode& entry)
{
    if (entry.rubyCode) {
        if (entry.rubyCode->instance) entry.rubyCode->instance->stopSound();
        clearRubyEvents(entry.rubyCode->instance);
        clearRubyDropList(entry.id);
        clearRubyMessageBoxes(entry.id);
    }
    if (auto* child = module(entry.id)) {
        for (const auto& node : child->nodes_) child->clearRubyBranch(node);
    }
}

std::pair<Schematic*, SchematicNode*> Schematic::rubyTarget(const ScheduledRubyEvent& event)
{
    const auto owner = event.owner.lock();
    if (!owner) return {};
    const auto matches = [&](const SchematicNode& node) {
        return node.rubyCode && node.rubyCode->running && node.rubyCode->instance == owner;
    };
    auto* level = this;
    for (int parent : event.path) {
        level = level->module(parent);
        if (!level) break;
    }
    if (level) {
        const auto node = std::ranges::find(level->nodes_, event.node, &SchematicNode::id);
        if (node != level->nodes_.end() && matches(*node)) return {level, &*node};
    }
    // A graph edit can move a running Ruby node into a module while its events are pending.
    const auto find = [&](const auto& walk, Schematic& graph) -> std::pair<Schematic*, SchematicNode*> {
        const auto node = std::ranges::find_if(graph.nodes_, matches);
        if (node != graph.nodes_.end()) return {&graph, &*node};
        for (auto& [id, child] : graph.modules_) {
            if (auto target = walk(walk, *child); target.first) return target;
        }
        return {};
    };
    return find(find, *this);
}

void Schematic::pruneRubyEvents()
{
    std::erase_if(rubyEvents_, [&](const auto& scheduled) { return !rubyTarget(scheduled.second).first; });
}

void Schematic::collectRubyDeadline(TickSchedule& result)
{
    while (!rubyEvents_.empty()) {
        const auto front = rubyEvents_.begin();
        if (rubyTarget(front->second).first) {
            if (!result.deadline || front->first < *result.deadline) result.deadline = front->first;
            return;
        }
        rubyEvents_.erase(front);
    }
}

void Schematic::advanceRubyEvents(TickTime now, TickSchedule& result)
{
    // Yield to the host event loop even if a script keeps scheduling at the same timestamp.
    constexpr std::size_t maxEventsPerWake = 256;
    for (std::size_t delivered = 0; delivered < maxEventsPerWake && !rubyEvents_.empty() &&
         rubyEvents_.begin()->first <= now; ++delivered) {
        auto event = rubyEvents_.extract(rubyEvents_.begin());
        const auto [level, entry] = rubyTarget(event.mapped());
        if (!entry) continue;
        const auto previous = std::exchange(rubyDispatchTime_, event.key());
        const auto& action = event.mapped().action;
        try {
            if (action.kind == RubyActionKind::Input) {
                ++level->triggers_;
                ++entry->inputTriggers.at(action.port);
            }
            result.fired = true;
            level->dispatchRuby(*entry, action);
        } catch (...) {
            if (!result.error) result.error = std::current_exception();
        }
        rubyDispatchTime_ = previous;
    }
}

}
