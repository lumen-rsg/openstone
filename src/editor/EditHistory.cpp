#include "Session.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace openstone {

struct Session::History {
    struct NodeLayout {
        int id;
        std::uint64_t identity;
        double x;
        double y;
        Schematic::PortTypes types;
        std::vector<ModuleConnector> inputs;
        std::vector<ModuleConnector> outputs;
        std::vector<std::string> channels;
        std::optional<green::Type> boundaryType;
        int boundaryIndex;
        std::optional<stream::DspProgram> dspProgram;
        std::optional<std::string> rubySource;
        std::optional<RubyPorts> rubyManualPorts;
        RubyBoxSize rubyBoxSize;

        bool sameWiring(const NodeLayout& other) const
        {
            return id == other.id && identity == other.identity && channels == other.channels &&
                   boundaryIndex == other.boundaryIndex && sameDspPorts(other) && rubySource == other.rubySource &&
                   rubyManualPorts == other.rubyManualPorts &&
                   std::ranges::equal(inputs, other.inputs, {}, &ModuleConnector::endpoint, &ModuleConnector::endpoint) &&
                   std::ranges::equal(outputs, other.outputs, {}, &ModuleConnector::endpoint, &ModuleConnector::endpoint);
        }

        bool sameDspPorts(const NodeLayout& other) const
        {
            if (!dspProgram || !other.dspProgram) return dspProgram.has_value() == other.dspProgram.has_value();
            const auto samePort = [](const stream::DspPort& a, const stream::DspPort& b) {
                return a.name == b.name && a.type == b.type && a.arraySize == b.arraySize;
            };
            return std::ranges::equal(dspProgram->inputs(), other.dspProgram->inputs(), samePort) &&
                   std::ranges::equal(dspProgram->outputs(), other.dspProgram->outputs(), samePort);
        }

        bool operator==(const NodeLayout& other) const
        {
            return x == other.x && y == other.y && rubyBoxSize == other.rubyBoxSize && sameWiring(other);
        }
    };

    struct GraphLayout {
        std::vector<NodeLayout> nodes;
        std::vector<SchematicLink> links;
        std::map<SchematicLink, std::uint64_t> counts;
        std::uint64_t identity;
        std::uint64_t syncGroup;

        bool sameWiring(const GraphLayout& other) const
        {
            return identity == other.identity && syncGroup == other.syncGroup && links == other.links &&
                   std::ranges::equal(nodes, other.nodes, &NodeLayout::sameWiring);
        }

        bool operator==(const GraphLayout& other) const
        {
            return identity == other.identity && syncGroup == other.syncGroup && links == other.links && nodes == other.nodes;
        }
    };

    using Layout = std::map<Path, GraphLayout>;
    using StoredNodes = std::map<std::uint64_t, SchematicNode>;
    struct Entry {
        SchematicEdit edit;
        Path path;
        Layout before;
        Layout after;
        StoredNodes removed;
    };

    Layout capture(Schematic& root)
    {
        Layout result;
        const auto visit = [&](const auto& self, Schematic& graph, std::uint64_t identity) -> void {
            GraphLayout layout{{}, graph.links_, graph.linkTriggers_, identity, graph.syncGroup_};
            layout.nodes.reserve(graph.nodes_.size());
            for (auto& node : graph.nodes_) {
                if (node.historyId == 0) {
                    node.historyId = nextIdentity++;
                }
                layout.nodes.push_back({node.id, node.historyId, node.x, node.y, graph.ports_.at(node.id).types,
                                       node.moduleInputs, node.moduleOutputs, node.channelNames,
                                       node.boundaryType, node.boundaryIndex,
                                       node.dspCode ? std::optional{node.dspCode->program} : std::nullopt,
                                       node.rubyCode ? std::optional{node.rubyCode->source} : std::nullopt,
                                       node.rubyCode && node.rubyCode->manualPorts
                                           ? std::optional{node.rubyCode->ports} : std::nullopt,
                                       node.rubyCode ? node.rubyCode->boxSize : RubyBoxSize{}});
                if (auto* child = graph.module(node.id)) {
                    self(self, *child, node.historyId);
                }
            }
            result.emplace(graph.path_, std::move(layout));
        };
        visit(visit, root, 0);
        return result;
    }

