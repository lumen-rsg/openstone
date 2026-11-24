#include "Schematic.hpp"

#include <algorithm>
#include <stdexcept>

namespace openstone {
namespace {

using green::Type;
using green::Value;

bool isReceiver(PrimitiveKind kind)
{
    return kind == PrimitiveKind::WirelessInput || kind == PrimitiveKind::WirelessReceiver;
}

bool isSender(PrimitiveKind kind)
{
    return kind == PrimitiveKind::WirelessOutput || kind == PrimitiveKind::WirelessLink || isModuleWirelessOutput(kind);
}

}

void Schematic::setWirelessLabel(int id, std::string label)
{
    auto& entry = node(id);
    if (!usesWirelessLabel(entry.kind)) {
        throw std::invalid_argument("Choose a Wireless Input or Wireless Output");
    }
    if (entry.wirelessLabel != label) {
        entry.wirelessLabel = std::move(label);
        refreshConnections();
    }
}

std::string Schematic::wirelessName(int id) const
{
    const auto& entry = node(id);
    if (usesWirelessLabel(entry.kind)) {
        return entry.wirelessLabel;
    }
    if (entry.kind == PrimitiveKind::WirelessLink || entry.kind == PrimitiveKind::ModuleWirelessLink) {
        return std::get<std::string>(readInput(id, 1));
    }
    if (entry.kind == PrimitiveKind::WirelessReceiver) {
        return std::get<std::string>(readInput(id, 0));
    }
    return {};
}

Schematic::WirelessSources Schematic::wirelessSources(int receiver) const
{
    const auto type = outputType(receiver);
    if (!type || !parent_) {
        return {};
    }
    const auto name = wirelessName(receiver);
    if (name.empty()) {
        return {};
    }

    // A matching publisher in the nearest ancestor shadows farther publishers.
    const bool singleSource = green::hasSingleValueSource(*type);
    for (auto* ancestor = parent_; ancestor; ancestor = ancestor->parent_) {
        WirelessSources sources;
        const auto consider = [&](const Schematic& graph, const SchematicNode& sender) {
            if (graph.inputType(sender.id, 0) != type || graph.wirelessName(sender.id) != name) return false;
            sources.push_back({&graph, sender.id});
            return singleSource;
        };
        for (const auto& sender : ancestor->nodes_) {
            if (isSender(sender.kind) && !isModuleWirelessOutput(sender.kind) && consider(*ancestor, sender)) break;
            if (const auto* module = ancestor->module(sender.id)) {
                const auto matches = [&](const SchematicNode& nested) {
                    return isModuleWirelessOutput(nested.kind) && consider(*module, nested);
                };
                if (std::ranges::any_of(module->nodes_, matches)) break;
            }
        }
        if (!sources.empty()) {
            return sources;
        }
    }
    return {};
}

bool Schematic::wirelessConnected(int id) const
{
    const auto kind = node(id).kind;
    if (isReceiver(kind)) {
        const auto route = wirelessRoutes_.find(id);
        return route != wirelessRoutes_.end() && !route->second.empty();
    }
    if (!isSender(kind)) {
        return false;
    }

    const auto foundReceiver = [&](const auto& visit, const Schematic& graph) -> bool {
        for (const auto& [moduleId, child] : graph.modules_) {
            const auto matches = [this, id](const auto& route) {
                return std::ranges::contains(route.second, WirelessSource{this, id});
            };
            if (std::ranges::any_of(child->wirelessRoutes_, matches) || visit(visit, *child)) {
                return true;
            }
        }
        return false;
    };
    const auto* scope = isModuleWirelessOutput(kind) ? parent_ : this;
    return scope && foundReceiver(foundReceiver, *scope);
}

std::optional<Value> Schematic::readWireless(int id, Type type) const
{
    std::optional<Value> value;
    for (const auto& source : wirelessSources(id)) {
        auto input = source.graph->readInput(source.id, 0);
        if (auto* view = std::get_if<yellow::View>(&input); view && view->panel != 0) {
            // Several receivers can branch from the same sender.
            view->drawOrder.push_back(path_.size());
            view->drawOrder.insert(view->drawOrder.end(), path_.begin(), path_.end());
            view->drawOrder.push_back(static_cast<std::size_t>(id));
        }
        if (value) {
            green::combineInputValue(*value, input);
        } else {
            value = std::move(input);
        }
    }
    if (value && type == Type::Float) {
        *value = green::normalizeFloat(std::get<float>(*value));
    }
    return value;
}

void Schematic::publishWireless(WirelessSource source)
{
    for (auto& [moduleId, child] : modules_) {
        for (auto& entry : child->nodes_) {
            if (!isReceiver(entry.kind)) {
                continue;
            }
            auto sources = child->wirelessSources(entry.id);
            if (std::ranges::contains(sources, source)) {
                child->wirelessRoutes_.insert_or_assign(entry.id, std::move(sources));
                child->fireOutput(entry, 0);
            }
        }
        child->publishWireless(source);
    }
}

void Schematic::refreshConnections(bool notify, std::optional<int> newReceiverOutput)
{
    auto* root = &rootGraph();
    // Boundaries can carry Bus names and types in either direction.
    for (std::size_t pass = 0; root->refreshBusTree(); ++pass) {
        if (pass == 128) {
            throw std::logic_error("Bus routes did not settle");
        }
    }
    const auto skip = newReceiverOutput ? std::optional{WirelessSource{this, *newReceiverOutput}} : std::nullopt;
    root->refreshWirelessRoutes(notify, skip);
    root->refreshMatrices(notify);
    root->refreshCodeSources();
    if (notify) root->refreshPluginLatency();
    else {
        root->primeStateMachines();
        root->primeEmbeddedFonts();
        root->primeGreenLogic();
    }
}

void Schematic::refreshWirelessRoutes(bool notify, std::optional<WirelessSource> newReceiverOutput)
{
    // A value delivered on rebind can change another receiver's Name in this schematic.
    for (std::size_t pass = 0; ; ++pass) {
        const auto busRevision = busLayoutRevision_;
        bool changed = false;
        bool hasReceiver = false;
        for (auto& entry : nodes_) {
            if (!isReceiver(entry.kind)) {
                continue;
            }
            hasReceiver = true;
            auto sources = wirelessSources(entry.id);
            auto& previous = wirelessRoutes_[entry.id];
            if (previous == sources) {
                continue;
            }
            changed = true;
            previous = std::move(sources);
            const auto type = outputType(entry.id);
            if (type == Type::View) {
                ++rootGraph().drawingTopology_;
            }
            // Changing addresses updates values without manufacturing a Trigger signal.
            if (notify && type && *type != Type::Trigger && !green::deliversDiscreteEvents(*type) &&
                newReceiverOutput != WirelessSource{this, entry.id}) {
                fireOutput(entry, 0);
            }
        }
        if (hasReceiver) {
            refreshBusPorts();
        }
        if (!changed && busRevision == busLayoutRevision_) {
            break;
        }
        if (pass > nodes_.size()) {
            throw std::logic_error("Wireless routes did not settle");
        }
    }
    for (auto& [moduleId, child] : modules_) {
        child->refreshWirelessRoutes(notify, newReceiverOutput);
    }
}

}
