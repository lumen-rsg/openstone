#include "Schematic.hpp"

#include <algorithm>
#include <chrono>

namespace openstone {

bool Schematic::hasSequencerTracks() const
{
    for (const auto& entry : nodes_) {
        if (entry.kind == PrimitiveKind::StreamSequencerTrack || entry.kind == PrimitiveKind::MidiSequencerTrack) return true;
    }
    for (const auto& [id, child] : modules_) if (child->hasSequencerTracks()) return true;
    return false;
}

std::shared_ptr<stream::SequenceAudioTrack> Schematic::sequencerAudio(const SchematicNode& entry) const
{
    if (!entry.sequenceAudio) {
        entry.sequenceAudio = std::make_shared<stream::SequenceAudioTrack>();
        if (sequencerMode() == stream::SequenceMode::Recording) {
            entry.sequenceAudio->prepareTake(sampleRate(), rootGraph().sequenceTransport_->requestedCommand());
        }
    }
    return entry.sequenceAudio;
}

stream::SequenceMode Schematic::sequencerMode() const
{
    return rootGraph().sequenceTransport_->requestedMode();
}

void Schematic::setSequencerMode(stream::SequenceMode mode, TickTime now)
{
    using enum stream::SequenceMode;
    auto& root = rootGraph();
    const auto previous = root.sequencerMode();
    if (mode == previous) return;
    if (previous == Recording) {
        root.sequenceDuration_ = std::clamp(root.sequenceElapsed(now), TickTime::duration{}, TickTime::duration{stream::maxSequenceSeconds});
    }
    const auto command = root.sequenceTransport_->nextCommand(mode);
    // Finish MIDI notes before seeking or starting a new take.
    const auto reset = [&](const auto& walk, Schematic& graph) -> void {
        for (auto& entry : graph.nodes_) {
            if (entry.kind == PrimitiveKind::StreamSequencerTrack && mode == Recording && entry.sequenceAudio) {
                entry.sequenceAudio->prepareTake(root.sampleRate(), command);
            }
            if (entry.kind != PrimitiveKind::MidiSequencerTrack) continue;
            if (!entry.sequenceMidi) entry.sequenceMidi.emplace();
            auto& midi = *entry.sequenceMidi;
            for (const auto& [key, off] : midi.held) graph.publishMidi(entry, off);
            midi.held.clear();
            midi.next = 0;
            if (mode == Recording) midi.take.clear();
        }
        for (auto& [id, child] : graph.modules_) walk(walk, *child);
    };
    reset(reset, root);
    root.sequenceStart_ = now;
    root.sequenceTransport_->setMode(mode);
    root.refreshConnections();
}

void Schematic::receiveSequenceMidi(SchematicNode& entry)
{
    const auto message = std::get<green::MidiMessage>(readInput(entry.id, 0));
    if (message.bytes.empty()) return;
    if (!entry.midi) entry.midi.emplace();
    auto& root = rootGraph();
    if (root.sequencerMode() == stream::SequenceMode::Recording) {
        if (!entry.sequenceMidi) entry.sequenceMidi.emplace();
        auto& take = entry.sequenceMidi->take;
        const auto time = root.sequenceElapsed(root.sequenceNow());
        if (take.size() < stream::maxSequenceMidiEvents && time.count() < stream::maxSequenceSeconds) {
            take.emplace_back(take.empty() ? time : std::max(time, take.back().first), message);
        }
    }
    publishMidi(entry, message);
}

TickTime Schematic::sequenceNow() const
{
    const auto& backend = rootGraph().systemInfo_.monotonicClock;
    return backend ? backend() : std::chrono::steady_clock::now();
}

TickTime::duration Schematic::sequenceElapsed(TickTime now) const
{
    if (audioMonitor()) return TickTime::duration{sequenceTransport_->completed() / sampleRate()};
    return std::max(TickTime::duration{}, now - sequenceStart_);
}

bool Schematic::advanceSequencer(TickTime now)
{
    auto& root = rootGraph();
    if (root.sequencerMode() != stream::SequenceMode::Playing) return false;
    const auto elapsed = std::min(root.sequenceElapsed(now), root.sequenceDuration_);
    bool changed = false;
    const auto advance = [&](const auto& walk, Schematic& graph) -> void {
        for (auto& entry : graph.nodes_) {
            if (!entry.sequenceMidi) continue;
            auto& midi = *entry.sequenceMidi;
            while (midi.next < midi.take.size() && midi.take[midi.next].first <= elapsed) {
                const auto& event = midi.take[midi.next++].second;
                if (event.bytes.size() >= 3) {
                    const auto status = event.bytes[0] & 0xf0;
                    const auto key = std::pair{static_cast<int>(event.bytes[0] & 0x0f), static_cast<int>(event.bytes[1])};
                    if (status == 0x90 && event.bytes[2] != 0) {
                        midi.held.try_emplace(key, green::MidiMessage{{static_cast<std::uint8_t>(0x80 | key.first),
                                                                       event.bytes[1], 0}});
                    } else if (status == 0x80 || status == 0x90) {
                        midi.held.erase(key);
                    }
                }
                if (!entry.midi) entry.midi.emplace();
                graph.publishMidi(entry, event);
                changed = true;
                if (root.sequencerMode() != stream::SequenceMode::Playing) return;
            }
        }
        for (auto& [id, child] : graph.modules_) walk(walk, *child);
    };
    advance(advance, root);
    if (elapsed >= root.sequenceDuration_) {
        root.setSequencerMode(stream::SequenceMode::Stopped, now);
        return true;
    }
    if (changed) root.refreshConnections();
    return changed;
}

}