    static void rememberNode(const Schematic& graph, int id, StoredNodes& removed)
    {
        const auto& node = graph.node(id);
        auto saved = node;
        saved.serialPort.reset();
        removed.insert_or_assign(node.historyId, std::move(saved));
        if (const auto* child = graph.module(id)) {
            for (const auto& member : child->nodes_) {
                rememberNode(*child, member.id, removed);
            }
        }
    }

    static void restore(Session& session, const Layout& target, const Layout& current, Entry& entry,
                        const std::function<void(HistoryUpdate)>& beforeApply)
    {
        std::map<std::uint64_t, const SchematicNode*> liveNodes;
        std::map<std::uint64_t, const Schematic*> liveGraphs;
        std::map<std::uint64_t, Path> newPaths;
        std::map<std::uint64_t, Viewport> viewports;
        std::set<std::uint64_t> targetNodes;
        for (const auto& [path, layout] : current) {
            const auto* graph = session.findGraph(path);
            liveGraphs.emplace(layout.identity, graph);
            if (auto saved = session.viewports_.find(path); saved != session.viewports_.end()) {
                viewports.emplace(layout.identity, saved->second);
            }
            for (const auto& node : graph->nodes_) {
                liveNodes.emplace(node.historyId, &node);
            }
        }
        for (const auto& [path, layout] : target) {
            newPaths.emplace(layout.identity, path);
            for (const auto& node : layout.nodes) {
                targetNodes.insert(node.identity);
            }
        }

        auto& oldRoot = *session.root_;
        auto replacement = std::make_unique<Schematic>(oldRoot.bitmapLoader_, oldRoot.bitmapBackend_,
                                                       oldRoot.textMeasurer_, oldRoot.textFiles_, oldRoot.systemInfo_);
        replacement->systemFontNames_ = oldRoot.systemFontNames_;
        replacement->sampleRate_ = oldRoot.sampleRate_;
        replacement->rubyClockStart_ = oldRoot.rubyClockStart_;
        replacement->sequenceDuration_ = oldRoot.sequencerMode() == stream::SequenceMode::Recording
            ? std::clamp(oldRoot.sequenceElapsed(oldRoot.sequenceNow()), TickTime::duration{},
                         TickTime::duration{stream::maxSequenceSeconds})
            : oldRoot.sequenceDuration_;
        const auto build = [&](const auto& self, Schematic& graph) -> void {
            const auto& layout = target.at(graph.path_);
            graph.syncGroup_ = layout.syncGroup;
            graph.links_ = layout.links;
            graph.linkTriggers_ = layout.counts;
            if (const auto previous = liveGraphs.find(layout.identity); previous != liveGraphs.end()) {
                graph.nextId_ = previous->second->nextId_;
                graph.triggers_ = previous->second->triggers_;
                for (auto& [link, count] : graph.linkTriggers_) {
                    if (const auto actual = previous->second->linkTriggers_.find(link);
                        actual != previous->second->linkTriggers_.end()) {
                        count = actual->second;
                    }
                }
            }
            for (const auto& item : layout.nodes) {
                const auto live = liveNodes.find(item.identity);
                auto node = live == liveNodes.end() ? entry.removed.at(item.identity) : *live->second;
                node.x = item.x;
                node.y = item.y;
                node.channelNames = item.channels;
                node.boundaryType = item.boundaryType;
                node.boundaryIndex = item.boundaryIndex;
                const auto remapPorts = [&](auto& currentPorts, auto& triggers, const auto& ports) {
                    auto previous = std::exchange(currentPorts, ports);
                    auto counts = std::exchange(triggers, std::vector<std::uint64_t>(ports.size()));
                    for (std::size_t i = 0; i < ports.size(); ++i) {
                        const auto old = std::ranges::find(previous, ports[i].endpoint, &ModuleConnector::endpoint);
                        if (old != previous.end()) {
                            currentPorts[i].name = old->name;
                            triggers[i] = counts.at(old - previous.begin());
                        }
                    }
                };
                if (node.kind == PrimitiveKind::Module) {
                    remapPorts(node.moduleInputs, node.inputTriggers, item.inputs);
                    remapPorts(node.moduleOutputs, node.outputTriggers, item.outputs);
                }
                if (item.rubySource && (node.rubyCode->source != *item.rubySource ||
                    node.rubyCode->manualPorts != item.rubyManualPorts.has_value() ||
                    (item.rubyManualPorts && node.rubyCode->ports != *item.rubyManualPorts))) {
                    const auto& draft = node.rubyCode->source == *item.rubySource ? node.rubyCode->draft : *item.rubySource;
                    auto code = rubyCode(draft, *item.rubySource, item.rubyManualPorts);
                    code.savedState = std::move(node.rubyCode->savedState);
                    node.rubyCode = std::move(code);
                    node.inputTriggers.resize(node.rubyCode->ports.inputs.size());
                    node.outputTriggers.resize(node.rubyCode->ports.outputs.size());
                }
                if (node.rubyCode) node.rubyCode->boxSize = item.rubyBoxSize;
                if (node.kind == PrimitiveKind::BusCreate) node.inputTriggers.resize(item.channels.size() + 1);
                if (node.kind == PrimitiveKind::BusExtract) node.outputTriggers.resize(item.channels.size());
                graph.nextId_ = std::max(graph.nextId_, node.id + 1);
                graph.ports_.emplace(node.id, Schematic::NodePorts{{}, {}, item.types});
                if (node.kind == PrimitiveKind::Module) {
                    auto child = std::unique_ptr<Schematic>(new Schematic(graph.bitmapLoader_, &graph, node.id));
                    self(self, *child);
                    graph.modules_.emplace(node.id, std::move(child));
                }
                graph.nodes_.push_back(std::move(node));
            }
        };
        replacement->location_ = oldRoot.location_;
        replacement->hostRedrawsSuspended_ = oldRoot.hostRedrawsSuspended_;
        replacement->pluginHostTransport_ = oldRoot.pluginHostTransport_;
        replacement->pluginEditorOpen_ = oldRoot.pluginEditorOpen_;
        replacement->settingsStore_ = oldRoot.settingsStore_;
        replacement->serialBackend_ = oldRoot.serialBackend_;
        replacement->serialPorts_ = oldRoot.serialPorts_;
        build(build, *replacement);
        replacement->rebuildConnectedRuntimes(true, true);
        replacement->refreshConnections(false);

        std::map<Path, Viewport> newViewports;
        for (const auto& [identity, viewport] : viewports) {
            if (const auto path = newPaths.find(identity); path != newPaths.end()) {
                newViewports.emplace(path->second, viewport);
            }
        }
        auto path = entry.path;
        auto clipboard = session.clipboardSource_;
        if (auto source = current.find(clipboard); source != current.end() && !clipboard.empty()) {
            const auto newPath = newPaths.find(source->second.identity);
            clipboard = newPath == newPaths.end() ? Path{} : newPath->second;
        }
        newViewports.try_emplace(path);

        for (const auto& [identity, node] : liveNodes) {
            if (!targetNodes.contains(identity)) {
                auto saved = *node;
                saved.serialPort.reset();
                entry.removed.insert_or_assign(identity, std::move(saved));
            }
        }

        beforeApply(HistoryUpdate::Topology);
        replacement->setMidiDeviceBackend(oldRoot.midiDevices_.backend);
        replacement->audioControls_ = std::move(oldRoot.audioControls_);
        replacement->rubyEvents_ = std::move(oldRoot.rubyEvents_);
        replacement->pruneRubyEvents();
        session.root_ = std::move(replacement);
        session.path_ = std::move(path);
        session.viewports_ = std::move(newViewports);
        session.activeGraph_ = session.findGraph(session.path_);
        session.clipboardSource_ = std::move(clipboard);
    }

