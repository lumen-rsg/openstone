#pragma once

#include "stream/RealtimeValue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <span>
#include <cstdint>
#include <memory>
#include <vector>

namespace openstone::stream {

enum class SequenceMode : unsigned { Stopped, Recording, Playing };

inline constexpr double maxSequenceSeconds = 60;
inline constexpr std::size_t maxSequenceFrames = 11'520'000;
inline constexpr std::size_t maxSequenceMidiEvents = 262'144;

// One transport shared by all tracks in the prepared audio patch. The control
// thread posts commands and the audio thread applies them between host buffers.
class SequenceTransport {
public:
    void setMode(SequenceMode mode) noexcept
    {
        requested_.store(nextCommand(mode), std::memory_order_release);
    }

    [[nodiscard]] std::uint64_t nextCommand(SequenceMode mode) const noexcept
    {
        const auto revision = (requested_.load(std::memory_order_relaxed) & ~mask) + mask + 1;
        return revision | static_cast<unsigned>(mode);
    }

    [[nodiscard]] std::uint64_t requestedCommand() const noexcept
    {
        return requested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] SequenceMode requestedMode() const noexcept
    {
        return modeOf(requested_.load(std::memory_order_acquire));
    }

    void beginBuffer() noexcept
    {
        const auto next = requested_.load(std::memory_order_acquire);
        if (active_ != next) {
            completedCommand_.store(mask, std::memory_order_release);
            completed_.store(0, std::memory_order_release);
            completedCommand_.store(next, std::memory_order_release);
            active_ = next;
            cursor_ = 0;
        }
    }

    [[nodiscard]] std::size_t completed() const noexcept
    {
        const auto command = requested_.load(std::memory_order_acquire);
        if (completedCommand_.load(std::memory_order_acquire) != command) return 0;
        const auto samples = completed_.load(std::memory_order_relaxed);
        return completedCommand_.load(std::memory_order_acquire) == command ? samples : 0;
    }

    void advance(std::size_t samples) noexcept
    {
        if (mode() != SequenceMode::Stopped) cursor_ += samples;
        completed_.store(cursor_, std::memory_order_release);
    }

    [[nodiscard]] SequenceMode mode() const noexcept { return modeOf(active_); }
    [[nodiscard]] std::uint64_t revision() const noexcept { return active_; }
    [[nodiscard]] std::size_t position() const noexcept { return cursor_; }

private:
    static constexpr std::uint64_t mask = 3;
    static SequenceMode modeOf(std::uint64_t value) noexcept { return static_cast<SequenceMode>(value & mask); }
    std::atomic<std::uint64_t> requested_{};
    std::atomic<std::uint64_t> completedCommand_{};
    std::atomic<std::size_t> completed_{};
    std::uint64_t active_{};
    std::size_t cursor_{};
};

class SequenceAudioTrack {
public:
    using Frame = std::array<float, 4>;
    struct TakeView {
        double sampleRate;
        std::span<const Frame> samples;
    };

    void prepareTake(double sampleRate, std::uint64_t recordingCommand)
    {
        auto take = std::make_shared<Take>();
        take->sampleRate = sampleRate;
        take->recordingCommand = recordingCommand;
        take->samples.resize(static_cast<std::size_t>(
            std::min(sampleRate * maxSequenceSeconds, static_cast<double>(maxSequenceFrames))));
        publish(std::move(take));
    }

    void restoreTake(double sampleRate, std::vector<Frame> samples)
    {
        auto take = std::make_shared<Take>();
        take->sampleRate = sampleRate;
        take->samples = std::move(samples);
        take->recorded.store(take->samples.size(), std::memory_order_relaxed);
        publish(std::move(take));
    }

    // Call on the control thread. The committed frames are immutable and the
    // view stays valid until the next prepareTake or restoreTake.
    [[nodiscard]] TakeView take() const noexcept
    {
        if (!current_) return {};
        return {current_->sampleRate, std::span{current_->samples}.first(current_->recorded.load(std::memory_order_acquire))};
    }

    void latch(const SequenceTransport& transport) noexcept
    {
        if (!latched_ || revision_ != transport.revision()) {
            take_ = takes_.read().get();
            revision_ = transport.revision();
            latched_ = true;
        }
    }

    [[nodiscard]] float process(const SequenceTransport& transport, std::size_t position, std::size_t lane,
                                float input, double outputRate) noexcept
    {
        if (transport.mode() == SequenceMode::Playing) {
            return playback(position, lane, outputRate);
        }
        if (transport.mode() == SequenceMode::Recording && take_ &&
            take_->recordingCommand == transport.revision() && position < take_->samples.size()) {
            if (position != take_->writingFrame) {
                take_->writingFrame = position;
                take_->writtenLanes = 0;
            }
            take_->samples[position][lane] = input;
            take_->writtenLanes |= 1u << lane;
            if (take_->writtenLanes == (1u << Frame{}.size()) - 1) {
                take_->recorded.store(position + 1, std::memory_order_release);
            }
        }
        return input;
    }

private:
    struct Take {
        double sampleRate = 0;
        std::uint64_t recordingCommand = 0;
        std::vector<Frame> samples;
        std::atomic<std::size_t> recorded{};
        std::size_t writingFrame{};
        unsigned writtenLanes{};
    };

    void publish(std::shared_ptr<Take> take)
    {
        current_ = std::move(take);
        takes_.writable() = current_;
        takes_.publish();
    }

    [[nodiscard]] float playback(std::size_t position, std::size_t lane, double outputRate) const noexcept
    {
        if (!take_ || outputRate <= 0) return 0.F;
        const auto count = take_->recorded.load(std::memory_order_relaxed);
        const auto frame = take_->sampleRate == outputRate ? static_cast<double>(position)
            : static_cast<double>(position) * take_->sampleRate / outputRate;
        if (frame >= count) return 0.F;
        const auto first = static_cast<std::size_t>(frame);
        const auto fraction = static_cast<float>(frame - first);
        const float value = take_->samples[first][lane];
        if (fraction == 0 || first + 1 == count) return value;
        return std::lerp(value, take_->samples[first + 1][lane], fraction);
    }

    std::shared_ptr<Take> current_; // Control-thread ownership for saving.
    RealtimeValue<std::shared_ptr<Take>> takes_;
    Take* take_{};
    std::uint64_t revision_{};
    bool latched_ = false;
};

}
