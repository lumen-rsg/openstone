#include "Session.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace openstone {
namespace {

struct PortLocation {
    const Schematic* graph;
    int node;
    int port;
};

}

bool Schematic::wouldCreateFeedback(SchematicLink proposed) const
{
    const auto& source = node(proposed.source);
    const auto& resolved = ports_.at(proposed.source).types.outputs;
    const auto declared = source.outputPorts();
    const auto type = source.dspCode ? declared[proposed.output].type
                    : std::cmp_less(proposed.output, resolved.size()) ? resolved[proposed.output] : std::nullopt;
    // The audio scheduler can carry stream feedback across samples. Green callbacks still need an acyclic path.
    if (type && green::isDspType(*type)) return false;
    return reachesConnector(proposed.target, proposed.input, ConnectorSide::Input, proposed.source, proposed.output);
}

bool Schematic::reachesConnector(int start, int port, ConnectorSide side, int target, int targetPort) const
{
    std::vector<PortLocation> pending;
    std::set<std::tuple<const Schematic*, int, int>> visited;
    std::set<std::tuple<const Schematic*, int, int>> sent;
    const auto visitOutput = [&](const Schematic& graph, int source, int output) {
        if (!sent.emplace(&graph, source, output).second) {
            return false;
        }
        if (side == ConnectorSide::Input && &graph == this && source == target && output == targetPort) {
            return true;
        }
        for (const auto& link : graph.links_) {
            if (link.source == source && link.output == output) {
                pending.push_back({&graph, link.target, link.input});
            }
        }
        return false;
    };

    if (side == ConnectorSide::Input) {
        pending.push_back({this, start, port});
    } else {
        visitOutput(*this, start, port);
    }
    while (!pending.empty()) {
        const auto [graph, id, input] = pending.back();
        pending.pop_back();
        if (!visited.emplace(graph, id, input).second) {
            continue;
        }
        if (side == ConnectorSide::Output && graph == this && id == target && input == targetPort) {
            return true;
        }
        const auto& entry = graph->node(id);
        if (entry.kind == PrimitiveKind::Module) {
            // A module's inputs can reach entirely different sets of outputs.
            const auto* child = graph->module(id);
            if (visitOutput(*child, entry.moduleInputs.at(input).endpoint, 0)) {
                return true;
            }
        } else if (entry.kind == PrimitiveKind::ModuleOutput) {
            if (visitOutput(*graph->parent_, graph->path_.back(), entry.boundaryIndex)) {
                return true;
            }
            for (const auto& property : graph->nodes_) {
                if (property.kind == PrimitiveKind::ModuleInputProperty &&
                    graph->propertyBoundary(property) == &entry && visitOutput(*graph, property.id, 0)) {
                    return true;
                }
            }
        } else if (entry.kind == PrimitiveKind::ModuleOutputProperty) {
            if (const auto* boundary = graph->propertyBoundary(entry);
                boundary && graph->propertyWriter(*boundary) == &entry && visitOutput(*graph, boundary->id, 0)) {
                return true;
            }
        } else if (entry.dspCode) {
            const auto& program = entry.dspCode->program;
            for (std::size_t output = 0; output < program.outputs().size(); ++output) {
                if (program.outputDependsOnInput(output, input) && visitOutput(*graph, id, static_cast<int>(output))) {
                    return true;
                }
            }
        } else if (isStreamDelay(entry.kind)) {
            continue; // The delay publishes the previous frame before sampling its input.
        } else if (entry.kind == PrimitiveKind::VoicesToPoly && input == 1) {
            // Done retires a released voice after the frame that produced it.
            continue;
        } else if (entry.kind != PrimitiveKind::AboutToClose && entry.kind != PrimitiveKind::TriggerThread &&
                   entry.kind != PrimitiveKind::CustomTimer) {
            // The excluded inputs end the synchronous cascade.
            const auto outputCount = graph->ports_.at(id).outputs.size();
            for (std::size_t output = 0; output < outputCount; ++output) {
                if (visitOutput(*graph, id, static_cast<int>(output))) {
                    return true;
                }
            }
        }
    }
    return false;
}

