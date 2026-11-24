#include "Schematic.hpp"

#include "StringConversions.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace openstone {

struct SerialPortState {
    std::unique_ptr<SerialDevicePort> connection;
    std::deque<std::string> sends;
    std::size_t sendOffset = 0;
    std::size_t unsentBytes = 0;
    std::size_t buffer = 0;
    std::string receiving;
    std::string received;
    std::string log;
    std::uint64_t generation = 0;
    int terminator = 0;
    bool opening = false;
    bool discarding = false;

    void clearSends()
    {
        sends.clear();
        sendOffset = unsentBytes = buffer = 0;
    }

    void resetConnection()
    {
        connection.reset();
        clearSends();
        receiving.clear();
        discarding = false;
        opening = false;
        ++generation;
    }
};

namespace {

enum Input {
    PortNumber, Baud, StopBits, Parity, DataBits, OpenPort, ClosePort, Data, RxTerminator,
    Send, Hex, Logging, ClearLog, NoNull, Refresh, ClearSends
};
enum Output { IsOpen, Received, Sent, LogText, Ports, Opening, Buffer };

constexpr std::size_t bufferLimit = 1024 * 1024;
constexpr std::size_t logLimit = 64 * 1024;
constexpr std::size_t readLimit = 4096;

std::vector<SerialDeviceInfo> portsFrom(const std::shared_ptr<SerialDeviceBackend>& backend)
{
    auto ports = backend ? backend->list() : std::vector<SerialDeviceInfo>{};
    std::erase_if(ports, [](const auto& entry) { return entry.number < 1 || entry.id.empty(); });
    std::ranges::sort(ports, [](const auto& a, const auto& b) {
        return std::tie(a.number, a.id) < std::tie(b.number, b.id);
    });
    const auto duplicates = std::ranges::unique(ports, {}, &SerialDeviceInfo::number);
    ports.erase(duplicates.begin(), duplicates.end());
    return ports;
}

}

void Schematic::setSerialDeviceBackend(std::shared_ptr<SerialDeviceBackend> backend)
{
    auto& root = rootGraph();
    if (root.serialBackend_ == backend) return;
    auto ports = portsFrom(backend);
    const auto reset = [](const auto& visit, Schematic& graph) -> void {
        for (auto& entry : graph.nodes_) {
            if (entry.serialPort) entry.serialPort->resetConnection();
        }
        for (auto& [id, child] : graph.modules_) visit(visit, *child);
    };
    reset(reset, root);
    root.serialBackend_ = std::move(backend);
    root.serialPorts_ = std::move(ports);
}

void Schematic::refreshSerialPorts(bool notify)
{
    auto& root = rootGraph();
    if (root.refreshingSerialPorts_) return;
    struct ResetFlag {
        bool& flag;
        ~ResetFlag() { flag = false; }
    } reset{root.refreshingSerialPorts_};
    root.refreshingSerialPorts_ = true;
    root.serialPorts_ = portsFrom(root.serialBackend_);
    if (!notify) return;
    const auto publish = [](const auto& visit, Schematic& graph) -> void {
        for (auto& entry : graph.nodes_) {
            if (entry.kind == PrimitiveKind::ComPort) graph.fireOutput(entry, Ports);
            if (entry.kind == PrimitiveKind::ComPorts) graph.fireOutput(entry, 0);
        }
        for (auto& [id, child] : graph.modules_) visit(visit, *child);
    };
    publish(publish, root);
}

green::Value Schematic::readSerialDevice(const SchematicNode& entry, int output) const
{
    if (entry.kind == PrimitiveKind::ComPorts || output == Ports) {
        green::IntArray numbers;
        for (const auto& port : rootGraph().serialPorts_) numbers.push_back(port.number);
        return numbers;
    }
    const auto* state = entry.serialPort.get();
    switch (output) {
    case IsOpen: return state && state->connection;
    case Received: return state ? state->received : std::string{};
    case LogText: return state ? state->log : std::string{};
    case Opening: return state && state->opening;
    case Buffer: return state ? static_cast<std::int32_t>(state->buffer) : 0;
    default: throw std::logic_error("Expected a COM Port data output");
    }
}

