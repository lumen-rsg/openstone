#include "Session.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <ranges>
#include <set>
#include <stdexcept>

namespace openstone {
namespace {

std::size_t nodeIndex(const Schematic& graph, int id)
{
    const auto found = std::ranges::find(graph.nodes(), id, &SchematicNode::id);
    if (found == graph.nodes().end()) {
        throw std::invalid_argument("Choose a node");
    }
    return static_cast<std::size_t>(found - graph.nodes().begin());
}

bool isPathPrefix(std::span<const int> prefix, std::span<const int> path)
{
    return path.size() >= prefix.size() && std::ranges::equal(prefix, path.first(prefix.size()));
}

std::optional<green::Value> authoredValue(const SchematicNode& node)
{
    const auto behavior = primitive(node.kind).behavior;
    if (behavior == PrimitiveBehavior::Counter) {
        return std::int32_t{0};
    }
    if (behavior == PrimitiveBehavior::SampleMono) {
        return node.kind == PrimitiveKind::MonoToFloat ? std::optional<green::Value>{0.0F} : std::nullopt;
    }
    if (behavior == PrimitiveBehavior::Blocker || behavior == PrimitiveBehavior::GreenLogic ||
        behavior == PrimitiveBehavior::ProcessTrigger || behavior == PrimitiveBehavior::TextFile ||
        behavior == PrimitiveBehavior::PresetUtility || behavior == PrimitiveBehavior::PersistSettings ||
        behavior == PrimitiveBehavior::MidiOperation ||
        behavior == PrimitiveBehavior::GetArrayElement ||
        behavior == PrimitiveBehavior::TransformArray ||
        node.kind == PrimitiveKind::ArrayInterpolatedGet || node.kind == PrimitiveKind::FloatArrayDraw) {
        return {};
    }
    if (behavior == PrimitiveBehavior::MemoryOperation &&
        node.kind != PrimitiveKind::MemCreate && node.kind != PrimitiveKind::MemArraySetAt) {
        return {};
    }
    return node.localValue;
}

void copyNodeStorage(SchematicNode& node)
{
    if (node.localValue) {
        if (auto* bitmap = std::get_if<yellow::Bitmap>(&*node.localValue)) {
            *bitmap = bitmap->copy();
        } else if (auto* memory = std::get_if<green::Memory>(&*node.localValue)) {
            *memory = memory->copy();
        } else if (auto* array = std::get_if<green::MemoryArray>(&*node.localValue)) {
            *array = array->copy();
        }
    }
}

SchematicNode relocateCopy(SchematicNode node, int id, double x, double y)
{
    if (!std::isfinite(x) || !std::isfinite(y)) {
        throw std::invalid_argument("Node position must be finite");
    }
    copyNodeStorage(node);
    node.id = id;
    node.x = x;
    node.y = y;
    node.panelPosition = {};
    node.propertiesPosition = {};
    node.panelHidden = false;
    return node;
}

bool sameValue(const SchematicNode& a, const SchematicNode& b)
{
    if (a.tick && b.tick &&
        (a.tick->manualRate != b.tick->manualRate || a.tick->displaySync != b.tick->displaySync)) {
        return false;
    }
    const auto first = authoredValue(a);
    const auto second = authoredValue(b);
    if (first == second) {
        return true;
    }
    return first && second && std::holds_alternative<float>(*first) && std::holds_alternative<float>(*second) &&
           std::isnan(std::get<float>(*first)) && std::isnan(std::get<float>(*second));
}

int correspondingPort(const std::vector<Port>& source, const std::vector<Port>& target, int index)
{
    const auto name = source.at(index).name;
    // Most connectors are fixed. Bus channels can move as instance inputs change.
    if (static_cast<std::size_t>(index) < target.size() && target[index].name == name) {
        return index;
    }
    const auto match = std::ranges::find(target, name, &Port::name);
    if (match == target.end()) {
        throw std::invalid_argument("A synchronized copy is missing that connector");
    }
    return static_cast<int>(match - target.begin());
}

}

Schematic* Session::findGraph(std::span<const int> path)
{
    return const_cast<Schematic*>(std::as_const(*this).findGraph(path));
}

const Schematic* Session::findGraph(std::span<const int> path) const
{
    const auto* result = root_.get();
    for (int id : path) {
        result = result->module(id);
        if (!result) {
            break;
        }
    }
    return result;
}

std::vector<const Schematic*> Session::moduleGraphs() const
{
    std::vector<const Schematic*> result;
    const auto collect = [&](const auto& visit, const Schematic& graph) -> void {
        for (const auto& [id, child] : graph.modules_) {
            result.push_back(child.get());
            visit(visit, *child);
        }
    };
    collect(collect, *root_);
    return result;
}

std::vector<const Schematic*> Session::synchronizedGraphs(const Schematic& graph) const
{
    std::vector<const Schematic*> peers{&graph};
    std::optional<std::vector<const Schematic*>> modules;
    for (std::size_t index = 0; index < peers.size(); ++index) {
        std::vector<std::size_t> relative;
        for (auto* ancestor = peers[index]; ancestor->parent_; ancestor = ancestor->parent_) {
            if (ancestor->syncGroup_ != 0) {
                if (!modules) {
                    modules = moduleGraphs();
                }
                for (auto* peer : *modules) {
                    if (peer == ancestor || peer->syncGroup_ != ancestor->syncGroup_) {
                        continue;
                    }
                    const auto* match = peer;
                    for (auto row : relative | std::views::reverse) {
                        match = row < match->nodes().size() ? match->module(match->nodes()[row].id) : nullptr;
                        if (!match) {
                            break;
                        }
                    }
                    if (match && !std::ranges::contains(peers, match)) {
                        peers.push_back(match);
                    }
                }
            }
            relative.push_back(nodeIndex(*ancestor->parent_, ancestor->path_.back()));
        }
    }
    return peers;
}

std::vector<Session::EditTarget> Session::editTargets(std::span<const int> ids)
{
    std::vector<std::size_t> rows;
    for (int id : ids) {
        rows.push_back(nodeIndex(graph(), id));
    }

    std::vector<EditTarget> targets;
    for (const auto* peer : synchronizedGraphs(graph())) {
        EditTarget target{const_cast<Schematic*>(peer), {}};
        for (auto row : rows) {
            const auto& entry = peer->nodes().at(row);
            if (entry.kind != graph().nodes()[row].kind) {
                throw std::logic_error("Synchronized schematics have different components");
            }
            target.ids.push_back(entry.id);
        }
        targets.push_back(std::move(target));
    }
    return targets;
}

void Session::edit(std::span<const int> ids,
                   const std::function<void(Schematic&, const std::vector<int>&)>& apply)
{
    stopSyncPainter();
    applyEdit(editTargets(ids), apply);
}

void Session::applyEdit(const std::vector<EditTarget>& targets,
                        const std::function<void(Schematic&, const std::vector<int>&)>& apply)
{
    std::exception_ptr failure;
    for (const auto& target : targets) {
        try {
            apply(*target.graph, target.ids);
        } catch (...) {
            // A pulse can fail after an edit is committed in one instance.
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

int Session::addNode(PrimitiveKind kind, double x, double y)
{
    int id = -1;
    edit({}, [&](auto& graph, const auto&) {
        const int added = graph.addNode(kind, x, y);
        if (&graph == activeGraph_) {
            id = added;
        }
    });
    return id;
}

void Session::forgetViewports(std::span<const int> subtree)
{
    std::erase_if(viewports_, [&](const auto& entry) { return isPathPrefix(subtree, entry.first); });
}

void Session::removeNode(int id)
{
    edit({id}, [&](auto& graph, const auto& ids) {
        rememberRemovedNode(graph, ids[0]);
        if (auto* child = graph.module(ids[0])) {
            forgetViewports(child->path_);
        }
        graph.removeNode(ids[0]);
    });
}

void Session::renameNode(int id, std::string name)
{
    edit({id}, [&](auto& graph, const auto& ids) {
        auto& node = graph.node(ids[0]);
        if (node.kind == PrimitiveKind::Module) {
            graph.setModuleName(node.id, name);
        } else if (isModuleEndpoint(node.kind)) {
            graph.setModulePortName(node.id, name);
        } else if (isPropertyEndpoint(node.kind)) {
            graph.setPropertyPortName(node.id, name);
            graph.refreshConnections();
        } else if (usesWirelessLabel(node.kind)) {
            graph.setWirelessLabel(node.id, name);
        } else {
            node.label = name;
        }
    });
}

void Session::moveNode(int id, double x, double y)
{
    const NodePosition position{id, x, y};
    moveNodes(std::span{&position, 1});
}

void Session::moveNodes(std::span<const NodePosition> positions)
{
    if (positions.empty() || std::ranges::any_of(positions, [](const auto& position) {
            return !std::isfinite(position.x) || !std::isfinite(position.y);
        })) return;

    std::vector<int> ids;
    ids.reserve(positions.size());
    for (const auto& position : positions) ids.push_back(position.id);
    edit(ids, [&](auto& graph, const auto& mappedIds) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            graph.moveNode(mappedIds[i], positions[i].x, positions[i].y);
        }
    });
}

void Session::connect(SchematicLink link)
{
    stopSyncPainter();
    auto targets = editTargets({link.source, link.target});
    std::vector<SchematicLink> mapped;
    mapped.reserve(targets.size());
    const auto sourcePorts = graph().node(link.source).outputPorts();
    const auto targetPorts = graph().node(link.target).inputPorts();
    for (const auto& target : targets) {
        const int source = target.ids[0];
        const int destination = target.ids[1];
        const SchematicLink local{source, destination,
            correspondingPort(targetPorts, target.graph->node(destination).inputPorts(), link.input),
            correspondingPort(sourcePorts, target.graph->node(source).outputPorts(), link.output)};
        if (targets.size() > 1) {
            target.graph->checkConnection(local);
        }
        mapped.push_back(local);
    }
    std::exception_ptr failure;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        try {
            targets[i].graph->connect(mapped[i]);
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void Session::disconnectInput(int id, int input)
{
    const auto ports = graph().node(id).inputPorts();
    edit({id}, [&](auto& graph, const auto& ids) {
        const auto& node = graph.node(ids[0]);
        const auto remote = node.inputPorts();
        const auto name = ports.at(input).name;
        const auto match = std::ranges::find(remote, name, &Port::name);
        if (match != remote.end()) {
            graph.disconnectInput(node.id, correspondingPort(ports, remote, input));
        }
    });
}

void Session::disconnectLink(SchematicLink link)
{
    const auto outputs = graph().node(link.source).outputPorts();
    const auto inputs = graph().node(link.target).inputPorts();
    edit({link.source, link.target}, [&](auto& graph, const auto& ids) {
        graph.disconnectLink({ids[0], ids[1],
            correspondingPort(inputs, graph.node(ids[1]).inputPorts(), link.input),
            correspondingPort(outputs, graph.node(ids[0]).outputPorts(), link.output)});
    });
}

void Session::disconnectNode(int id)
{
    edit({id}, [](auto& graph, const auto& ids) { graph.disconnectNode(ids[0]); });
}

void Session::setTemplateType(int id, std::optional<green::Type> type)
{
    if (isModuleEndpoint(graph().node(id).kind)) {
        setModulePortType(id, type);
        return;
    }
    const auto targets = editTargets({id});
    if (targets.size() > 1) {
        for (const auto& target : targets) {
            target.graph->checkTemplateType(target.ids[0], type);
        }
    }
    edit({id}, [type](auto& graph, const auto& ids) { graph.setTemplateType(ids[0], type); });
}

void Session::setModulePortType(int id, std::optional<green::Type> type)
{
    stopSyncPainter();
    std::vector<std::pair<Schematic*, int>> ports;
    for (const auto& target : editTargets({id})) {
        ports.emplace_back(target.graph, target.ids.front());
    }
    Schematic::setModulePortTypes(ports, type);
}

void Session::setValue(int id, const green::Value& value)
{
    for (const auto& target : editTargets({id})) {
        if (!target.graph->editable(target.ids[0])) {
            throw std::invalid_argument("The value is driven by a link in a synchronized copy");
        }
    }
    edit({id}, [&](auto& graph, const auto& ids) { graph.setValue(ids[0], value); });
}

void Session::setPanelHidden(int id, bool hidden)
{
    edit({id}, [hidden](auto& graph, const auto& ids) { graph.setPanelHidden(ids[0], hidden); });
}

void Session::enableModulePanel(int id, ModulePanelKind panel)
{
    stopSyncPainter();
    const auto* owner = graph().module(id);
    if (!owner) {
        throw std::invalid_argument("Choose a Module");
    }
    const auto kind = panel == ModulePanelKind::Properties ? PrimitiveKind::ModulePropertiesGui : PrimitiveKind::ModuleGui;
    for (const auto* peer : synchronizedGraphs(*owner)) {
        if (std::ranges::none_of(peer->nodes_, [kind](const auto& node) { return node.kind == kind; })) {
            const auto first = std::ranges::min_element(peer->nodes_, {}, &SchematicNode::x);
            const auto x = first == peer->nodes_.end() ? 0 : first->x - 200;
            const_cast<Schematic*>(peer)->addNode(kind, x, 0);
        }
    }
}

void Session::setPanelShownInParent(int id, bool shown)
{
    if (!graph().module(id)) {
        throw std::invalid_argument("Choose a Module");
    }
    edit({id}, [shown](auto& graph, const auto& ids) { graph.node(ids[0]).showPanelInParent = shown; });
}

void Session::movePanelItem(int id, std::span<const int> itemPath, green::Point position,
                            ModulePanelKind surface, ModulePanelKind itemPanel)
{
    if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
        throw std::invalid_argument("Panel position must be finite");
    }
    editPanelItem(id, itemPath, surface, itemPanel, [position](auto& child, const auto& panel) {
        child.applyPanelArea(panel.view.panel, {position.x, position.y, panel.view.bounds.width, panel.view.bounds.height});
    });
}

void Session::setPanelItemAnchors(int id, std::span<const int> itemPath, ModulePanelKind surface,
                                  ModulePanelKind itemPanel, std::uint8_t anchors)
{
    editPanelItem(id, itemPath, surface, itemPanel, [anchors](auto& child, const auto& panel) {
        child.setPanelAnchors(panel.view.panel, anchors);
    });
}

void Session::editPanelItem(int id, std::span<const int> itemPath, ModulePanelKind surface, ModulePanelKind itemPanel,
                            const std::function<void(Schematic&, const ModulePanel&)>& apply)
{
    stopSyncPainter();
    static_cast<void>(graph().findPanelItem(id, itemPath, surface, itemPanel));
    auto* owner = graph().module(id);
    auto* source = owner;
    std::vector<std::size_t> rows;
    for (int item : itemPath) {
        rows.push_back(nodeIndex(*source, item));
        source = source->module(item);
    }
    for (const auto* peer : synchronizedGraphs(*owner)) {
        auto* parent = const_cast<Schematic*>(peer);
        for (auto row : std::span{rows}.first(rows.size() - 1)) {
            parent = parent->module(parent->nodes().at(row).id);
        }
        // The other instances may receive a different View. Keep their layout ready.
        const auto id = parent->nodes_.at(rows.back()).id;
        auto* child = parent->module(id);
        const auto panel = child->modulePanel(itemPanel);
        apply(*child, *panel);
        child->refreshConnections();
    }
}

void Session::resizePanel(int id, float width, float height, ModulePanelKind surface)
{
    stopSyncPainter();
    auto* owner = graph().module(id);
    if (!owner) {
        throw std::invalid_argument("Choose a Module with a panel");
    }
    std::exception_ptr failure;
    for (const auto* peer : synchronizedGraphs(*owner)) {
        try {
            peer->parent_->resizePanel(peer->path_.back(), width, height, surface);
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void Session::loadBitmap(int id, std::string_view path)
{
    edit({id}, [&](auto& graph, const auto& ids) { graph.loadBitmap(ids[0], path); });
}

Session::NodeCopy Session::snapshotNode(const Schematic& graph, int id, bool includeSequenceTakes)
{
    NodeCopy result{graph.node(id), {}, {}, {}};
    result.node.historyId = 0;
    result.node.localValue = authoredValue(result.node);
    copyNodeStorage(result.node);
    result.node.propertiesVisibility = PropertiesVisibility::Closed;
    result.node.panelUnlocked = false;
    result.node.dropListSelection.reset();
    result.node.editControl.reset();
    result.node.popupListIndex.reset();
    result.node.fileDrag.reset();
    result.node.fileDropSucceeded = false;
    result.node.lastMouseMessage = {};
    if (primitive(result.node.kind).behavior == PrimitiveBehavior::KeyboardInput) {
        result.node.capturedSignals.clear();
    }
    result.node.mouseDrag.reset();
    result.node.viewLoopValue.reset();
    result.node.arrayStorage.reset();
    result.node.greenLoopValue.reset();
    result.node.loopRunning = false;
    result.node.counterAscending = true;
    result.node.holdingClose = false;
    result.node.stateMachine = {};
    result.node.httpRequestToken = 0;
    result.node.embeddedFonts.registration.reset();
    const auto behavior = primitive(result.node.kind).behavior;
    if (behavior == PrimitiveBehavior::SystemAction || behavior == PrimitiveBehavior::HttpTransfer ||
        behavior == PrimitiveBehavior::ParseColorMatrix || behavior == PrimitiveBehavior::EmbedFonts ||
        result.node.kind == PrimitiveKind::ColorDialog) {
        result.node.capturedSignals.clear();
    }
    result.node.codeSourceDirty = false;
    result.node.lastSwitchInput = 0;
    result.node.matrix.reset();
    result.node.network.reset();
    result.node.sfz.reset();
    result.node.media.reset();
    result.node.mediaSamples.reset();
    if (!includeSequenceTakes) {
        result.node.sequenceAudio.reset();
        result.node.sequenceMidi.reset();
    }
    result.node.triggersSinceDivision = 0;
    result.node.previousTriggerTime.reset();
    result.node.arrayDrawIndex.reset();
    result.node.arrayTransformResult = {};
    result.node.viewCache.reset();
    result.node.window.reset();
    result.node.redraw = {};
    result.node.panelPaint.reset();
    if (auto& tick = result.node.tick) {
        tick->deadline.reset();
    }
    result.node.timer.reset();
    if (const auto& code = result.node.rubyCode) {
        auto snapshot = rubyCode(code->draft, code->source, code->manualPorts ? std::optional{code->ports} : std::nullopt);
        snapshot.savedState = code->savedState;
        snapshot.boxSize = code->boxSize;
        result.node.rubyCode = std::move(snapshot);
    }
    result.node.midi.reset();
    result.node.serialPort.reset();
    result.node.voiceBank.reset();
    result.node.legacyPresetInfo = {};
    if (auto& preset = result.node.presetManager) {
        preset->dispatching = false;
        preset->action = green::PresetAction::Recall;
    }
    if (auto& preset = result.node.presetParameter) preset->dispatching = false;
    if (auto& selection = result.node.midiSelection) {
        if (result.node.kind == PrimitiveKind::MidiInSelect || result.node.kind == PrimitiveKind::MidiOutSelect) {
            selection.reset();
        } else {
            *selection = {.ports = std::move(selection->ports)};
        }
    }
    result.node.localTime.reset();
    result.node.memoryCache = {};
    std::ranges::fill(result.node.inputTriggers, 0);
    std::ranges::fill(result.node.outputTriggers, 0);
    if (const auto* child = graph.module(id)) {
        result.syncGroup = child->syncGroup_;
        result.links = child->links_;
        for (const auto& entry : child->nodes_) {
            result.children.push_back(snapshotNode(*child, entry.id, includeSequenceTakes));
            result.portTypes.emplace(entry.id, child->ports_.at(entry.id).types);
        }
    }
    return result;
}

void Session::restoreGraph(Schematic& graph, const NodeCopy& content, bool restoreSync, bool buildRuntime)
{
    if (restoreSync) {
        graph.syncGroup_ = content.syncGroup;
    }
    graph.links_ = content.links;
    for (const auto& link : graph.links_) {
        graph.linkTriggers_.emplace(link, 0);
    }
    for (const auto& entry : content.children) {
        const auto id = entry.node.id;
        graph.nodes_.push_back(entry.node);
        copyNodeStorage(graph.nodes_.back());
        graph.nextId_ = std::max(graph.nextId_, id + 1);
        graph.ports_.emplace(id, Schematic::NodePorts{{}, {}, content.portTypes.at(id)});
        if (entry.node.kind == PrimitiveKind::Module) {
            auto child = std::unique_ptr<Schematic>(new Schematic(graph.bitmapLoader_, &graph, id));
            restoreGraph(*child, entry, restoreSync, buildRuntime);
            graph.modules_.emplace(id, std::move(child));
        }
    }
    if (buildRuntime) graph.rebuildRuntime(true);
}

int Session::insertCopy(Schematic& graph, const NodeCopy& content, double x, double y)
{
    auto copy = relocateCopy(content.node, graph.nextId_, x, y);
    std::unique_ptr<Schematic> restored;
    if (copy.kind == PrimitiveKind::Module) {
        restored.reset(new Schematic(graph.bitmapLoader_, &graph, graph.nextId_));
        restoreGraph(*restored, content);
    }
    const int id = graph.addNode(copy.kind, x, y);
    const auto& defaults = graph.node(id);
    copy.localTime = defaults.localTime;
    if (restored) {
        graph.modules_.at(id) = std::move(restored);
    } else if (copy.kind == PrimitiveKind::BusCreate || copy.kind == PrimitiveKind::BusExtract) {
        copy.channelNames.clear();
        copy.inputTriggers.resize(defaults.inputTriggers.size());
        copy.outputTriggers.resize(defaults.outputTriggers.size());
    }
    graph.nodes_.back() = std::move(copy);
    try {
        graph.rebuildAfterRemoval();
    } catch (...) {
        graph.modules_.erase(id);
        graph.ports_.erase(id);
        graph.nodes_.pop_back();
        throw;
    }
    graph.refreshConnections(false);
    graph.fireLifecycleTriggers(PrimitiveKind::AfterDuplicate, {&graph.node(id), 1}, lifecycleError_);
    return id;
}

void Session::copyNode(int id)
{
    copyNodes(std::span{&id, 1});
}

Session::SelectionCopy Session::snapshotSelection(std::span<const int> selection) const
{
    const std::set<int> selected(selection.begin(), selection.end());
    SelectionCopy copy;
    for (const auto& node : graph().nodes_) {
        if (!selected.contains(node.id)) continue;
        if (isModuleEndpoint(node.kind)) {
            throw std::invalid_argument("Copy the module to include its boundary connectors");
        }
        copy.nodes.push_back(snapshotNode(graph(), node.id));
        copy.portTypes.emplace(node.id, graph().ports_.at(node.id).types);
    }
    if (copy.nodes.size() != selected.size()) {
        throw std::invalid_argument("Choose nodes in the current schematic");
    }
    for (const auto link : graph().links_) {
        if (selected.contains(link.source) && selected.contains(link.target)) copy.links.push_back(link);
    }
    copy.left = std::ranges::min(copy.nodes | std::views::transform([](const auto& item) { return item.node.x; }));
    copy.top = std::ranges::min(copy.nodes | std::views::transform([](const auto& item) { return item.node.y; }));
    return copy;
}

void Session::copyNodes(std::span<const int> selection)
{
    if (selection.empty()) return;

    auto copy = snapshotSelection(selection);
    Path source;
    if (copy.nodes.size() == 1) {
        source = path_;
        source.push_back(copy.nodes.front().node.id);
    }
    clipboard_ = std::move(copy);
    clipboardSource_ = std::move(source);
}

void Session::cutNodes(std::span<const int> selection)
{
    if (selection.empty()) return;
    const auto targets = editTargets(selection);
    copyNodes(selection);
    // Capture the entire selection before removing a wire can change a node's runtime state.
    for (const auto& target : targets) {
        for (int id : target.ids) rememberRemovedNode(*target.graph, id);
    }
    stopSyncPainter();
    applyEdit(targets, [&](auto& graph, const auto& ids) {
        for (int id : ids) {
            if (auto* child = graph.module(id)) {
                forgetViewports(child->path_);
            }
        }
        graph.eraseNodes(ids);
    });
}

bool Session::canPaste() const
{
    return clipboard_ && std::ranges::all_of(clipboard_->nodes, [&](const auto& node) {
        return graph().canPlace(node.node.kind);
    });
}

bool Session::canPasteSynchronized() const
{
    if (!clipboard_ || clipboard_->nodes.size() != 1 || clipboard_->nodes.front().node.kind != PrimitiveKind::Module) {
        return false;
    }
    const auto* source = findGraph(clipboardSource_);
    if (!source) {
        return false;
    }
    const auto sources = synchronizedGraphs(*source);
    for (const auto* destination : synchronizedGraphs(graph())) {
        if (std::ranges::any_of(sources, [&](const auto* peer) {
                return isPathPrefix(peer->path_, destination->path_);
            })) {
            return false;
        }
    }
    return true;
}

int Session::pasteNode(double x, double y, bool synchronized)
{
    if (!canPaste() || clipboard_->nodes.size() != 1) {
        throw std::invalid_argument("Copy one node to paste it here");
    }
    if (synchronized && !canPasteSynchronized()) {
        throw std::invalid_argument("Copy a module from outside this module to paste a synchronized instance");
    }
    lifecycleError_ = {};
    auto* source = synchronized ? findGraph(clipboardSource_) : nullptr;
    const auto content = source ? snapshotNode(*source->parent_, source->path_.back()) : clipboard_->nodes.front();
    std::vector<Schematic*> added;
    int id = -1;
    edit({}, [&](auto& graph, const auto&) {
        const int pasted = insertCopy(graph, content, x, y);
        if (source) {
            added.push_back(graph.module(pasted));
        }
        if (&graph == activeGraph_) {
            id = pasted;
        }
    });
    if (source) {
        joinSyncGroup(*source, added);
    }
    return id;
}

std::vector<int> Session::pasteNodes(double x, double y, bool synchronized)
{
    if (!canPaste()) throw std::invalid_argument("The copied selection cannot be placed here");
    const auto& copy = *clipboard_;
    if (copy.nodes.size() == 1) return {pasteNode(x, y, synchronized)};
    if (synchronized) throw std::invalid_argument("Copy one module to paste a synchronized instance");
    return insertSelection(copy, x, y);
}

std::vector<int> Session::duplicateNodes(std::span<const int> selection, double dx, double dy)
{
    if (selection.empty()) return {};
    const auto copy = snapshotSelection(selection);
    return insertSelection(copy, copy.left + dx, copy.top + dy);
}

std::vector<int> Session::insertSelection(const SelectionCopy& copy, double x, double y)
{
    lifecycleError_ = {};
    struct Destination {
        Schematic* graph;
        int firstId;
        std::size_t nodeCount;
        std::size_t linkCount;
    };
    std::vector<Destination> destinations;
    for (const auto& target : editTargets({})) {
        auto& graph = *target.graph;
        destinations.push_back({&graph, graph.nextId_, graph.nodes_.size(), graph.links_.size()});
    }

    stopSyncPainter();
    std::vector<int> pasted;
    pasted.reserve(copy.nodes.size());
    try {
        for (const auto& destination : destinations) {
            auto& graph = *destination.graph;
            std::map<int, int> ids;
            for (const auto& item : copy.nodes) {
                const int id = graph.nextId_++;
                ids.emplace(item.node.id, id);
                auto node = relocateCopy(item.node, id, x + (item.node.x - copy.left), y + (item.node.y - copy.top));
                if (node.kind == PrimitiveKind::Module) {
                    auto child = std::make_unique<Schematic>(graph.bitmapLoader_, graph.bitmapBackend_, graph.textMeasurer_);
                    restoreGraph(*child, item);
                    child->reparent(graph, id);
                    graph.modules_.emplace(id, std::move(child));
                }
                graph.nodes_.push_back(std::move(node));
                graph.ports_.emplace(id, Schematic::NodePorts{{}, {}, copy.portTypes.at(item.node.id)});
                if (&graph == activeGraph_) pasted.push_back(id);
            }
            for (auto link : copy.links) {
                link.source = ids.at(link.source);
                link.target = ids.at(link.target);
                graph.links_.push_back(link);
                graph.linkTriggers_.emplace(link, 0);
            }
        }
        // Install complete fragments so buses, templates and fan-in see all their copied inputs.
        root_->rebuildConnectedRuntimes(true, true);
    } catch (...) {
        for (const auto& destination : destinations) {
            auto& graph = *destination.graph;
            for (const auto& link : std::span(graph.links_).subspan(destination.linkCount)) {
                graph.linkTriggers_.erase(link);
            }
            graph.links_.resize(destination.linkCount);
            graph.nodes_.resize(destination.nodeCount);
            graph.ports_.erase(graph.ports_.lower_bound(destination.firstId), graph.ports_.end());
            graph.modules_.erase(graph.modules_.lower_bound(destination.firstId), graph.modules_.end());
            graph.nextId_ = destination.firstId;
        }
        throw;
    }
    ++root_->drawingTopology_;
    root_->refreshConnections(false);
    for (const auto& destination : destinations) {
        auto& graph = *destination.graph;
        graph.fireLifecycleTriggers(PrimitiveKind::AfterDuplicate,
                                    std::span(graph.nodes_).subspan(destination.nodeCount), lifecycleError_);
    }
    return pasted;
}

void Session::saveModuleToToolbox(int id, std::string group)
{
    if (!graph().module(id)) {
        throw std::invalid_argument("Choose a Module to add to the toolbox");
    }
    toolbox_.push_back({snapshotNode(graph(), id), std::move(group)});
}

std::vector<std::string> Session::toolboxNames() const
{
    std::vector<std::string> names;
    names.reserve(toolbox_.size());
    for (const auto& saved : toolbox_) {
        names.push_back(saved.content.node.moduleName);
    }
    return names;
}

const std::string& Session::toolboxGroup(std::size_t index) const
{
    return toolbox_.at(index).group;
}

const SchematicNode& Session::toolboxModule(std::size_t index) const
{
    return toolbox_.at(index).content.node;
}

int Session::insertToolboxModule(std::size_t index, double x, double y)
{
    lifecycleError_ = {};
    const auto& content = toolbox_.at(index).content;
    int id = -1;
    edit({}, [&](auto& graph, const auto&) {
        const int added = insertCopy(graph, content, x, y);
        if (&graph == activeGraph_) {
            id = added;
        }
    });
    return id;
}

bool Session::sameModule(const Schematic& a, const Schematic& b)
{
    if (a.parent_->node(a.path_.back()).moduleName != b.parent_->node(b.path_.back()).moduleName ||
        a.nodes_.size() != b.nodes_.size() || a.links_.size() != b.links_.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.nodes_.size(); ++i) {
        const auto& first = a.nodes_[i];
        const auto& second = b.nodes_[i];
        if (first.kind != second.kind || first.x != second.x || first.y != second.y ||
            first.typeOverride != second.typeOverride || first.label != second.label ||
            first.codeSourcePorts != second.codeSourcePorts ||
            first.compareArrayElements != second.compareArrayElements ||
            first.wirelessLabel != second.wirelessLabel || first.panelHidden != second.panelHidden ||
            first.showPanelInParent != second.showPanelInParent || first.panelAnchors != second.panelAnchors ||
            first.anchorReference != second.anchorReference ||
            first.panelPosition != second.panelPosition || first.propertiesPosition != second.propertiesPosition ||
            first.panelBounds != second.panelBounds ||
            first.channelNames != second.channelNames ||
            (first.dspCode && first.dspCode->source != second.dspCode->source) ||
            (first.rubyCode && (first.rubyCode->source != second.rubyCode->source ||
             first.rubyCode->ports != second.rubyCode->ports || first.rubyCode->manualPorts != second.rubyCode->manualPorts ||
             first.rubyCode->boxSize != second.rubyCode->boxSize)) ||
            first.boundaryIndex != second.boundaryIndex || !sameValue(first, second)) {
            return false;
        }
        if (first.kind == PrimitiveKind::Module && !sameModule(*a.module(first.id), *b.module(second.id))) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.links_.size(); ++i) {
        const auto& first = a.links_[i];
        const auto& second = b.links_[i];
        if (first.input != second.input || first.output != second.output ||
            nodeIndex(a, first.source) != nodeIndex(b, second.source) ||
            nodeIndex(a, first.target) != nodeIndex(b, second.target)) {
            return false;
        }
    }
    return true;
}

void Session::joinSyncGroup(Schematic& source, const std::vector<Schematic*>& members)
{
    if (members.empty()) {
        return;
    }
    const auto alreadySynced = synchronizedGraphs(source);
    if (std::ranges::all_of(members, [&](auto* member) { return std::ranges::contains(alreadySynced, member); })) {
        return;
    }
    const auto group = source.syncGroup_ == 0 ? nextSyncGroup_++ : source.syncGroup_;
    std::vector<std::uint64_t> groups;
    for (auto* member : members) {
        if (member->syncGroup_ != 0) {
            groups.push_back(member->syncGroup_);
        }
    }
    for (const auto* module : moduleGraphs()) {
        if (std::ranges::contains(groups, module->syncGroup_)) {
            const_cast<Schematic*>(module)->syncGroup_ = group;
        }
    }
    source.syncGroup_ = group;
    for (auto* member : members) {
        member->syncGroup_ = group;
    }
}

std::size_t Session::syncCount(int id) const
{
    const auto* target = graph().module(id);
    return target ? synchronizedGraphs(*target).size() - 1 : 0;
}

bool Session::hasSyncGroup(int id) const
{
    const auto* target = graph().module(id);
    return target && target->syncGroup_ != 0 && syncCount(id) != 0;
}

void Session::synchronizeAll(int id)
{
    auto* source = graph().module(id);
    if (!source) {
        return;
    }
    std::vector<Schematic*> matches;
    for (const auto* module : moduleGraphs()) {
        if (module != source && sameModule(*source, *module)) {
            matches.push_back(const_cast<Schematic*>(module));
        }
    }
    if (matches.empty()) {
        throw std::invalid_argument("No identical modules found");
    }
    joinSyncGroup(*source, matches);
}

void Session::unsynchronize(int id, bool all)
{
    auto* source = graph().module(id);
    if (!source || source->syncGroup_ == 0) {
        return;
    }
    const auto group = source->syncGroup_;
    source->syncGroup_ = 0;
    if (all) {
        for (const auto* module : moduleGraphs()) {
            if (module->syncGroup_ == group) {
                const_cast<Schematic*>(module)->syncGroup_ = 0;
            }
        }
    }
}

int Session::followSync(int id, bool previous)
{
    auto* source = graph().module(id);
    if (!source) {
        return -1;
    }
    auto peers = synchronizedGraphs(*source);
    if (peers.size() < 2) {
        return -1;
    }
    std::ranges::sort(peers, {}, &Schematic::path_);
    const auto current = static_cast<std::size_t>(std::ranges::find(peers, source) - peers.begin());
    const auto* peer = peers[(current + (previous ? peers.size() - 1 : 1)) % peers.size()];
    activeGraph_ = peer->parent_;
    path_ = activeGraph_->path_;
    viewports_.try_emplace(path_);
    return peer->path_.back();
}

void Session::startSyncPainter(int id)
{
    if (const auto* source = graph().module(id)) {
        painterSource_ = source->path_;
    }
}

bool Session::syncPainterActive() const
{
    return !painterSource_.empty() && findGraph(painterSource_);
}

void Session::paintSync(int id)
{
    auto* source = syncPainterActive() ? findGraph(painterSource_) : nullptr;
    auto* target = graph().module(id);
    if (!source || !target || source == target) {
        return;
    }
    if (std::ranges::contains(synchronizedGraphs(*source), target)) {
        target->syncGroup_ = 0;
        return;
    }
    if (!sameModule(*source, *target)) {
        throw std::invalid_argument("Only identical modules can be synchronized");
    }
    joinSyncGroup(*source, {target});
}

}