void Schematic::validatePropertyRoutes() const
{
    for (const auto& property : nodes_) {
        if (!isPropertyEndpoint(property.kind)) continue;
        const auto* boundary = propertyBoundary(property);
        if (!boundary) continue;
        const bool input = property.kind == PrimitiveKind::ModuleInputProperty;
        if ((input || propertyWriter(*boundary) == &property) &&
            reachesConnector(input ? property.id : boundary->id, 0, ConnectorSide::Output,
                             input ? boundary->id : property.id, 0)) {
            throw std::invalid_argument("The property connection would create a feedback loop");
        }
    }
}

void Schematic::reparent(Schematic& parent, int moduleId)
{
    parent_ = &parent;
    path_ = parent.path_;
    path_.push_back(moduleId);
    wirelessRoutes_.clear();
    for (auto& [id, child] : modules_) {
        child->reparent(*this, id);
    }
}

int Schematic::makeModule(std::span<const int> selection)
{
    const std::set<int> selected(selection.begin(), selection.end());
    if (selected.empty()) {
        throw std::invalid_argument("Select nodes to make a module");
    }

    const int moduleId = nextId_;
    auto child = std::unique_ptr<Schematic>(new Schematic(bitmapLoader_, this, moduleId));
    child->nextId_ = nextId_;
    auto outsideNodes = std::vector<SchematicNode>{};
    std::map<int, NodePorts> outsidePorts;
    for (const auto& entry : nodes_) {
        if (selected.contains(entry.id)) {
            if (isModuleEndpoint(entry.kind) || isPropertyEndpoint(entry.kind)) {
                throw std::invalid_argument("Module boundary connectors stay in their owning module");
            }
            child->nodes_.push_back(entry);
            child->ports_.emplace(entry.id, ports_.at(entry.id));
        } else {
            outsideNodes.push_back(entry);
            outsidePorts.emplace(entry.id, ports_.at(entry.id));
        }
    }
    if (selected.size() != child->nodes_.size()) {
        throw std::invalid_argument("Choose nodes in the current schematic");
    }

    const double x = std::ranges::min_element(child->nodes_, {}, &SchematicNode::x)->x;
    const double y = std::ranges::min_element(child->nodes_, {}, &SchematicNode::y)->y;
    const double right = std::ranges::max_element(child->nodes_, {}, &SchematicNode::x)->x;
    SchematicNode wrapper{.id = moduleId, .kind = PrimitiveKind::Module, .x = x, .y = y};
    wrapper.moduleName = "Module";
    for (auto& entry : child->nodes_) {
        entry.x -= x;
        entry.y -= y;
    }

    std::vector<SchematicLink> outsideLinks;
    std::map<SchematicLink, std::uint64_t> outsideCounts;
    const auto keepOutside = [&](SchematicLink link, std::uint64_t count) {
        outsideLinks.push_back(link);
        outsideCounts.emplace(link, count);
    };
    const auto keepInside = [&](SchematicLink link, std::uint64_t count) {
        child->links_.push_back(link);
        child->linkTriggers_.emplace(link, count);
    };

    for (const auto& link : links_) {
        const auto count = linkTriggers_.at(link);
        const bool sourceInside = selected.contains(link.source);
        const bool targetInside = selected.contains(link.target);
        if (sourceInside == targetInside) {
            if (sourceInside) {
                keepInside(link, count);
            } else {
                keepOutside(link, count);
            }
            continue;
        }

        // Seed the effective type so conversion and fan-in still happen at the old target.
        // The new endpoint stays in Template mode and can infer again on the next edit.
        const auto type = outputType(link.source, link.output);
        auto& connectors = targetInside ? wrapper.moduleInputs : wrapper.moduleOutputs;
        const int index = static_cast<int>(connectors.size());
        const auto label = targetInside ? node(link.target).inputPorts().at(link.input).name
                                        : node(link.source).outputPorts().at(link.output).name;
        const std::string name = label.empty()
            ? (targetInside ? "In " : "Out ") + std::to_string(index + 1)
            : std::string(label);
        std::string uniqueName = name;
        for (int suffix = 2; std::ranges::any_of(connectors, [&](const auto& port) {
                 return port.name == uniqueName;
             }); ++suffix) {
            uniqueName = name + " " + std::to_string(suffix);
        }
        const int endpointId = child->nextId_++;
        SchematicNode endpoint{
            .id = endpointId,
            .kind = targetInside ? PrimitiveKind::ModuleInput : PrimitiveKind::ModuleOutput,
            .x = targetInside ? -210.0 : right - x + 320.0,
            .y = 86.0 * index,
        };
        endpoint.label = uniqueName;
        endpoint.boundaryType = type;
        endpoint.boundaryIndex = index;
        endpoint.inputTriggers.resize(targetInside ? 0 : 1);
        endpoint.outputTriggers.resize(targetInside ? 1 : 0);
        child->ports_.emplace(endpointId, NodePorts{{}, {}, child->resolveTypes(endpoint, {}, {})});
        child->nodes_.push_back(std::move(endpoint));
        connectors.push_back({std::move(uniqueName), type, endpointId});

        if (targetInside) {
            keepOutside({link.source, moduleId, index, link.output}, count);
            keepInside({endpointId, link.target, link.input, 0}, count);
        } else {
            keepOutside({moduleId, link.target, link.input, index}, count);
            keepInside({link.source, endpointId, 0, link.output}, count);
        }
    }

    wrapper.inputTriggers.resize(wrapper.moduleInputs.size());
    wrapper.outputTriggers.resize(wrapper.moduleOutputs.size());
    outsidePorts.emplace(moduleId, NodePorts{{}, {}, resolveTypes(wrapper, {}, {})});
    outsideNodes.push_back(std::move(wrapper));
    child->rebuildRuntime(true);

    // These rebuilds only check connector types. Keep the original graph on failure.
    nodes_.swap(outsideNodes);
    ports_.swap(outsidePorts);
    links_.swap(outsideLinks);
    linkTriggers_.swap(outsideCounts);
    try {
        rebuildRuntime(true);
    } catch (...) {
        nodes_.swap(outsideNodes);
        ports_.swap(outsidePorts);
        links_.swap(outsideLinks);
        linkTriggers_.swap(outsideCounts);
        throw;
    }

    for (int id : selected) {
        if (auto module = modules_.extract(id)) {
            module.mapped()->reparent(*child, id);
            child->modules_.insert(std::move(module));
        }
        wirelessRoutes_.erase(id);
    }
    modules_.emplace(moduleId, std::move(child));
    for (auto& pulse : rootGraph().deferredTriggers_) {
        if (pulse.path.size() >= path_.size() &&
            std::ranges::equal(std::span{pulse.path}.first(path_.size()), path_)) {
            const int nodeOrModule = pulse.path.size() == path_.size() ? pulse.node : pulse.path[path_.size()];
            if (selected.contains(nodeOrModule)) {
                pulse.path.insert(pulse.path.begin() + path_.size(), moduleId);
            }
        }
    }
    if (parent_ && std::ranges::none_of(nodes_, [](const auto& entry) {
            return entry.kind == PrimitiveKind::ModulePropertiesGui;
        })) {
        parent_->node(path_.back()).propertiesVisibility = PropertiesVisibility::Closed;
    }
    ++nextId_;
    ++rootGraph().drawingTopology_;
    refreshConnections(false);
    return moduleId;
}