    std::vector<Entry> entries;
    std::size_t position = 0;
    std::uint64_t nextIdentity = 1;
    std::optional<Entry> pending;
};

Session::Session(Schematic::BitmapLoader bitmapLoader, Schematic::BitmapBackend bitmapBackend,
                 Schematic::TextMeasurer textMeasurer, Schematic::TextFileBackend textFiles,
                 Schematic::SystemInfoBackend systemInfo)
    : root_(std::make_unique<Schematic>(std::move(bitmapLoader), std::move(bitmapBackend),
                                        std::move(textMeasurer), std::move(textFiles), std::move(systemInfo))),
      history_(std::make_unique<History>())
{
    viewports_.try_emplace(path_);
}

Session::~Session() = default;

bool Session::beginEdit(SchematicEdit edit)
{
    if (history_->pending) return false;
    history_->pending.emplace(History::Entry{edit, path_, history_->capture(*root_), {}, {}});
    return true;
}

void Session::finishEdit()
{
    auto pending = std::exchange(history_->pending, std::nullopt);
    if (!pending) return;
    pending->after = history_->capture(*root_);
    if (pending->before == pending->after) return;
    auto& entries = history_->entries;
    entries.erase(entries.begin() + history_->position, entries.end());
    constexpr std::size_t maxHistory = 64;
    if (entries.size() == maxHistory) entries.erase(entries.begin());
    entries.push_back(std::move(*pending));
    history_->position = entries.size();
}

