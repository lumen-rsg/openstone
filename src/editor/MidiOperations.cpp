#include "Schematic.hpp"

#include "MidiMessage.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <string_view>
#include <utility>

namespace openstone {
namespace {

using green::MidiMessage;
using green::NoteEvent;
using green::Notes;

constexpr int noteOff = 0x80;
constexpr int noteOn = 0x90;
constexpr int controlChange = 0xB0;
constexpr int channelPressure = 0xD0;
constexpr int pitchBend = 0xE0;
constexpr int sysexStart = 0xF0;
constexpr int sysexEnd = 0xF7;
constexpr int sustainPedal = 64;
constexpr int resetControllers = 121;
constexpr int allNotesOff = 123;

struct MessageFields {
    int status;
    int channel;
    int data1;
    int data2;
};

int messageStatus(int byte)
{
    return byte < sysexStart ? byte & 0xF0 : byte;
}

int messageLength(int status)
{
    switch (status) {
    case noteOff: case noteOn: case 0xA0: case controlChange: case pitchBend: case 0xF2:
        return 3;
    case 0xC0: case channelPressure: case 0xF1: case 0xF3:
        return 2;
    case 0xF6: case sysexEnd: case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF:
        return 1;
    default:
        return 0;
    }
}

bool validData(int value) { return value >= 0 && value <= 127; }
bool validChannel(int channel) { return channel >= 1 && channel <= 16; }

bool isSysex(const MidiMessage& message)
{
    const auto& bytes = message.bytes;
    return bytes.size() >= 3 && bytes.front() == sysexStart && bytes.back() == sysexEnd &&
           std::all_of(bytes.begin() + 1, bytes.end() - 1, validData);
}

std::optional<MessageFields> splitMessage(const MidiMessage& message)
{
    const auto& bytes = message.bytes;
    if (bytes.empty()) return {};
    const int status = messageStatus(bytes.front());
    if (status == sysexStart) {
        if (!isSysex(message)) return {};
    } else if (bytes.size() != static_cast<std::size_t>(messageLength(status)) ||
               !std::all_of(bytes.begin() + 1, bytes.end(), validData)) {
        return {};
    }
    return MessageFields{status, status < sysexStart ? (bytes.front() & 0x0F) + 1 : 0,
                         bytes.size() > 1 ? bytes[1] : 0, bytes.size() > 2 ? bytes[2] : 0};
}

std::optional<MidiMessage> makeMessage(int status, int channel, int data1, int data2)
{
    if (status < 0x80 || status > 0xFF) return {};
    status = messageStatus(status);
    const int length = messageLength(status);
    if (!length || (status < sysexStart && !validChannel(channel)) ||
        (length >= 2 && !validData(data1)) || (length == 3 && !validData(data2))) {
        return {};
    }
    const auto byte = static_cast<std::uint8_t>(status < sysexStart ? status | (channel - 1) : status);
    MidiMessage result{{byte}};
    if (length >= 2) result.bytes.push_back(static_cast<std::uint8_t>(data1));
    if (length == 3) result.bytes.push_back(static_cast<std::uint8_t>(data2));
    return result;
}

MidiMessage encodeNote(const NoteEvent& event)
{
    return MidiMessage{{static_cast<std::uint8_t>((event.noteOn ? noteOn : noteOff) | event.channel),
                        event.key, event.velocity}};
}

std::optional<NoteEvent> decodeNote(MessageFields fields)
{
    if (fields.status != noteOn && fields.status != noteOff) return {};
    return NoteEvent{static_cast<std::uint8_t>(fields.channel - 1), static_cast<std::uint8_t>(fields.data1),
                     static_cast<std::uint8_t>(fields.data2), fields.status == noteOn && fields.data2 != 0};
}

std::optional<MidiMessage> parseSysex(std::string_view hex)
{
    MidiMessage result;
    int highNibble = -1;
    for (const auto ch : hex) {
        if (std::string_view{" \t\r\n"}.contains(ch)) continue;
        unsigned digit{};
        const auto [ptr, error] = std::from_chars(&ch, &ch + 1, digit, 16);
        if (error != std::errc{} || ptr != &ch + 1) return {};
        if (highNibble < 0) {
            highNibble = static_cast<int>(digit);
        } else {
            result.bytes.push_back(static_cast<std::uint8_t>(highNibble * 16 + digit));
            highNibble = -1;
        }
    }
    if (highNibble >= 0 || result.bytes.empty()) return {};
    if (result.bytes.front() != sysexStart) {
        result.bytes.insert(result.bytes.begin(), sysexStart);
        result.bytes.push_back(sysexEnd);
    }
    if (!isSysex(result)) return {};
    return result;
}

std::string formatSysex(const MidiMessage& message)
{
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string result;
    for (const auto byte : message.bytes) {
        if (!result.empty()) result += ' ';
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

bool sameKey(const NoteEvent& first, const NoteEvent& second)
{
    return first.channel == second.channel && first.key == second.key;
}

Notes routeNotes(MidiState& state, MessageFields message, bool noRepeat, bool noSustainRepeat)
{
    Notes result;
    if (message.channel == 0) return result;
    const auto channel = static_cast<std::uint8_t>(message.channel - 1);
    auto& sustain = state.sustain[channel];
    const auto onChannel = [channel](const auto& note) { return note.channel == channel; };
    const auto releaseSustained = [&] {
        for (const auto& note : state.sustained) {
            if (onChannel(note)) result.events.push_back(note);
        }
        std::erase_if(state.sustained, onChannel);
    };
    const auto release = [&](NoteEvent note) {
        note.noteOn = false;
        (sustain ? state.sustained : result.events).push_back(note);
    };
    if (auto note = decodeNote(message)) {
        const auto matches = [&note](const auto& other) { return sameKey(*note, other); };
        const auto held = std::ranges::find_if(state.held, matches);
        if (note->noteOn) {
            const bool suppressRepeat = noRepeat || (sustain && noSustainRepeat);
            if (suppressRepeat && held != state.held.end()) return result;
            const auto deferred = std::ranges::find_if(state.sustained, matches);
            if (suppressRepeat && deferred != state.sustained.end()) {
                // A sustained note becomes held again without a new attack.
                state.sustained.erase(deferred);
            } else {
                result.events.push_back(*note);
            }
            state.held.push_back(*note);
        } else if (held != state.held.end()) {
            state.held.erase(held);
            release(*note);
        }
    } else if (message.status == controlChange) {
        if (message.data1 == sustainPedal) {
            sustain = message.data2 >= 64;
            if (!sustain) releaseSustained();
        } else if (message.data1 == resetControllers) {
            sustain = false;
            releaseSustained();
        } else if (message.data1 >= allNotesOff) {
            for (const auto& note : state.held) {
                if (onChannel(note)) release({note.channel, note.key, 0, false});
            }
            std::erase_if(state.held, onChannel);
        }
    }
    return result;
}

}

bool validMidiMessage(const green::MidiMessage& message)
{
    return splitMessage(message).has_value();
}

void Schematic::publishMidi(SchematicNode& entry, MidiMessage message, int output)
{
    entry.midi->values[output] = std::move(message);
    fireOutput(entry, output);
}

void Schematic::setHostMidiOutput(std::function<void(const MidiMessage&)> output)
{
    rootGraph().hostMidiOutput_ = std::move(output);
}

void Schematic::receiveHostMidi(const MidiMessage& message)
{
    if (!validMidiMessage(message)) return;
    const auto deliver = [&](const auto& self, Schematic& level) -> void {
        for (auto& entry : level.nodes_) {
            if (entry.kind != PrimitiveKind::VstMidiIn) continue;
            if (!entry.midi) entry.midi.emplace();
            level.publishMidi(entry, message);
        }
        for (auto& [id, child] : level.modules_) self(self, *child);
    };
    deliver(deliver, rootGraph());
    rootGraph().refreshConnections();
}

void Schematic::receiveHostMidiTrigger(SchematicNode& entry, int input)
{
    const auto& output = rootGraph().hostMidiOutput_;
    if (entry.kind == PrimitiveKind::VstMidiOut && input == 0 && output) {
        const auto message = std::get<MidiMessage>(readInput(entry.id, 0));
        if (splitMessage(message)) output(message);
    }
}

float Schematic::readMidiStream(const SchematicNode& entry, std::optional<std::size_t> midiChannel) const
{
    using enum PrimitiveKind;
    const auto* state = entry.midi ? &*entry.midi : nullptr;
    const auto raw = state && state->values[0] ? std::optional{std::get<std::int32_t>(*state->values[0])}
                                              : std::nullopt;
    if (entry.kind == MidiCCStream) {
        const auto ccValue = midiChannel && state ? state->streamCc.at(*midiChannel) : std::nullopt;
        const auto value = midiChannel ? (ccValue ? std::optional<std::int32_t>{*ccValue} : std::nullopt) : raw;
        if (!value || state->streamController != std::get<std::int32_t>(readInput(entry.id, 1))) {
            return std::get<float>(readInput(entry.id, 4));
        }
        const auto minimum = std::get<float>(readInput(entry.id, 2));
        const auto maximum = std::get<float>(readInput(entry.id, 3));
        return std::lerp(minimum, maximum, *value / 127.0F);
    }
    if (entry.kind == MidiPitchBendStream) {
        const int offset = raw.value_or(8192) - 8192;
        return offset / (offset < 0 ? 8192.0F : 8191.0F);
    }
    return raw.value_or(0) / 127.0F;
}

void Schematic::receiveMidiTrigger(SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    const auto id = entry.id;
    const auto readInt = [&](int port) { return std::get<std::int32_t>(readInput(id, port)); };
    if (entry.kind == MidiEvent) {
        if (input != 4) return;
        if (auto message = makeMessage(readInt(0), readInt(1), readInt(2), readInt(3))) {
            if (!entry.midi) entry.midi.emplace();
            publishMidi(entry, std::move(*message));
        }
        return;
    }
    if (entry.kind == MidiStringToSysex) {
        if (input != 1) return;
        if (auto message = parseSysex(std::get<std::string>(readInput(id, 0)))) {
            if (!entry.midi) entry.midi.emplace();
            publishMidi(entry, std::move(*message));
        }
        return;
    }
    if (entry.kind == MidiNoteEvent) {
        if (input != 4) return;
        const auto channel = readInt(0);
        const auto key = readInt(1);
        const auto velocity = readInt(2);
        if (!validChannel(channel) || !validData(key) || !validData(velocity)) return;

        if (!entry.midi) entry.midi.emplace();
        const NoteEvent note{static_cast<std::uint8_t>(channel - 1), static_cast<std::uint8_t>(key),
                             static_cast<std::uint8_t>(velocity), velocity != 0};
        const auto duration = readInt(3);
        if (note.noteOn && duration > 0) {
            const auto& root = rootGraph();
            const auto& clock = root.systemInfo_.monotonicClock;
            const auto now = root.tickDispatchTime_ ? *root.tickDispatchTime_
                                                   : clock ? clock() : TickTime{std::chrono::steady_clock::now()};
            const auto deadline = now + std::chrono::milliseconds(duration);
            auto& pending = entry.midi->scheduled;
            const auto position = std::ranges::upper_bound(pending, deadline, {}, &MidiState::ScheduledNote::deadline);
            pending.insert(position, {deadline, {note.channel, note.key, 0, false}});
        }
        publishMidi(entry, encodeNote(note));
        if (note.noteOn && duration <= 0) publishMidi(entry, encodeNote({note.channel, note.key, 0, false}));
        return;
    }
    if (entry.kind == MidiBuffer && input != 0) {
        if (entry.midi) {
            auto messages = std::exchange(entry.midi->buffer, {});
            if (input == 1) {
                for (auto& message : messages) publishMidi(entry, std::move(message));
            }
        }
        if (input == 1) fireOutput(entry, 1);
        return;
    }
    if (input != 0) {
        if (entry.kind == MidiCCStream && input == 1 && entry.midi && entry.midi->streamController != readInt(1)) {
            entry.midi->streamController.reset();
            entry.midi->values[0].reset();
            entry.midi->streamCc = {};
        }
        return;
    }

    auto message = std::get<MidiMessage>(readInput(id, 0));
    const auto fields = splitMessage(message);
    if (!fields) return;
    if (!entry.midi) entry.midi.emplace();
    auto& state = *entry.midi;
    auto& values = state.values;
    const auto storeInt = [&](int port, int value) { values[port] = static_cast<std::int32_t>(value); };
    switch (entry.kind) {
    case MidiAftertouch:
    case MidiAftertouchStream:
        if (fields->status == channelPressure) {
            storeInt(0, fields->data1);
            if (entry.kind == MidiAftertouch) fireOutput(entry, 0);
        }
        break;
    case MidiCC:
    case MidiCCStream:
        if (fields->status == controlChange && fields->data1 == readInt(1)) {
            storeInt(0, fields->data2);
            if (entry.kind == MidiCC) {
                fireOutput(entry, 1);
            } else {
                if (state.streamController != fields->data1) state.streamCc = {};
                state.streamController = fields->data1;
                state.streamCc.at(fields->channel - 1) = static_cast<std::uint8_t>(fields->data2);
            }
        }
        break;
    case MidiPitchBend:
    case MidiPitchBendStream:
        if (fields->status == pitchBend) {
            storeInt(0, (fields->data2 << 7) | fields->data1);
            if (entry.kind == MidiPitchBend) fireOutput(entry, 0);
        }
        break;
    case MidiSplit:
        storeInt(0, fields->status);
        storeInt(1, fields->channel);
        storeInt(2, fields->data1);
        storeInt(3, fields->data2);
        fireOutput(entry, 4);
        break;
    case MidiBuffer:
        state.buffer.push_back(std::move(message));
        break;
    case MidiChannelFilter: {
        const auto filterChannel = readInt(1);
        publishMidi(entry, std::move(message), validChannel(filterChannel) && fields->channel == filterChannel ? 0 : 1);
        break;
    }
    case MidiStatusFilter: {
        const auto filterStatus = readInt(1);
        const bool matches = filterStatus >= 0x80 && filterStatus <= 0xFF &&
                             fields->status == messageStatus(filterStatus);
        publishMidi(entry, std::move(message), matches ? 0 : 1);
        break;
    }
    case MidiNoteFilter:
        publishMidi(entry, std::move(message), decodeNote(*fields) && fields->data1 == readInt(1) ? 0 : 1);
        break;
    case MidiSysexToString:
        if (fields->status == sysexStart) {
            values[0] = formatSysex(message);
            fireOutput(entry, 1);
        }
        break;
    case MidiToNotes: {
        auto notes = routeNotes(state, *fields, std::get<bool>(readInput(id, 1)), std::get<bool>(readInput(id, 2)));
        if (!notes.events.empty()) {
            values[0] = std::move(notes);
            fireOutput(entry, 0);
        }
        break;
    }
    case MidiNote: {
        if (const auto note = decodeNote(*fields)) {
            if (note->noteOn) {
                state.held.push_back(*note);
            } else {
                const auto held = std::ranges::find_if(state.held, [&note](const auto& key) { return sameKey(*note, key); });
                if (held == state.held.end()) break;
                state.held.erase(held);
            }
        } else if (fields->status == controlChange && fields->data1 >= allNotesOff) {
            std::erase_if(state.held, [&fields](const auto& note) { return note.channel == fields->channel - 1; });
        } else {
            break;
        }
        storeInt(1, static_cast<int>(state.held.size()));
        if (!state.held.empty()) {
            const int key = state.held.back().key;
            storeInt(0, key);
            values[2] = static_cast<float>(880.0 * std::exp2((key - 69) / 12.0) / sampleRate());
        }
        fireOutputs(id);
        break;
    }
    default:
        break;
    }
}

void Schematic::advanceMidiNotes(SchematicNode& entry, TickTime now, TickSchedule& result)
{
    auto& pending = entry.midi->scheduled;
    while (!pending.empty() && pending.front().deadline <= now) {
        const auto note = pending.front().note;
        pending.erase(pending.begin());
        result.fired = true;
        publishMidi(entry, encodeNote(note));
    }
}

}
