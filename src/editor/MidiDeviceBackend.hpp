#pragma once

#include "green/Value.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openstone {

enum class MidiDirection { Input, Output };

struct MidiDeviceInfo {
    std::string id;
    std::string name;

    bool operator==(const MidiDeviceInfo&) const = default;
};

class MidiInputPort {
public:
    virtual ~MidiInputPort() = default;
    // Returns one complete MIDI 1.0 message, or an empty optional when the queue is empty.
    virtual std::optional<green::MidiMessage> receive() = 0;
};

class MidiOutputPort {
public:
    virtual ~MidiOutputPort() = default;
    virtual void send(const green::MidiMessage& message) = 0;
};

// Device handles are used on the control thread. A backend may queue input on driver threads.
class MidiDeviceBackend {
public:
    virtual ~MidiDeviceBackend() = default;
    virtual std::vector<MidiDeviceInfo> list(MidiDirection direction) = 0;
    virtual std::unique_ptr<MidiInputPort> openInput(std::string_view id) = 0;
    virtual std::unique_ptr<MidiOutputPort> openOutput(std::string_view id) = 0;
};

}