void Schematic::logSerial(SchematicNode& entry, std::string_view message)
{
    if (!std::get<bool>(readInput(entry.id, Logging))) return;
    auto& log = entry.serialPort->log;
    message = message.substr(0, logLimit - 1);
    if (log.size() + message.size() + 1 > logLimit) {
        const auto cutoff = log.find('\n', log.size() + message.size() - logLimit);
        log.erase(0, cutoff == std::string::npos ? log.size() : cutoff + 1);
    }
    log.append(message);
    log += '\n';
    fireOutput(entry, LogText);
}

void Schematic::setSerialOpen(SchematicNode& entry, bool open)
{
    auto& state = *entry.serialPort;
    if (open && state.opening) return;
    const bool wasOpen = bool(state.connection);
    const bool wasOpening = state.opening;
    const auto previousBuffer = state.buffer;
    state.resetConnection();
    const auto generation = state.generation;
    if (!open) {
        if (wasOpening) fireOutput(entry, Opening);
        if (wasOpen) fireOutput(entry, IsOpen);
        if (previousBuffer > 0) fireOutput(entry, Buffer);
        return;
    }

    const auto integer = [&](int port) { return std::get<int>(readInput(entry.id, port)); };
    const int number = integer(PortNumber);
    const int baud = integer(Baud);
    const int bits = integer(DataBits);
    const SerialPortSettings settings{baud == 0 ? 9600 : baud, integer(StopBits), integer(Parity),
                                      bits == 0 ? 8 : bits};
    state.opening = true;
    std::string error;
    try {
        fireOutput(entry, Opening);
        if (state.generation != generation) return;
        auto& root = rootGraph();
        if (!root.serialBackend_) throw std::runtime_error("Serial device access is unavailable");
        if (settings.baud < 1 || settings.stopBits < 0 || settings.stopBits > 2 ||
            settings.parity < 0 || settings.parity > 4 || settings.dataBits < 5 || settings.dataBits > 8) {
            throw std::invalid_argument("Invalid serial port settings");
        }
        refreshSerialPorts(false);
        const auto found = std::ranges::find(root.serialPorts_, number, &SerialDeviceInfo::number);
        if (found == root.serialPorts_.end()) throw std::runtime_error("COM port is unavailable");
        state.connection = root.serialBackend_->open(found->id, settings);
        if (!state.connection) throw std::runtime_error("COM port could not be opened");
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    if (state.generation != generation) return;
    state.opening = false;
    fireOutput(entry, Opening);
    if (state.generation != generation) return;
    fireOutput(entry, IsOpen);
    if (state.generation != generation) return;
    if (previousBuffer > 0) fireOutput(entry, Buffer);
    if (error.empty()) logSerial(entry, "Opened COM " + std::to_string(number));
    else logSerial(entry, error);
}

void Schematic::receiveSerialDevice(SchematicNode& entry, int input)
{
    if (entry.kind == PrimitiveKind::ComPorts) {
        if (input == 0) refreshSerialPorts(true);
        return;
    }
    if (input != OpenPort && input != ClosePort && input != Send &&
        input != ClearLog && input != Refresh && input != ClearSends) return;
    if (!entry.serialPort) entry.serialPort = std::make_shared<SerialPortState>();
    auto& state = *entry.serialPort;
    if (input == OpenPort || input == ClosePort) {
        setSerialOpen(entry, input == OpenPort);
    } else if (input == Refresh) {
        refreshSerialPorts(true);
    } else if (input == ClearLog) {
        state.log.clear();
        fireOutput(entry, LogText);
    } else if (input == ClearSends) {
        try {
            if (state.connection && state.connection->pendingWrites()) state.connection->clearWrites();
        } catch (const std::exception& error) {
            setSerialOpen(entry, false);
            logSerial(entry, error.what());
            return;
        }
        const bool changed = state.buffer != 0;
        state.clearSends();
        if (changed) fireOutput(entry, Buffer);
    } else {
        if (!state.connection) {
            logSerial(entry, "COM port is closed");
            return;
        }
        auto data = std::get<std::string>(readInput(entry.id, Data));
        try {
            if (std::get<bool>(readInput(entry.id, Hex))) data = decodeHexBytes(data);
            else if (!std::get<bool>(readInput(entry.id, NoNull))) data += '\0';
        } catch (const std::invalid_argument& error) {
            logSerial(entry, error.what());
            return;
        }
        if (data.empty()) return;
        if (data.size() > bufferLimit - std::min(bufferLimit, state.buffer)) {
            logSerial(entry, "Serial send buffer is full");
            return;
        }
        const auto size = data.size();
        state.sends.push_back(std::move(data));
        state.unsentBytes += size;
        state.buffer += size;
        fireOutput(entry, Buffer);
    }
}

bool Schematic::hasOpenSerialPorts() const
{
    const auto visit = [](const auto& self, const Schematic& level) -> bool {
        if (std::ranges::any_of(level.nodes_, [](const auto& entry) {
            return entry.serialPort && entry.serialPort->connection;
        })) return true;
        return std::ranges::any_of(level.modules_, [&](const auto& child) { return self(self, *child.second); });
    };
    return visit(visit, rootGraph());
}

bool Schematic::pollSerialPort(SchematicNode& entry)
{
    auto& state = *entry.serialPort;
    const auto generation = state.generation;
    const auto oldBuffer = state.buffer;
    std::string data;
    std::string sent;
    try {
        auto& port = *state.connection;
        data = port.read(readLimit);
        if (!state.sends.empty()) {
            const auto bytes = std::string_view(state.sends.front()).substr(state.sendOffset);
            if (!bytes.empty()) {
                const auto accepted = port.write(bytes);
                if (accepted > bytes.size()) throw std::runtime_error("Serial device returned an invalid write count");
                state.sendOffset += accepted;
                state.unsentBytes -= accepted;
            }
            const auto pending = port.pendingWrites();
            if (state.sendOffset == state.sends.front().size() && !pending) {
                sent = std::move(state.sends.front());
                state.sends.pop_front();
                state.sendOffset = 0;
            }
            state.buffer = state.unsentBytes + pending;
        }
    } catch (const std::exception& error) {
        setSerialOpen(entry, false);
        logSerial(entry, error.what());
        return true;
    }

    const bool bufferChanged = oldBuffer != state.buffer;
    if (bufferChanged) fireOutput(entry, Buffer);
    if (!sent.empty()) {
        fireOutput(entry, Sent);
        if (std::get<bool>(readInput(entry.id, Logging))) logSerial(entry, "TX " + encodeHexBytes(sent));
    }
    bool changed = bufferChanged || !sent.empty();
    if (data.empty() || state.generation != generation) return changed;

    const int terminator = std::get<int>(readInput(entry.id, RxTerminator));
    if (terminator != state.terminator) {
        state.receiving.clear();
        state.discarding = false;
        state.terminator = terminator;
    }
    const auto publish = [&](std::string frame) {
        if (std::get<bool>(readInput(entry.id, Logging))) logSerial(entry, "RX " + encodeHexBytes(frame));
        if (state.generation != generation) return;
        state.received = std::get<bool>(readInput(entry.id, Hex)) ? encodeHexBytes(frame) : std::move(frame);
        fireOutput(entry, Received);
        changed = true;
    };
    for (const unsigned char byte : data) {
        if (state.generation != generation) break;
        if (terminator < 0) {
            publish(std::string(1, static_cast<char>(byte)));
        } else if (byte == terminator) {
            if (!state.discarding) publish(std::exchange(state.receiving, {}));
            state.discarding = false;
        } else if (!state.discarding) {
            if (state.receiving.size() == bufferLimit) {
                state.receiving.clear();
                state.discarding = true;
                logSerial(entry, "Serial receive frame exceeds the buffer limit");
                changed = true;
            } else {
                state.receiving.push_back(static_cast<char>(byte));
            }
        }
    }
    return changed;
}

bool Schematic::pollSerialDevices()
{
    auto& root = rootGraph();
    const auto visit = [](const auto& self, Schematic& graph) -> bool {
        bool changed = false;
        for (auto& entry : graph.nodes_) {
            if (entry.serialPort && entry.serialPort->connection) changed |= graph.pollSerialPort(entry);
        }
        for (auto& [id, child] : graph.modules_) changed |= self(self, *child);
        return changed;
    };
    return visit(visit, root);
}

}
