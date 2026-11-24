#include "MidiDevice.hpp"

#if __has_include(<rtmidi/RtMidi.h>)
#include <rtmidi/RtMidi.h>
#else
#include <RtMidi.h>
#endif

#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#endif

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace openstone::desktop {
namespace {

struct IndexedPort {
    MidiDeviceInfo info;
    unsigned int index;
};

std::string portLabel(std::string name, RtMidi::Api api, unsigned int index)
{
    if (api == RtMidi::WINDOWS_MM) {
        const auto suffix = " " + std::to_string(index);
        if (name.ends_with(suffix)) name.resize(name.size() - suffix.size());
    } else if (api == RtMidi::LINUX_ALSA) {
        const auto space = name.find_last_of(' ');
        if (space != std::string::npos &&
            name.find_first_not_of("0123456789:", space + 1) == std::string::npos) {
            name.resize(space);
        }
    }
    return name;
}

std::string nativeId(RtMidi::Api api, MidiDirection direction, unsigned int index)
{
#ifdef __APPLE__
    if (api == RtMidi::MACOSX_CORE) {
        const auto endpoint = direction == MidiDirection::Input ? MIDIGetSource(index) : MIDIGetDestination(index);
        SInt32 id = 0;
        if (endpoint && MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &id) == noErr && id != 0) {
            return "coremidi:" + std::to_string(id);
        }
    }
#else
    static_cast<void>(api);
    static_cast<void>(direction);
    static_cast<void>(index);
#endif
    return {};
}

template<class Device>
std::vector<IndexedPort> ports(Device& midi, MidiDirection direction)
{
    const auto api = midi.getCurrentApi();
    const auto prefix = RtMidi::getApiName(api) + ':';
    std::vector<IndexedPort> result;
    std::map<std::string, int> occurrences;
    const auto count = midi.getPortCount();
    for (unsigned int index = 0; index < count; ++index) {
        auto name = portLabel(midi.getPortName(index), api, index);
        if (name.empty()) continue;
        auto id = nativeId(api, direction, index);
        if (id.empty()) {
            const auto occurrence = ++occurrences[name];
            id = prefix + std::to_string(occurrence) + ':' + name;
            if (occurrence > 1) name += " (" + std::to_string(occurrence) + ')';
        }
        result.push_back({{std::move(id), std::move(name)}, index});
    }
    return result;
}

template<class Device>
void connectPort(Device& midi, MidiDirection direction, std::string_view id)
{
    const auto scanned = ports(midi, direction);
    const auto match = std::ranges::find_if(scanned, [id](const auto& port) { return port.info.id == id; });
    if (match == scanned.end()) throw std::runtime_error("MIDI device is no longer connected");
    midi.openPort(match->index);
    if (!midi.isPortOpen()) throw std::runtime_error("Could not open MIDI device");
    // Enumeration indices can change during a hot plug.
    const auto current = ports(midi, direction);
    const auto opened = std::ranges::find(current, match->index, &IndexedPort::index);
    if (opened == current.end() || opened->info.id != id) {
        throw std::runtime_error("MIDI devices changed while opening a port");
    }
}

class Input final : public MidiInputPort {
public:
    explicit Input(std::string_view id)
    {
        midi_.ignoreTypes(false, false, false);
        midi_.setCallback(receiveMessage, this);
        connectPort(midi_, MidiDirection::Input, id);
    }

    std::optional<green::MidiMessage> receive() override
    {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return {};
        auto message = std::move(queue_.front());
        queue_.pop_front();
        return message;
    }

private:
    static void receiveMessage(double, std::vector<unsigned char>* bytes, void* context) noexcept
    {
        auto& port = *static_cast<Input*>(context);
        try {
            std::lock_guard lock(port.mutex_);
            if (!bytes->empty() && port.queue_.size() < 2048) {
                port.queue_.push_back({*bytes});
            }
        } catch (...) {
            // Keep callback exceptions inside the adapter's driver thread.
        }
    }

    std::mutex mutex_;
    std::deque<green::MidiMessage> queue_;
    // Destroy the RtMidi input before destroying its callback context.
    RtMidiIn midi_{RtMidi::UNSPECIFIED, "OpenStone MIDI In"};
};

class Output final : public MidiOutputPort {
public:
    explicit Output(std::string_view id)
    {
        connectPort(midi_, MidiDirection::Output, id);
    }

    void send(const green::MidiMessage& message) override
    {
        try {
            midi_.sendMessage(&message.bytes);
        } catch (const RtMidiError& error) {
            throw std::runtime_error(error.getMessage());
        }
    }

private:
    RtMidiOut midi_{RtMidi::UNSPECIFIED, "OpenStone MIDI Out"};
};

template<class Device>
std::vector<MidiDeviceInfo> listPorts(MidiDirection direction)
{
    Device probe(RtMidi::UNSPECIFIED, "OpenStone MIDI Discovery");
    std::vector<MidiDeviceInfo> result;
    for (auto& port : ports(probe, direction)) result.push_back(std::move(port.info));
    return result;
}

class Backend final : public MidiDeviceBackend {
public:
    std::vector<MidiDeviceInfo> list(MidiDirection direction) override
    {
        try {
            return direction == MidiDirection::Input ? listPorts<RtMidiIn>(direction) : listPorts<RtMidiOut>(direction);
        } catch (const RtMidiError& error) {
            throw std::runtime_error(error.getMessage());
        }
    }

    std::unique_ptr<MidiInputPort> openInput(std::string_view id) override
    {
        try {
            return std::make_unique<Input>(id);
        } catch (const RtMidiError& error) {
            throw std::runtime_error(error.getMessage());
        }
    }

    std::unique_ptr<MidiOutputPort> openOutput(std::string_view id) override
    {
        try {
            return std::make_unique<Output>(id);
        } catch (const RtMidiError& error) {
            throw std::runtime_error(error.getMessage());
        }
    }
};

}

std::shared_ptr<MidiDeviceBackend> makeMidiDeviceBackend()
{
    return std::make_shared<Backend>();
}

}
