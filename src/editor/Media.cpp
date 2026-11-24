#include "Schematic.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openstone {

MediaAudioBuffer::MediaAudioBuffer() : frames_(262144), limit_(frames_.size()) {}

void MediaAudioBuffer::configure(double sampleRate, float seconds) noexcept
{
    if (!std::isfinite(seconds)) seconds = 1.F;
    limit_ = static_cast<std::size_t>(
        std::clamp(sampleRate * std::max(seconds, .25F), 1.0, static_cast<double>(frames_.size())));
}

void MediaAudioBuffer::push(std::span<const Frame> frames) noexcept
{
    const auto next = next_.load(std::memory_order_relaxed);
    const auto consumed = consumed_.load(std::memory_order_acquire);
    const auto remaining = next - consumed < limit_ ? limit_ - (next - consumed) : 0;
    const auto count = std::min<std::uint64_t>(remaining, frames.size());
    for (std::uint64_t i = 0; i < count; ++i) frames_[(next + i) % frames_.size()] = frames[i];
    next_.store(next + count, std::memory_order_release);
}

void MediaAudioBuffer::clear() noexcept
{
    discardBefore_.store(next_.load(std::memory_order_relaxed), std::memory_order_release);
}

void MediaAudioBuffer::setPlaying(bool playing) noexcept
{
    playing_.store(playing, std::memory_order_relaxed);
}

bool MediaAudioBuffer::empty() const noexcept
{
    return next_.load(std::memory_order_relaxed) <=
        std::max(discardBefore_.load(std::memory_order_relaxed), consumed_.load(std::memory_order_acquire));
}

MediaAudioBuffer::Frame MediaAudioBuffer::read() noexcept
{
    auto cursor = std::max(consumed_.load(std::memory_order_relaxed), discardBefore_.load(std::memory_order_acquire));
    Frame frame{};
    if (playing_.load(std::memory_order_relaxed) && cursor < next_.load(std::memory_order_acquire)) {
        frame = frames_[cursor++ % frames_.size()];
    }
    consumed_.store(cursor, std::memory_order_release);
    return frame;
}

std::shared_ptr<MediaAudioBuffer> Schematic::mediaAudio(const SchematicNode& entry) const
{
    if (!entry.mediaSamples) entry.mediaSamples = std::make_shared<MediaAudioBuffer>();
    return entry.mediaSamples;
}

MediaOptions Schematic::mediaOptions(const SchematicNode& entry) const
{
    if (entry.kind != PrimitiveKind::VideoStreamIn) return {};
    return {std::clamp(std::get<std::int32_t>(readInput(entry.id, 1)), 1, 4096),
            std::clamp(std::get<std::int32_t>(readInput(entry.id, 2)), 1, 4096),
            std::get<bool>(readInput(entry.id, 7)), std::get<bool>(readInput(entry.id, 9))};
}

green::Value Schematic::readMedia(const SchematicNode& entry, int output) const
{
    const auto* playback = entry.media ? &*entry.media : nullptr;
    if (entry.kind == PrimitiveKind::VideoStreamIn) {
        if (output == 0) return playback ? playback->frame : yellow::Bitmap{};
        if (output == 1) return std::int32_t{playback ? playback->state : 0};
    } else if (output == 2) {
        if (playback && !playback->error.empty()) return playback->error;
        return std::to_string(playback ? playback->state : 0);
    }
    return playback ? playback->position : 0.F;
}

void Schematic::receiveMedia(SchematicNode& entry, int input)
{
    const bool video = entry.kind == PrimitiveKind::VideoStreamIn;
    if (input == 0) return;
    if (!entry.media) entry.media.emplace();
    auto& state = *entry.media;
    const auto options = mediaOptions(entry);
    const auto samples = video ? nullptr : mediaAudio(entry);

    if (input == (video ? 3 : 1)) {
        state = {};
        if (samples) {
            samples->setPlaying(false);
            samples->clear();
            samples->configure(sampleRate(), std::get<float>(readInput(entry.id, 4)));
        }
        try {
            auto source = std::get<std::string>(readInput(entry.id, 0));
            if (source.empty() || source.contains('\0')) throw std::invalid_argument("Choose a media source");
            const auto& open = rootGraph().systemInfo_.openMedia;
            if (!open) throw std::runtime_error("Media playback is unavailable");
            state.playback = open({std::move(source), sampleRate(), options, samples});
            if (!state.playback) throw std::runtime_error("Could not open the media source");
        } catch (const std::exception& error) {
            state.error = error.what();
        }
        fireOutput(entry, video ? 1 : 2);
        fireOutput(entry, video ? 2 : 3);
    } else if (input == (video ? 6 : 3)) {
        state = {};
        if (samples) {
            samples->setPlaying(false);
            samples->clear();
        }
        if (video) fireOutput(entry, 0);
        fireOutput(entry, video ? 1 : 2);
        fireOutput(entry, video ? 2 : 3);
    } else if (!video && input == 4) {
        samples->configure(sampleRate(), std::get<float>(readInput(entry.id, 4)));
    } else if (state.playback) {
        if (input == (video ? 5 : 2)) state.playback->command(MediaCommand::PausePlay, options);
        else if (video && input == 4) state.playback->command(MediaCommand::Step, options);
        else if (video && input == 8) state.playback->command(MediaCommand::Grab, options);
        else state.playback->setOptions(options);
        pollMediaPlayback(entry);
    }
}

bool Schematic::pollMediaPlayback(SchematicNode& entry)
{
    auto& state = *entry.media;
    const bool video = entry.kind == PrimitiveKind::VideoStreamIn;
    auto update = state.playback->poll();
    if (update.finished) state.playback.reset();
    const bool stateChanged = update.state != state.state || update.error != state.error;
    const bool positionChanged = update.position != state.position;
    state.state = update.state;
    state.error = std::move(update.error);
    state.position = update.position;
    const bool frame = video && !update.frame.empty();
    if (frame) {
        state.frame = std::move(update.frame);
        fireOutput(entry, 0);
    }
    if (stateChanged) fireOutput(entry, video ? 1 : 2);
    if (positionChanged) fireOutput(entry, video ? 2 : 3);
    return stateChanged || positionChanged || frame;
}

bool Schematic::hasOpenMedia() const
{
    for (const auto& entry : nodes_) if (entry.media && entry.media->playback) return true;
    for (const auto& [id, child] : modules_) if (child->hasOpenMedia()) return true;
    return false;
}

bool Schematic::pollMedia()
{
    bool changed = false;
    const auto poll = [&](const auto& visit, Schematic& graph) -> void {
        for (auto& entry : graph.nodes_) {
            if (entry.media && entry.media->playback) changed |= graph.pollMediaPlayback(entry);
        }
        for (auto& [id, child] : graph.modules_) visit(visit, *child);
    };
    auto& root = rootGraph();
    poll(poll, root);
    if (changed) root.refreshConnections();
    return changed;
}

}
