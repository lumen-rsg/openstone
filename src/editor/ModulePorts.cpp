#include "Schematic.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <tuple>

namespace openstone {
namespace {

auto& moduleConnectors(SchematicNode& module, bool input)
{
    return input ? module.moduleInputs : module.moduleOutputs;
}

}

const SchematicNode* Schematic::propertyBoundary(const SchematicNode& property) const
{
    if (property.label.empty()) return nullptr;
    const auto kind = property.kind == PrimitiveKind::ModuleInputProperty
        ? PrimitiveKind::ModuleOutput : PrimitiveKind::ModuleInput;
    const auto boundary = std::ranges::find_if(nodes_, [&](const auto& node) {
        return node.kind == kind && node.label == property.label;
    });
    return boundary == nodes_.end() ? nullptr : &*boundary;
}

const SchematicNode* Schematic::propertyWriter(const SchematicNode& input) const
{
    const auto writer = std::ranges::find_if(nodes_, [&](const auto& node) {
        return node.kind == PrimitiveKind::ModuleOutputProperty && node.label == input.label &&
               propertyBoundary(node) == &input;
    });
    return writer == nodes_.end() ? nullptr : &*writer;
}

void Schematic::setPropertyPortName(int id, std::string name)
{
    auto& endpoint = node(id);
    const auto previous = std::exchange(endpoint.label, std::move(name));
    try {
        rebuildConnectedRuntimes();
    } catch (...) {
        endpoint.label = previous;
        throw;
    }
}

std::string Schematic::nextModulePortName(bool input, int except) const
{
    const auto kind = input ? PrimitiveKind::ModuleInput : PrimitiveKind::ModuleOutput;
    for (int i = 1; ; ++i) {
        auto name = (input ? "Input " : "Output ") + std::to_string(i);
        if (std::ranges::none_of(nodes_, [&](const auto& endpoint) {
                return endpoint.kind == kind && endpoint.id != except && endpoint.label == name;
            })) {
            return name;
        }
    }
}

void Schematic::setModulePortName(int id, std::string name)
{
    auto& endpoint = node(id);
    if (!isModuleEndpoint(endpoint.kind)) {
        throw std::invalid_argument("Choose a Module Input or Module Output");
    }
    const bool input = endpoint.kind == PrimitiveKind::ModuleInput;
    if (name.empty()) {
        name = nextModulePortName(input, id);
    }
    auto& port = moduleConnectors(parent_->node(path_.back()), input).at(endpoint.boundaryIndex);
    const auto previous = std::exchange(port.name, name);
    const bool affectsProperty = std::ranges::any_of(nodes_, [&](const auto& node) {
        return isPropertyEndpoint(node.kind) && (node.label == previous || node.label == name);
    });
    try {
        if (affectsProperty) {
            setPropertyPortName(id, std::move(name));
        } else {
            endpoint.label = std::move(name);
        }
    } catch (...) {
        port.name = previous;
        throw;
    }
    if (affectsProperty) refreshConnections();
}

void Schematic::updateModulePorts(bool input, int removed)
{
    const int moduleId = path_.back();
    auto& parent = *parent_;
    auto& owner = parent.node(moduleId);
    auto& connectors = moduleConnectors(owner, input);
    auto& triggerCounts = input ? owner.inputTriggers : owner.outputTriggers;
    auto& types = parent.ports_.at(moduleId).types;
    auto& oldTypes = input ? types.inputs : types.outputs;
    const auto kind = input ? PrimitiveKind::ModuleInput : PrimitiveKind::ModuleOutput;

    // Stable node order breaks ties when two endpoints occupy the same position.
    std::vector<SchematicNode*> ordered;
    for (auto& entry : nodes_) {
        if (entry.kind == kind && entry.id != removed) {
            ordered.push_back(&entry);
        }
    }
    std::ranges::stable_sort(ordered, [](const auto* a, const auto* b) {
        return std::tie(a->y, a->x) < std::tie(b->y, b->x);
    });
    std::vector<ModuleConnector> newConnectors;
    std::vector<std::optional<green::Type>> newTypes;
    std::vector<std::uint64_t> newTriggers;
    std::vector<int> newIndex(connectors.size(), -1);
    for (const auto* endpoint : ordered) {
        const auto old = std::ranges::find(connectors, endpoint->id, &ModuleConnector::endpoint);
        std::uint64_t count = 0;
        if (old != connectors.end()) {
            const auto index = old - connectors.begin();
            newIndex[index] = static_cast<int>(newConnectors.size());
            count = triggerCounts[index];
        }
        newConnectors.push_back({endpoint->label, endpoint->boundaryType, endpoint->id});
        newTypes.push_back(endpoint->boundaryType);
        newTriggers.push_back(count);
    }
    if (newConnectors == connectors) {
        return;
    }

    std::vector<SchematicLink> links;
    links.reserve(parent.links_.size());
    std::map<SchematicLink, std::uint64_t> counts;
    for (auto link : parent.links_) {
        const auto original = link;
        if (input && link.target == moduleId) {
            link.input = newIndex.at(link.input);
        } else if (!input && link.source == moduleId) {
            link.output = newIndex.at(link.output);
        }
        if (link.input >= 0 && link.output >= 0) {
            links.push_back(link);
            counts.emplace(link, parent.linkTriggers_.at(original));
        }
    }

    const auto swapLayout = [&] {
        connectors.swap(newConnectors);
        oldTypes.swap(newTypes);
        triggerCounts.swap(newTriggers);
        parent.links_.swap(links);
        parent.linkTriggers_.swap(counts);
    };
    swapLayout();
    RuntimeBuild build;
    try {
        // The remaining wires have the same types and order of delivery.
        build = parent.buildRuntime(true);
    } catch (...) {
        swapLayout();
        throw;
    }
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        ordered[i]->boundaryIndex = static_cast<int>(i);
    }
    parent.installRuntime(std::move(build));
}

