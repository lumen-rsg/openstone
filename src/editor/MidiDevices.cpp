#include "Schematic.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace openstone {
namespace {

using enum PrimitiveKind;

MidiDirection direction(PrimitiveKind kind)
{
    return kind == MidiIn || kind == MidiInDevices || kind == MidiInSelect
        ? MidiDirection::Input : MidiDirection::Output;
}

bool isListing(PrimitiveKind kind)
{
    return kind == MidiInDevices || kind == MidiOutDevices;
}

bool isIndexSelector(PrimitiveKind kind)
{
    return kind == MidiInSelect || kind == MidiOutSelect;
}

std::size_t directionIndex(PrimitiveKind kind)
{
    return std::to_underlying(direction(kind));
}

int deviceIndexInput(PrimitiveKind kind) { return kind == MidiInSelect ? 0 : 1; }
int selectedOutput(PrimitiveKind kind) { return kind == MidiInSelect ? 1 : 0; }

struct RefreshGuard {
    explicit RefreshGuard(bool& flag) : flag(flag) { flag = true; }
    ~RefreshGuard() { flag = false; }

    bool& flag;
};

}

void Schematic::setMidiDeviceBackend(std::shared_ptr<MidiDeviceBackend> backend)
{
    auto& state = rootGraph().midiDevices_;
    state.lists = {};
    state.backend = std::move(backend);
    state.scanned = false;
}

bool Schematic::watchesMidiDevices() const
{
    return rootGraph().midiDevices_.watching;
}

bool Schematic::listensToMidi() const
{
    return !rootGraph().midiDevices_.lists[std::to_underlying(MidiDirection::Input)].leases.empty();
}

std::vector<MidiDeviceChoice> Schematic::midiDeviceChoices(int id) const
{
    const auto& entry = node(id);
    if (entry.kind != MidiIn && entry.kind != MidiOut) return {};
    const auto& list = rootGraph().midiDevices_.lists[directionIndex(entry.kind)];
    const MidiDeviceOwner owner{path_, id};
    std::vector<MidiDeviceChoice> result;
    for (const auto& port : list.ports) {
        const bool chosen = entry.midiSelection && std::ranges::contains(entry.midiSelection->ports, port.id);
        const auto lease = list.leases.find(port.id);
        const bool connected = lease != list.leases.end() && lease->second.owner == owner;
        result.push_back({port.id, port.name, chosen, lease == list.leases.end() || connected, connected});
    }
    if (entry.midiSelection) {
        for (const auto& port : entry.midiSelection->ports) {
            if (!std::ranges::contains(list.ports, port, &MidiDeviceInfo::id)) {
                result.push_back({port, "Missing device", true, false, false});
            }
        }
    }
    return result;
}

void Schematic::setMidiDeviceSelected(int id, std::string port, bool selected)
{
    auto& entry = node(id);
    if (entry.kind != MidiIn && entry.kind != MidiOut) {
        throw std::invalid_argument("Choose a MIDI In or MIDI Out node");
    }
    if (!entry.midiSelection) entry.midiSelection.emplace();
    auto& ports = entry.midiSelection->ports;
    if (!selected) {
        std::erase(ports, port);
    } else if (!std::ranges::contains(ports, port)) {
        const auto choices = midiDeviceChoices(id);
        const auto choice = std::ranges::find(choices, port, &MidiDeviceChoice::id);
        if (choice == choices.end() || !choice->available) throw std::invalid_argument("MIDI device is unavailable");
        ports.push_back(std::move(port));
    }
}

