#include "Schematic.hpp"
#include "StringConversions.hpp"

#include <stdexcept>

namespace openstone {
namespace {

void logLine(NetworkState& state, std::string_view line)
{
    constexpr std::size_t limit = 64 * 1024;
    state.log.append(line.substr(0, limit - 1));
    state.log.push_back('\n');
    if (state.log.size() > limit) state.log.erase(0, state.log.size() - limit);
}

}

green::Value Schematic::readNetwork(const SchematicNode& entry, int output) const
{
    const auto& state = entry.network;
    if (output == 0) return state ? state->status : std::string{"Closed"};
    if (!state) return std::string{};
    if (entry.kind == PrimitiveKind::NetworkServer) {
        if (output == 1) return state->address;
        --output;
    }
    return output == 1 ? state->data : state->log;
}

void Schematic::receiveNetwork(SchematicNode& entry, int input)
{
    const bool server = entry.kind == PrimitiveKind::NetworkServer;
    const int first = server ? 2 : 3;
    if (input != first && input != first + 1 && input != first + 4 && input != first + 6) return;
    if (!entry.network) entry.network.emplace();
    auto& state = *entry.network;
    const int log = server ? 4 : 3;
    const auto logging = std::get<bool>(readInput(entry.id, first + 5));
    if (input == first + 6) {
        state.log.clear();
        fireOutput(entry, log);
        return;
    }
    if (input == first + 4) {
        if (!state.connection) return;
        const auto text = std::get<std::string>(readInput(entry.id, first + 2));
        const bool hex = std::get<bool>(readInput(entry.id, first + 3));
        const auto data = hex ? decodeHexBytes(text) : text;
        if (!state.connection->send(data)) return;
        if (logging) {
            logLine(state, "Sent: " + text);
            fireOutput(entry, log);
        }
        fireOutput(entry, server ? 3 : 2);
        return;
    }

    state.connection.reset();
    state.status = "Closed";
    state.address.clear();
    if (input == first) {
        try {
            const auto port = std::get<std::int32_t>(readInput(entry.id, first - 2));
            const auto protocol = std::get<std::string>(readInput(entry.id, first - 1));
            const bool udp = protocol == "UDP" || protocol == "udp";
            if (!udp && protocol != "TCP" && protocol != "tcp") {
                throw std::invalid_argument("Choose TCP or UDP");
            }
            if (port <= 0 || port > 65535) throw std::invalid_argument("Choose a port from 1 to 65535");
            const auto& backend = rootGraph().systemInfo_.openNetwork;
            if (!backend) throw std::runtime_error("Network backend unavailable");
            const auto host = server ? std::string{} : std::get<std::string>(readInput(entry.id, 0));
            if (!server && host.empty()) throw std::invalid_argument("Enter the server address");
            state.connection = backend({server, udp, host, static_cast<std::uint16_t>(port)});
            if (!state.connection) throw std::runtime_error("Could not open the network connection");
            state.status = "Opening";
        } catch (const std::exception& error) {
            state.status = error.what();
        }
    }
    if (logging) {
        logLine(state, state.status);
        fireOutput(entry, log);
    }
    fireOutput(entry, 0);
    if (server) fireOutput(entry, 1);
    if (state.connection) pollNetworkConnection(entry);
}

bool Schematic::pollNetworkConnection(SchematicNode& entry)
{
    auto& state = *entry.network;
    const auto connection = state.connection;
    auto result = connection->poll();
    const bool server = entry.kind == PrimitiveKind::NetworkServer;
    const int received = server ? 2 : 1;
    const auto logging = std::get<bool>(readInput(entry.id, server ? 7 : 8));
    bool changed = false;
    if (result.finished) state.connection.reset();
    if (state.status != result.status) {
        state.status = std::move(result.status);
        if (logging) logLine(state, state.status);
        fireOutput(entry, 0);
        changed = true;
    }
    if (server && state.address != result.address) {
        state.address = std::move(result.address);
        fireOutput(entry, 1);
        changed = true;
    }
    for (const auto& data : result.received) {
        const bool hex = std::get<bool>(readInput(entry.id, server ? 5 : 6));
        state.data = hex ? encodeHexBytes(data) : data;
        if (logging) logLine(state, "Received: " + state.data);
        fireOutput(entry, received);
        changed = true;
    }
    if (changed && logging) fireOutput(entry, server ? 4 : 3);
    return changed;
}

bool Schematic::hasOpenNetworkConnections() const
{
    const auto& root = rootGraph();
    const auto open = [&](const auto& visit, const Schematic& level) -> bool {
        for (const auto& entry : level.nodes_) if (entry.network && entry.network->connection) return true;
        for (const auto& [id, child] : level.modules_) if (visit(visit, *child)) return true;
        return false;
    };
    return open(open, root);
}

bool Schematic::pollNetworkConnections()
{
    bool changed = false;
    const auto poll = [&](const auto& visit, Schematic& level) -> void {
        for (auto& entry : level.nodes_) {
            if (entry.network && entry.network->connection) changed |= level.pollNetworkConnection(entry);
        }
        for (const auto& [id, child] : level.modules_) visit(visit, *child);
    };
    auto& root = rootGraph();
    poll(poll, root);
    if (changed) root.refreshConnections();
    return changed;
}

}
