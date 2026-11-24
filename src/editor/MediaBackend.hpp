#pragma once

#include "yellow/Bitmap.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace openstone {

class MediaAudioBuffer {
public:
    using Frame = std::array<float, 2>;
    MediaAudioBuffer();

    // The media backend produces frames on the control thread. The audio thread
    // reads from the same queue without allocating or waiting for the decoder.
    void push(std::span<const Frame> frames) noexcept;
    void configure(double sampleRate, float seconds) noexcept;
    void clear() noexcept;
    void setPlaying(bool playing) noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Frame read() noexcept;

private:
    std::vector<Frame> frames_;
    std::size_t limit_;
    std::atomic<std::uint64_t> next_{};
    std::atomic<std::uint64_t> consumed_{};
    std::atomic<std::uint64_t> discardBefore_{};
    std::atomic<bool> playing_{};
};

struct MediaOptions {
    int width = 640;
    int height = 480;
    bool manual = false;
    bool mute = false;
};

struct MediaSettings {
    std::string source;
    double sampleRate;
    MediaOptions video;
    std::shared_ptr<MediaAudioBuffer> samples; // Empty for video playback.
};

enum class MediaCommand { PausePlay, Step, Grab };

struct MediaPoll {
    int state = 0;
    float position = 0;
    bool finished = false;
    std::string error;
    yellow::Bitmap frame;
};

class MediaPlayback {
public:
    virtual ~MediaPlayback() = default;
    virtual void command(MediaCommand command, const MediaOptions& options) = 0;
    virtual void setOptions(const MediaOptions& options) = 0;
    [[nodiscard]] virtual MediaPoll poll() = 0;
};

struct MediaState {
    std::shared_ptr<MediaPlayback> playback;
    int state = 0;
    float position = 0;
    std::string error;
    yellow::Bitmap frame;
};

}