bool Schematic::refreshMidiDevices(bool scan, bool notify)
{
    auto& root = rootGraph();
    auto& state = root.midiDevices_;
    if (state.refreshing) {
        state.refreshRequested = true;
        return false;
    }
    const RefreshGuard guard(state.refreshing);
    struct Endpoint {
        Schematic* graph;
        SchematicNode* node;
        MidiDeviceOwner owner;
    };

    bool changed = false;
    for (int pass = 0; pass < 16; ++pass) {
        state.refreshRequested = false;
        std::vector<Endpoint> endpoints;
        const auto visit = [&](auto&& self, Schematic& level) -> void {
            for (auto& entry : level.nodes_) {
                if (primitive(entry.kind).behavior == PrimitiveBehavior::MidiDevice) {
                    endpoints.push_back({&level, &entry, {level.path_, entry.id}});
                }
            }
            for (auto& [id, child] : level.modules_) self(self, *child);
        };
        visit(visit, root);
        const bool watching = !endpoints.empty();
        changed |= state.watching != watching;
        state.watching = watching;
        if (!watching) {
            state.lists = {};
            state.scanned = false;
            return changed;
        }

        const auto available = [](const MidiDeviceList& list, std::string_view port) {
            return !list.leases.contains(port) && !list.unavailable.contains(port);
        };
        const auto availability = [&](const MidiDeviceList& list) {
            green::IntArray result;
            for (const auto& port : list.ports) result.push_back(available(list, port.id));
            return result;
        };
        auto& lists = state.lists;
        const std::array previous{lists[0].ports, lists[1].ports};
        const std::array previousAvailability{availability(lists[0]), availability(lists[1])};
        if (state.backend && (scan || !state.scanned)) {
            auto inputs = state.backend->list(MidiDirection::Input);
            auto outputs = state.backend->list(MidiDirection::Output);
            lists[0].ports = std::move(inputs);
            lists[1].ports = std::move(outputs);
            state.scanned = true;
        }
        scan = false;

        // A numeric selection binds to a port identity until its input changes.
        for (auto& endpoint : endpoints) {
            auto& entry = *endpoint.node;
            if (!isIndexSelector(entry.kind)) continue;
            const auto requested = std::get<std::int32_t>(
                endpoint.graph->readInput(entry.id, deviceIndexInput(entry.kind)));
            if (!entry.midiSelection) entry.midiSelection.emplace();
            auto& selection = *entry.midiSelection;
            if (requested != selection.requestedIndex) {
                selection.requestedIndex = requested;
                selection.ports.clear();
                selection.reportedUnavailable = false;
            }
            const auto& ports = lists[directionIndex(entry.kind)].ports;
            if (selection.ports.empty() && requested >= 0 && requested < std::ssize(ports)) {
                selection.ports.push_back(ports[requested].id);
            }
        }

        for (auto& list : lists) {
            const auto removed = std::erase_if(list.leases, [&](const auto& item) {
                const auto& [port, lease] = item;
                const auto endpoint = std::ranges::find(endpoints, lease.owner, &Endpoint::owner);
                return endpoint == endpoints.end() || !endpoint->node->midiSelection ||
                       !std::ranges::contains(endpoint->node->midiSelection->ports, port) ||
                       !std::ranges::contains(list.ports, port, &MidiDeviceInfo::id);
            });
            changed |= removed != 0;
            list.unavailable.clear();
        }
        for (const auto& endpoint : endpoints) {
            const auto& entry = *endpoint.node;
            if (!entry.midiSelection) continue;
            const auto side = direction(entry.kind);
            auto& list = lists[directionIndex(entry.kind)];
            for (const auto& id : entry.midiSelection->ports) {
                if (!state.backend || list.leases.contains(id) || list.unavailable.contains(id) ||
                    !std::ranges::contains(list.ports, id, &MidiDeviceInfo::id)) continue;
                MidiDeviceLease lease{endpoint.owner, {}, {}};
                try {
                    if (side == MidiDirection::Input) lease.input = state.backend->openInput(id);
                    else lease.output = state.backend->openOutput(id);
                } catch (const std::runtime_error&) {
                    // Keep the selection so a busy or temporarily disconnected device can be retried.
                }
                if (lease.input || lease.output) {
                    list.leases.emplace(id, std::move(lease));
                    changed = true;
                } else {
                    list.unavailable.insert(id);
                }
            }
        }
        const std::array listingsChanged{
            lists[0].ports != previous[0] || availability(lists[0]) != previousAvailability[0],
            lists[1].ports != previous[1] || availability(lists[1]) != previousAvailability[1],
        };
        changed |= listingsChanged[0] || listingsChanged[1];

        struct Notification { Endpoint* endpoint; int output; };
        std::vector<Notification> notifications;
        for (auto& endpoint : endpoints) {
            auto& entry = *endpoint.node;
            if (isListing(entry.kind) && listingsChanged[directionIndex(entry.kind)]) {
                notifications.push_back({&endpoint, -1});
            } else if (isIndexSelector(entry.kind)) {
                auto& selection = *entry.midiSelection;
                const auto& list = lists[directionIndex(entry.kind)];
                int index = -1;
                if (!selection.ports.empty()) {
                    const auto& port = selection.ports.front();
                    const auto lease = list.leases.find(port);
                    if (lease != list.leases.end() && lease->second.owner == endpoint.owner) {
                        index = static_cast<int>(std::ranges::find(list.ports, port, &MidiDeviceInfo::id) - list.ports.begin());
                    }
                }
                const bool unavailable = index < 0 && selection.requestedIndex.value_or(-1) >= 0;
                const int selected = selectedOutput(entry.kind);
                if (std::exchange(selection.reportedIndex, index) != index) notifications.push_back({&endpoint, selected});
                if (unavailable && !selection.reportedUnavailable) notifications.push_back({&endpoint, selected + 1});
                selection.reportedUnavailable = unavailable;
            }
        }

        if (notify) {
            for (const auto& [endpoint, output] : notifications) {
                if (output >= 0) endpoint->graph->fireOutput(*endpoint->node, output);
                else endpoint->graph->fireOutputs(endpoint->node->id);
            }
            changed |= !notifications.empty();
        }
        if (!state.refreshRequested) return changed;
    }
    throw std::runtime_error("MIDI device selections did not settle");
}