void Schematic::rebuildConnectedRuntimes(bool commit, bool forceWholeTree)
{
    using green::Type;
    struct Boundary {
        Schematic* child;
        SchematicNode* endpoint;
        ModuleConnector* port;
        std::optional<Type> previous;
    };
    std::vector<Schematic*> graphs{&rootGraph()};
    std::vector<Boundary> boundaries;
    std::size_t nodeCount = 0;
    bool hasBusExtract = false;
    bool hasPropertyRoutes = false;
    for (std::size_t i = 0; i < graphs.size(); ++i) {
        auto& graph = *graphs[i];
        nodeCount += graph.nodes_.size();
        for (auto& entry : graph.nodes_) {
            hasBusExtract |= entry.kind == PrimitiveKind::BusExtract;
            hasPropertyRoutes |= isPropertyEndpoint(entry.kind) && graph.propertyBoundary(entry);
            if (isModuleEndpoint(entry.kind)) {
                auto& owner = graph.parent_->node(graph.path_.back());
                auto& port = moduleConnectors(owner, entry.kind == PrimitiveKind::ModuleInput)
                    .at(entry.boundaryIndex);
                boundaries.push_back({&graph, &entry, &port, entry.boundaryType});
            }
        }
        for (auto& [id, child] : graph.modules_) {
            graphs.push_back(child.get());
        }
    }
    if (hasPropertyRoutes) {
        for (const auto* graph : graphs) {
            graph->validatePropertyRoutes();
        }
    }
    RuntimeBuilds builds;
    if (!forceWholeTree && std::ranges::none_of(boundaries, [](const auto& boundary) {
            const auto declared = boundary.endpoint->typeOverride;
            return !declared || green::isGenericDspStream(*declared);
        })) {
        auto& build = builds[this];
        build.ports = resolvePorts();
        resolveDspStreams(builds);
        wireRuntime(build);
        if (commit) {
            installRuntime(std::move(build));
        }
        return;
    }

    for (auto* graph : graphs) {
        builds.try_emplace(graph);
    }
    const auto restore = [&] {
        for (const auto& boundary : boundaries) {
            boundary.port->type = boundary.endpoint->boundaryType = boundary.previous;
        }
    };

    const auto infer = [&](bool upstream) {
        // Collect a round before applying it, so sibling traversal order cannot choose the type.
        std::vector<std::pair<const Boundary*, Type>> resolved;
        for (const auto& boundary : boundaries) {
            const auto& endpoint = *boundary.endpoint;
            if (endpoint.typeOverride || (!upstream && endpoint.boundaryType)) {
                continue;
            }
            const bool input = endpoint.kind == PrimitiveKind::ModuleInput;
            const bool outside = upstream == input;
            const auto* graph = outside ? boundary.child->parent_ : boundary.child;
            const int id = outside ? boundary.child->path_.back() : endpoint.id;
            const int index = outside ? endpoint.boundaryIndex : 0;
            const auto& ports = builds.at(graph).ports;
            std::optional<Type> candidate;
            for (auto link : graph->links_) {
                auto type = std::optional<Type>{};
                if (upstream && link.target == id && link.input == index) {
                    type = ports.at(link.source).types.outputs.at(link.output);
                } else if (!upstream && link.source == id && link.output == index) {
                    type = ports.at(link.target).types.inputs.at(link.input);
                }
                if (type) {
                    candidate = type;
                    if (*candidate != Type::Trigger) {
                        break;
                    }
                }
            }
            if (hasPropertyRoutes && !candidate) {
                const auto& child = *boundary.child;
                const auto& local = builds.at(&child).ports;
                if (input && upstream) {
                    if (const auto* writer = child.propertyWriter(endpoint)) {
                        candidate = local.at(writer->id).types.inputs.front();
                    }
                } else if (!input && !upstream) {
                    for (const auto& property : child.nodes_) {
                        if (property.kind == PrimitiveKind::ModuleInputProperty &&
                            child.propertyBoundary(property) == &endpoint) {
                            candidate = local.at(property.id).types.outputs.front();
                            if (candidate) break;
                        }
                    }
                }
            }
            if (candidate && candidate != endpoint.boundaryType) {
                resolved.emplace_back(&boundary, *candidate);
            }
        }
        for (const auto& [boundary, type] : resolved) {
            boundary->port->type = boundary->endpoint->boundaryType = type;
        }
        return !resolved.empty();
    };

    for (const auto& boundary : boundaries) {
        boundary.port->type = boundary.endpoint->boundaryType = boundary.endpoint->typeOverride;
    }
    try {
        for (std::size_t round = 0; ; ++round) {
            // Bus channels can depend on types computed in a different schematic.
            for (std::size_t pass = 0; ; ++pass) {
                bool changed = false;
                for (const auto* graph : graphs) {
                    auto& ports = builds.at(graph).ports;
                    auto resolved = graph->resolvePorts(&builds);
                    if (hasBusExtract) {
                        changed |= !std::ranges::equal(resolved, ports, [](const auto& a, const auto& b) {
                            return a.first == b.first && a.second.types == b.second.types;
                        });
                    }
                    ports = std::move(resolved);
                }
                if (!changed) {
                    break;
                }
                if (pass >= nodeCount) {
                    throw std::logic_error("Bus connector types did not settle");
                }
            }
            // A source can become more specific after a type has crossed another boundary.
            if (!infer(true) && !infer(false)) {
                break;
            }
            if (round >= nodeCount * 2) {
                throw std::invalid_argument("Module connector types did not settle");
            }
        }

        resolveDspStreams(builds);
        for (auto* graph : graphs) {
            graph->wireRuntime(builds.at(graph));
        }
    } catch (...) {
        restore();
        throw;
    }
    if (!commit) {
        restore();
        return;
    }
    for (const auto& boundary : boundaries) {
        const auto& endpoint = *boundary.endpoint;
        const auto& ports = builds.at(boundary.child).ports.at(endpoint.id).types;
        const auto type = endpoint.kind == PrimitiveKind::ModuleInput ? ports.outputs.front() : ports.inputs.front();
        boundary.port->type = boundary.endpoint->boundaryType = type;
    }
    for (auto* graph : graphs) {
        graph->installRuntime(std::move(builds.at(graph)));
    }
}

