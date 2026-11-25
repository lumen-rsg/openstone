#include "Schematic.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <stdexcept>
#include <tuple>

namespace openstone {
namespace {

std::vector<std::string> parseChannelNames(const std::string& text)
{
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    std::vector<std::string> names;
    std::set<std::string_view> seen;
    for (auto part : text | std::views::split(',')) {
        std::string_view name(part.begin(), part.end());
        const auto first = name.find_first_not_of(whitespace);
        if (first == std::string_view::npos) {
            continue;
        }
        name = name.substr(first, name.find_last_not_of(whitespace) - first + 1);
        if (seen.insert(name).second) {
            names.emplace_back(name);
        }
    }
    return names;
}

struct ChannelUpdate {
    bool create;
    std::vector<std::string> names;
    std::vector<std::uint64_t> triggers;
    std::vector<int> newIndex;
};

}

std::optional<int> SchematicNode::codeSourcePort(bool output) const
{
    if (!codeSourcePorts) return {};
    if (dspCode) return output ? std::optional{static_cast<int>(dspCode->program.outputs().size())} : std::nullopt;
    if (rubyCode) return static_cast<int>(output ? rubyCode->ports.outputs.size() : rubyCode->ports.inputs.size());
    return {};
}

std::vector<Port> SchematicNode::inputPorts() const
{
    if (rubyCode) {
        std::vector<Port> result;
        for (const auto& port : rubyCode->ports.inputs) result.push_back({port.name, port.type});
        if (codeSourcePorts) result.push_back({"Source", green::Type::String});
        return result;
    }
    auto inputs = primitive(kind).inputs;
    if (kind == PrimitiveKind::PresetFloat && presetParameter && presetParameter->flowStoneFloat)
        inputs[9].type = green::Type::Float;
    if (hasExpandableInputs(kind) && inputTriggers.size() > inputs.size()) {
        const Port connected{kind == PrimitiveKind::Indexer ? "Trigger" : "Value", inputs.back().type};
        inputs.insert(inputs.end() - 1, inputTriggers.size() - inputs.size(), connected);
    }
    if (dspCode) {
        for (const auto& port : dspCode->program.inputs()) {
            inputs.push_back({port.name, dspConnectorType(port.type)});
        }
    }
    if (kind == PrimitiveKind::AsioOut && !channelNames.empty()) {
        inputs.clear();
        for (const auto& name : channelNames) inputs.push_back({name, green::Type::Mono});
    }
    if (kind == PrimitiveKind::BusCreate) {
        for (const auto& name : channelNames) {
            inputs.push_back({name, {}});
        }
    }
    if (kind == PrimitiveKind::Module) {
        for (const auto& port : moduleInputs) {
            inputs.push_back({port.name, port.type});
        }
    }
    if (kind == PrimitiveKind::ModuleOutput) {
        inputs.front() = {label, boundaryType};
    }
    if (codeSourcePort(false)) inputs.push_back({"Source", green::Type::String});
    return inputs;
}

std::vector<Port> SchematicNode::outputPorts() const
{
    if (rubyCode) {
        std::vector<Port> result;
        for (const auto& port : rubyCode->ports.outputs) result.push_back({port.name, port.type});
        if (codeSourcePorts) result.push_back({"Source", green::Type::String});
        return result;
    }
    auto outputs = primitive(kind).outputs;
    if (hasExpandableOutputs(kind) && outputTriggers.size() > outputs.size()) {
        const Port connected{"Value", outputs.front().type};
        outputs.insert(outputs.begin(), outputTriggers.size() - outputs.size(), connected);
    }
    if (dspCode) {
        for (const auto& port : dspCode->program.outputs()) {
            outputs.push_back({port.name, dspConnectorType(port.type)});
        }
    }
    if (kind == PrimitiveKind::AsioIn && !channelNames.empty()) {
        outputs.clear();
        for (const auto& name : channelNames) outputs.push_back({name, green::Type::Mono});
    }
    if (kind == PrimitiveKind::BusExtract) {
        for (const auto& name : channelNames) {
            outputs.push_back({name, {}});
        }
    }
    if (kind == PrimitiveKind::Module) {
        for (const auto& port : moduleOutputs) {
            outputs.push_back({port.name, port.type});
        }
    }
    if (kind == PrimitiveKind::ModuleInput) {
        outputs.front() = {label, boundaryType};
    }
    if (kind == PrimitiveKind::MidiCCStream && typeOverride == green::Type::Mono) {
        outputs.front().type = green::Type::Mono;
    }
    if (codeSourcePort(true)) outputs.push_back({"Source", green::Type::String});
    return outputs;
}

void Schematic::refreshBusPorts()
{
    // Names can come from an upstream Extract. Re-evaluate after each rebuild to let
    // changes reach dependent nodes. The graph has no cycles.
    for (std::size_t pass = 0; pass <= nodes_.size(); ++pass) {
        std::map<int, ChannelUpdate> updates;
        for (const auto& entry : nodes_) {
            const bool create = entry.kind == PrimitiveKind::BusCreate;
            if (!create && entry.kind != PrimitiveKind::BusExtract) {
                continue;
            }
            auto names = parseChannelNames(std::get<std::string>(readInput(entry.id, 0)));
            if (names == entry.channelNames) {
                continue;
            }

            const auto& counts = create ? entry.inputTriggers : entry.outputTriggers;
            const int offset = create ? 1 : 0;
            ChannelUpdate update{create, std::move(names), {}, std::vector<int>(counts.size(), -1)};
            update.triggers.resize(offset + update.names.size());
            if (create) {
                update.newIndex[0] = 0;
                update.triggers[0] = counts[0];
            }
            for (std::size_t i = 0; i < entry.channelNames.size(); ++i) {
                const auto match = std::ranges::find(update.names, entry.channelNames[i]);
                if (match != update.names.end()) {
                    const int index = offset + static_cast<int>(match - update.names.begin());
                    update.newIndex[i + offset] = index;
                    update.triggers[index] = counts[i + offset];
                }
            }
            updates.emplace(entry.id, std::move(update));
        }
        if (updates.empty()) {
            for (const auto& entry : nodes_) {
                if (entry.kind != PrimitiveKind::BusExtract) {
                    continue;
                }
                const auto source = findBusSource(entry.id);
                const auto& outputs = ports_.at(entry.id).types.outputs;
                for (std::size_t i = 0; i < entry.channelNames.size(); ++i) {
                    const auto type = source ? busChannelType(*source, entry.channelNames[i], ports_) : std::nullopt;
                    if (outputs[i] != type) {
                        try {
                            rebuildConnectedRuntimes();
                            ++busLayoutRevision_;
                        } catch (const std::invalid_argument&) {
                            // Keep the existing type if a remaining link still depends on it.
                        }
                        return;
                    }
                }
            }
            return;
        }

        std::vector<SchematicLink> links;
        links.reserve(links_.size());
        std::map<SchematicLink, std::uint64_t> counts;
        for (auto link : links_) {
            const auto original = link;
            if (const auto it = updates.find(link.source); it != updates.end() && !it->second.create) {
                link.output = it->second.newIndex.at(link.output);
            }
            if (const auto it = updates.find(link.target); it != updates.end() && it->second.create) {
                link.input = it->second.newIndex.at(link.input);
            }
            if (link.output >= 0 && link.input >= 0) {
                links.push_back(link);
                counts.emplace(link, linkTriggers_.at(original));
            }
        }

        const auto swapLayout = [&] {
            for (auto& [id, update] : updates) {
                auto& entry = node(id);
                entry.channelNames.swap(update.names);
                auto& triggers = update.create ? entry.inputTriggers : entry.outputTriggers;
                triggers.swap(update.triggers);
            }
            links_.swap(links);
            linkTriggers_.swap(counts);
        };
        swapLayout();
        try {
            rebuildConnectedRuntimes();
        } catch (...) {
            swapLayout();
            throw;
        }
        ++busLayoutRevision_;
    }
    throw std::logic_error("Bus channel names did not settle");
}

bool Schematic::refreshBusTree()
{
    const auto revision = busLayoutRevision_;
    refreshBusPorts();
    bool changed = revision != busLayoutRevision_;
    for (auto& [id, child] : modules_) {
        changed = child->refreshBusTree() || changed;
    }
    return changed;
}

Schematic& Schematic::rootGraph()
{
    return const_cast<Schematic&>(std::as_const(*this).rootGraph());
}

const Schematic& Schematic::rootGraph() const
{
    auto* graph = this;
    while (graph->parent_) {
        graph = graph->parent_;
    }
    return *graph;
}

std::optional<Schematic::BusSource> Schematic::findBusSource(int extractor) const
{
    const auto* graph = this;
    int id = extractor;
    int input = 1;
    std::set<std::tuple<const Schematic*, int, int>> visited;
    while (visited.emplace(graph, id, input).second) {
        const auto wire = std::ranges::find_if(graph->links_, [=](SchematicLink link) {
            return link.target == id && link.input == input;
        });
        if (wire == graph->links_.end()) {
            break;
        }
        const auto& source = graph->node(wire->source);
        if (source.kind == PrimitiveKind::BusCreate || source.kind == PrimitiveKind::SfzParser) {
            return BusSource{graph, source.id};
        }
        if (source.kind == PrimitiveKind::ModuleInput) {
            id = graph->path_.back();
            input = source.boundaryIndex;
            graph = graph->parent_;
        } else if (isIndexedRouter(source.kind)) {
            // During a rebuild there may not be a Green runtime to read the index from yet.
            const auto index = graph->ports_.at(source.id).inputs.empty() ? 0 : graph->routerSelection(source);
            const auto selected = routedDataInput(source.kind, index, wire->output);
            if (!selected) return std::nullopt;
            id = source.id;
            input = *selected;
        } else if (source.kind == PrimitiveKind::Module) {
            id = source.moduleOutputs.at(wire->output).endpoint;
            input = 0;
            graph = graph->module(source.id);
            if (!graph) {
                break;
            }
        } else {
            break;
        }
    }
    return {};
}

int Schematic::busChannelInput(const BusSource& source, std::string_view name) const
{
    const auto& entry = source.graph->node(source.id);
    if (entry.kind == PrimitiveKind::SfzParser) {
        const auto columns = sfzChannels();
        const auto match = std::ranges::find(columns, name, &green::BusChannel::name);
        return match == columns.end() ? -1 : 1 + static_cast<int>(match - columns.begin());
    }
    const auto& names = entry.channelNames;
    const auto match = std::ranges::find(names, name);
    return match == names.end() ? -1 : 1 + static_cast<int>(match - names.begin());
}

std::optional<green::Type> Schematic::busChannelType(const BusSource& source, std::string_view name,
                                                     const std::map<int, NodePorts>& building, bool staged) const
{
    const int input = busChannelInput(source, name);
    if (input < 0) {
        return {};
    }
    if (source.graph->node(source.id).kind == PrimitiveKind::SfzParser) {
        return sfzChannels()[input - 1].type;
    }
    if (source.graph == this || staged) {
        if (const auto found = building.find(source.id); found != building.end() &&
            static_cast<std::size_t>(input) < found->second.types.inputs.size()) {
            return found->second.types.inputs.at(input);
        }
        if (staged) {
            return {};
        }
    }
    const auto found = source.graph->ports_.find(source.id);
    if (found != source.graph->ports_.end() &&
        static_cast<std::size_t>(input) < found->second.types.inputs.size()) {
        return found->second.types.inputs[input];
    }
    return {};
}

void Schematic::fireBusChannel(SchematicNode& entry, int input)
{
    auto& pulse = rootGraph().busPulse_;
    const auto previous = std::exchange(pulse, BusPulse{{this, entry.id}, input});
    try {
        fireOutput(entry, 0);
    } catch (...) {
        pulse = previous;
        throw;
    }
    pulse = previous;
}

void Schematic::receiveBusPulse(SchematicNode& entry)
{
    const auto& types = ports_.at(entry.id).types;
    const auto source = findBusSource(entry.id);
    const auto& pulse = rootGraph().busPulse_;
    if (pulse && source && source->graph == pulse->source.graph && source->id == pulse->source.id) {
        for (std::size_t i = 0; i < entry.channelNames.size(); ++i) {
            if (types.outputs[i] && busChannelInput(*source, entry.channelNames[i]) == pulse->input) {
                fireOutput(entry, i);
                break;
            }
        }
        return;
    }
    // Establishing a Bus link initializes connected data. Trigger channels wait for a pulse.
    for (std::size_t i = 0; i < types.outputs.size(); ++i) {
        if (types.outputs[i] && types.outputs[i] != green::Type::Trigger) {
            fireOutput(entry, i);
        }
    }
}

}