void Session::rememberRemovedNode(const Schematic& graph, int id)
{
    if (history_->pending) {
        History::rememberNode(graph, id, history_->pending->removed);
    }
}

void Session::clearHistory()
{
    history_->entries.clear();
    history_->position = 0;
    history_->pending.reset();
}

std::optional<SchematicEdit> Session::historyEdit(bool redo) const
{
    const auto position = history_->position;
    if (history_->pending || (redo ? position == history_->entries.size() : position == 0)) return {};
    return history_->entries[redo ? position : position - 1].edit;
}

bool Session::applyHistory(bool redo, const std::function<void(HistoryUpdate)>& beforeApply)
{
    if (!historyEdit(redo)) return false;
    auto& entry = history_->entries[redo ? history_->position : history_->position - 1];
    const auto& expected = redo ? entry.before : entry.after;
    const auto& target = redo ? entry.after : entry.before;
    const auto current = history_->capture(*root_);
    if (current != expected) {
        clearHistory();
        throw std::runtime_error("A separate change to the schematic invalidated the edit history");
    }

    const bool positionOnly = std::ranges::equal(expected, target, [](const auto& first, const auto& second) {
        return first.first == second.first && first.second.sameWiring(second.second);
    });
    if (positionOnly) {
        auto path = entry.path;
        viewports_.try_emplace(path);
        const auto resized = std::ranges::any_of(target, [&](const auto& item) {
            return !std::ranges::equal(item.second.nodes, expected.at(item.first).nodes, {},
                                       &History::NodeLayout::rubyBoxSize, &History::NodeLayout::rubyBoxSize);
        });
        beforeApply(path != path_ ? HistoryUpdate::Navigation
                                 : resized ? HistoryUpdate::Geometry : HistoryUpdate::Positions);
        for (const auto& [location, layout] : target) {
            auto* graph = findGraph(location);
            for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
                graph->nodes_[i].x = layout.nodes[i].x;
                graph->nodes_[i].y = layout.nodes[i].y;
                if (auto& code = graph->nodes_[i].rubyCode) code->boxSize = layout.nodes[i].rubyBoxSize;
            }
        }
        path_ = std::move(path);
        activeGraph_ = findGraph(path_);
    } else {
        History::restore(*this, target, current, entry, beforeApply);
    }
    stopSyncPainter();
    if (redo) ++history_->position;
    else --history_->position;
    return true;
}

}