int Session::makeModule(std::span<const int> selection)
{
    stopSyncPainter();
    int result = -1;
    for (const auto& target : editTargets(selection)) {
        auto& graph = *target.graph;
        const int moduleId = graph.makeModule(target.ids);
        auto* child = graph.module(moduleId);

        // Move saved navigation paths with any existing modules in the selection.
        const auto relocate = [&](Path& path) {
            if (path.size() > graph.path_.size() &&
                std::ranges::equal(graph.path_, std::span{path}.first(graph.path_.size())) &&
                std::ranges::contains(target.ids, path[graph.path_.size()])) {
                path.insert(path.begin() + graph.path_.size(), moduleId);
                return true;
            }
            return false;
        };
        relocate(clipboardSource_);
        std::vector<std::pair<Path, Viewport>> moved;
        for (auto it = viewports_.begin(); it != viewports_.end();) {
            auto path = it->first;
            if (relocate(path)) {
                moved.emplace_back(std::move(path), it->second);
                it = viewports_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& [path, viewport] : moved) {
            viewports_.try_emplace(std::move(path), viewport);
        }
        const auto& nodes = child->nodes();
        const double left = std::ranges::min_element(nodes, {}, &SchematicNode::x)->x;
        const double right = std::ranges::max_element(nodes, {}, &SchematicNode::x)->x;
        const double bottom = std::ranges::max_element(nodes, {}, &SchematicNode::y)->y;
        viewports_.try_emplace(child->path_, Viewport{(left + right + 164) / 2, (bottom + 83) / 2});
        if (&graph == activeGraph_) {
            result = moduleId;
        }
    }
    return result;
}

}
