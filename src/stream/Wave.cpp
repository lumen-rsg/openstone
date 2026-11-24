#include "stream/Wave.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace openstone::stream {
namespace {

std::size_t roundedIndex(double value, std::size_t length) noexcept
{
    const auto index = std::nearbyint(static_cast<float>(value));
    return static_cast<std::size_t>(index > 0 ? std::min<double>(index, length - 1) : 0);
}

double phaseWrap(double phase) noexcept
{
    return std::isfinite(phase) ? phase - std::floor(phase) : 0;
}

class WaveProcessor final : public SampleProcessor {
public:
    WaveProcessor(WaveOperation operation, std::shared_ptr<WaveBuffer> buffer, int channel)
        : operation_(operation), buffer_(std::move(buffer)), channel_(channel) {}

    void reset() noexcept override
    {
        frame_ = 0;
        phase_ = 0;
        previousSync_ = false;
        held_ = 0;
    }

    double process(std::span<const double> inputs) noexcept override
    {
        switch (operation_) {
        case WaveOperation::Read: return read(std::get<green::Memory>(buffer_->read()), inputs[0]);
        case WaveOperation::ReadHop: {
            const auto exponent = inputs[1] >= 0 ? std::min(inputs[1], 12.0) : 0.0;
            const auto hop = 1u << static_cast<unsigned>(exponent);
            if (frame_++ % hop == 0) held_ = read(std::get<green::Memory>(buffer_->read()), inputs[0]);
            return held_;
        }
        case WaveOperation::ReadArray: {
            const auto entries = std::get<green::MemoryArray>(buffer_->read()).entries();
            return entries.empty() ? 0 : read(entries[roundedIndex(inputs[0], entries.size())], inputs[1]);
        }
        case WaveOperation::ReadTable: return readTable(inputs);
        case WaveOperation::WriteMono: case WaveOperation::WriteStereo: return write(inputs);
        }
        return 0;
    }

private:
    double read(const green::Memory& memory, double index) const noexcept
    {
        const auto stride = memory.channels();
        if (!stride) return 0;
        const auto frames = memory.size() / sizeof(float) / stride;
        if (!frames) return 0;
        return memory.sample(roundedIndex(index, frames) * stride + std::min(channel_, stride - 1));
    }

    double readTable(std::span<const double> inputs) noexcept
    {
        const auto& table = std::get<green::Memory>(buffer_->read());
        const auto size = table.size() / sizeof(float) / waveTableBands;
        const bool sync = inputs[2] != 0;
        if (sync && !previousSync_) phase_ = 0;
        previousSync_ = sync;
        const double frequency = std::isfinite(inputs[0]) ? inputs[0] : 0;
        const double position = phaseWrap(phase_ + inputs[1]) * size;
        phase_ = phaseWrap(phase_ + frequency * 0.5);
        if (size < 2 || size > maxWavePeriod || !std::has_single_bit(size) ||
            table.size() != size * waveTableBands * sizeof(float)) return 0;
        const auto band = frequency == 0 ? waveTableBands - 1
            : static_cast<std::size_t>(std::clamp(1 / std::abs(frequency), 1.0, static_cast<double>(waveTableBands))) - 1;
        const auto frame = static_cast<std::size_t>(position) % size;
        const auto next = (frame + 1) % size;
        return std::lerp(static_cast<double>(table.sample(band * size + frame)),
                         static_cast<double>(table.sample(band * size + next)), position - std::floor(position));
    }

    double write(std::span<const double> inputs) noexcept
    {
        const bool stereo = operation_ == WaveOperation::WriteStereo;
        const auto stride = stereo ? 2 : 1;
        const auto& memory = std::get<green::Memory>(buffer_->read());
        const auto count = memory.size() / sizeof(float) / stride;
        const auto index = std::nearbyint(static_cast<float>(inputs[0]));
        if (count && index < static_cast<double>(count)) {
            const auto first = roundedIndex(index, count) * stride;
            memory.writeSample(first, static_cast<float>(inputs[1]));
            if (stereo) memory.writeSample(first + 1, static_cast<float>(inputs[2]));
        }
        return static_cast<float>(inputs[1]);
    }

    WaveOperation operation_;
    std::shared_ptr<WaveBuffer> buffer_;
    int channel_;
    std::uint64_t frame_{};
    double phase_{};
    bool previousSync_{};
    double held_{};
};

class WavePassthrough final : public SampleProcessor {
public:
    // The first input ensures the writer runs before its right output is read.
    double process(std::span<const double> inputs) noexcept override { return static_cast<float>(inputs[1]); }
};

}

std::unique_ptr<SampleProcessor> makeWaveProcessor(WaveOperation operation, std::shared_ptr<WaveBuffer> buffer, int channel)
{
    return std::make_unique<WaveProcessor>(operation, std::move(buffer), channel);
}

std::unique_ptr<SampleProcessor> makeWavePassthrough()
{
    return std::make_unique<WavePassthrough>();
}

}
