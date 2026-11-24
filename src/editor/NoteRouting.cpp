#include "Schematic.hpp"
#include "stream/VoiceBank.hpp"

#include <algorithm>
#include <cmath>

namespace openstone {
namespace {

using green::NoteEvent;

bool sameNote(const NoteEvent& a, const NoteEvent& b)
{
    return a.channel == b.channel && a.key == b.key && a.tags == b.tags;
}

bool validTag(int tag) { return tag >= 0 && tag < 16; }

}

void Schematic::routeNoteLayers(SchematicNode& entry, int input)
{
    if (input != 0) return;
    const auto id = entry.id;
    const auto notes = std::get<green::Notes>(readInput(id, 0));
    if (notes.events.empty()) return;
    const auto readBounds = [&](int port) -> std::vector<float> {
        auto value = readInput(id, port);
        if (auto* floats = std::get_if<green::FloatArray>(&value)) return std::move(*floats);
        const auto& integers = std::get<green::IntArray>(value);
        return {integers.begin(), integers.end()};
    };
    const auto low = readBounds(1);
    const auto high = readBounds(2);
    const int writeTag = std::get<std::int32_t>(readInput(id, 3));
    const auto tagValues = std::get<green::IntArray>(readInput(id, 4));
    const int compareTag = std::get<std::int32_t>(readInput(id, 5));
    const bool duplicate = std::get<bool>(readInput(id, 6));
    const auto layers = std::max({low.size(), high.size(), tagValues.size(), std::size_t{1}});
    const bool velocity = entry.kind == PrimitiveKind::NotesVelocityFilter;
    const bool channel = entry.kind == PrimitiveKind::NotesChannelFilter;
    const float limit = velocity ? 1.0F : channel ? 15.0F : 127.0F;
    green::Notes match, remaining;
    if (!entry.midi) entry.midi.emplace();
    auto& routes = entry.midi->noteRoutes;
    for (const auto& note : notes.events) {
        if (!note.noteOn) {
            const auto route = std::ranges::find_if(routes, [&](const auto& route) { return sameNote(note, route.source); });
            if (route == routes.end()) continue;
            if (route->layers.empty()) remaining.events.push_back(note);
            for (const auto& layer : route->layers) {
                auto release = note;
                release.tags = layer.tags;
                match.events.push_back(release);
            }
            routes.erase(route);
            continue;
        }

        MidiState::NoteRoute route{note, {}};
        const float value = velocity ? note.velocity / 127.0F : channel ? note.channel : note.key;
        for (std::size_t i = 0; i < layers; ++i) {
            if (validTag(compareTag) && note.tags[compareTag] != static_cast<std::int32_t>(i)) continue;
            const auto minimum = i < low.size() ? low[i] : 0.0F;
            const auto maximum = i < high.size() ? high[i] : limit;
            if (!(value >= minimum && value <= maximum)) continue;
            auto tagged = note;
            if (validTag(writeTag)) {
                tagged.tags[writeTag] = i < tagValues.size() ? tagValues[i] : static_cast<std::int32_t>(i);
            }
            route.layers.push_back(tagged);
            if (!duplicate) break;
        }
        if (route.layers.empty()) remaining.events.push_back(note);
        match.events.insert(match.events.end(), route.layers.begin(), route.layers.end());
        routes.push_back(std::move(route));
    }
    const bool matched = !match.events.empty();
    const bool unmatched = !remaining.events.empty();
    entry.midi->values[0] = std::move(match);
    entry.midi->values[1] = std::move(remaining);
    if (matched) fireOutput(entry, 0);
    if (unmatched) fireOutput(entry, 1);
}

green::Value Schematic::readVoiceRouting(const SchematicNode& entry) const
{
    if (entry.kind == PrimitiveKind::Voices) {
        if (!entry.voiceBank) entry.voiceBank = std::make_shared<stream::VoiceBank>();
        const auto id = entry.id;
        entry.voiceBank->configure({
            std::get<std::int32_t>(readInput(id, 0)), std::get<std::int32_t>(readInput(id, 1)),
            std::get<bool>(readInput(id, 2)), std::get<bool>(readInput(id, 3)), std::get<bool>(readInput(id, 4)),
        });
        return green::Voices{{}, entry.voiceBank};
    }
    auto voices = std::get<green::Voices>(readInput(entry.id, 0));
    if (entry.midi && entry.midi->values[0]) {
        const auto& last = std::get<green::Voices>(*entry.midi->values[0]);
        if (last.bank == voices.bank) return last;
    }
    voices.events.clear();
    return voices;
}

void Schematic::receiveVoiceRouting(SchematicNode& entry, int input)
{
    const auto id = entry.id;
    if (entry.kind == PrimitiveKind::Voices) {
        const auto capacity = entry.voiceBank ? entry.voiceBank->capacity() : 32;
        const auto bank = std::get<green::Voices>(readVoiceRouting(entry)).bank;
        if (capacity != bank->capacity()) {
            auto& root = rootGraph();
            ++root.audioTopology_;
            root.monoPreviews_.clear();
            root.dspTraces_.clear();
        }
        fireOutput(entry, 0);
        return;
    }
    auto voices = std::get<green::Voices>(readInput(id, 0));
    if (input == 1 && voices.bank) {
        const auto notes = std::get<green::Notes>(readInput(id, 1));
        voices.events = voices.bank->apply(notes.events);
    } else {
        voices.events.clear();
    }
    const bool notify = input == 0 || !voices.events.empty();
    if (!entry.midi) entry.midi.emplace();
    entry.midi->values[0] = std::move(voices);
    if (notify) fireOutput(entry, 0);
}

void Schematic::receiveVoiceSignal(SchematicNode& entry, int input)
{
    if (entry.kind != PrimitiveKind::VoicesToPoly || input != 0) return;
    const auto bank = std::get<green::Voices>(readInput(entry.id, 0)).bank;
    if (entry.voiceBank != bank) {
        entry.voiceBank = bank;
        auto& root = rootGraph();
        ++root.audioTopology_;
        root.monoPreviews_.clear();
        root.dspTraces_.clear();
    }
}

}