bool Schematic::pollMidiDevices()
{
    auto& root = rootGraph();
    if (root.midiDevices_.refreshing) return false;
    auto& leases = root.midiDevices_.lists[std::to_underlying(MidiDirection::Input)].leases;
    std::vector<std::string> ports;
    for (const auto& [id, lease] : leases) ports.push_back(id);

    bool delivered = false;
    for (const auto& id : ports) {
        // Keep a busy device from monopolizing the editor event loop.
        for (int count = 0; count < 64; ++count) {
            const auto lease = leases.find(id);
            if (lease == leases.end()) break;
            auto message = lease->second.input->receive();
            if (!message) break;
            if (message->bytes.empty()) continue;

            auto* level = &root;
            const auto& [path, nodeId] = lease->second.owner;
            for (const auto moduleId : path) {
                level = level->module(moduleId);
                if (!level) break;
            }
            if (!level) break;
            const auto entry = std::ranges::find(level->nodes_, nodeId, &SchematicNode::id);
            if (entry == level->nodes_.end() || !entry->midiSelection ||
                !std::ranges::contains(entry->midiSelection->ports, id)) break;
            if (!entry->midi) entry->midi.emplace();
            level->publishMidi(*entry, std::move(*message));
            delivered = true;
        }
    }
    if (delivered) root.refreshConnections();
    return delivered;
}

green::Value Schematic::readMidiDevice(const SchematicNode& entry, int output) const
{
    const auto& list = rootGraph().midiDevices_.lists[directionIndex(entry.kind)];
    if (isListing(entry.kind)) {
        if (output == 0) return static_cast<std::int32_t>(list.ports.size());
        if (output == 1) {
            green::StringArray names;
            for (const auto& port : list.ports) names.push_back(port.name);
            return names;
        }
        green::IntArray available;
        for (const auto& port : list.ports) {
            available.push_back(!list.leases.contains(port.id) && !list.unavailable.contains(port.id));
        }
        return available;
    }
    if (isIndexSelector(entry.kind) && output == selectedOutput(entry.kind)) {
        return entry.midiSelection ? entry.midiSelection->reportedIndex : std::int32_t{-1};
    }
    if (entry.midi && entry.midi->values[0]) return *entry.midi->values[0];
    return green::MidiMessage{};
}

void Schematic::receiveMidiDeviceTrigger(SchematicNode& entry, int input)
{
    if (isIndexSelector(entry.kind) && input == deviceIndexInput(entry.kind)) {
        if (rootGraph().midiDevices_.backend) static_cast<void>(refreshMidiDevices(false));
        return;
    }
    if (direction(entry.kind) != MidiDirection::Output || !entry.midiSelection) return;
    const auto message = std::get<green::MidiMessage>(readInput(entry.id, input));
    if (message.bytes.empty()) return;
    const MidiDeviceOwner owner{path_, entry.id};
    auto& leases = rootGraph().midiDevices_.lists[std::to_underlying(MidiDirection::Output)].leases;
    std::exception_ptr failure;
    for (const auto& id : entry.midiSelection->ports) {
        const auto lease = leases.find(id);
        if (lease == leases.end() || lease->second.owner != owner) continue;
        try {
            lease->second.output->send(message);
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    }
    if (failure) std::rethrow_exception(failure);
}

}