void Schematic::setModulePortType(int id, std::optional<green::Type> type)
{
    const std::pair target{this, id};
    setModulePortTypes(std::span{&target, 1}, type);
}

void Schematic::setModulePortTypes(std::span<const std::pair<Schematic*, int>> targets,
                                   std::optional<green::Type> type)
{
    if (type && !supportsTemplateType(PrimitiveKind::ModuleInput, *type)) {
        throw std::invalid_argument("Choose a connector type");
    }
    std::vector<std::pair<SchematicNode*, std::optional<green::Type>>> changes;
    for (const auto& [child, id] : targets) {
        auto& endpoint = child->node(id);
        if (!isModuleEndpoint(endpoint.kind)) {
            throw std::invalid_argument("Choose a Module Input or Module Output");
        }
        if (endpoint.typeOverride != type) {
            changes.emplace_back(&endpoint, endpoint.typeOverride);
        }
    }
    if (changes.empty()) {
        return;
    }

    // Validate every copy and both ends of the boundary before replacing any runtime.
    for (auto& [endpoint, previous] : changes) {
        endpoint->typeOverride = type;
    }
    const auto restore = [&] {
        for (auto& [endpoint, previous] : changes) {
            endpoint->typeOverride = previous;
        }
    };
    try {
        targets.front().first->rebuildConnectedRuntimes(true, true);
    } catch (const std::invalid_argument& error) {
        restore();
        throw std::invalid_argument(std::string("Existing links prevent this type change: ") + error.what());
    } catch (...) {
        restore();
        throw;
    }
    targets.front().first->refreshConnections(false);
}

}
