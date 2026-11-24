#include "stream/Oscillator.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <numbers>

namespace openstone::stream {
namespace {

constexpr std::size_t tableSize = 2048;
constexpr std::size_t maxHarmonics = 1023;

template<std::floating_point Sample>
using Wave = std::array<Sample, tableSize>;

template<std::floating_point Sample, bool triangle>
const std::vector<Wave<Sample>>& wavetableBank()
{
    // Build once, during processor preparation. The whole bank shares one gain
    // so changing the harmonic count doesn't renormalize each waveform.
    static const auto tables = [] {
        Wave<Sample> sine{};
        for (std::size_t i = 0; i < tableSize; ++i) {
            sine[i] = static_cast<Sample>(std::sin(2 * std::numbers::pi * static_cast<double>(i) / tableSize));
        }

        // Each triangle level adds an odd harmonic. Even limits reuse the
        // preceding level since they contain exactly the same harmonics.
        constexpr std::size_t levels = triangle ? (maxHarmonics + 1) / 2 : maxHarmonics;
        std::vector<Wave<Sample>> waves(levels + 1);
        Sample peak{};
        for (std::size_t level = 1; level <= levels; ++level) {
            const auto harmonic = triangle ? level * 2 - 1 : level;
            const auto gain = triangle
                ? (level % 2 == 1 ? Sample{1} : Sample{-1}) / static_cast<Sample>(harmonic * harmonic)
                : Sample{1} / static_cast<Sample>(harmonic);
            for (std::size_t i = 0; i < tableSize; ++i) {
                const auto value = waves[level - 1][i] + gain * sine[(i * harmonic) % tableSize];
                waves[level][i] = value;
                peak = std::max(peak, std::abs(value));
            }
        }

        const auto scale = Sample{1} / peak;
        for (auto& wave : waves) {
            for (auto& value : wave) value *= scale;
        }
        return waves;
    }();
    return tables;
}

template<std::floating_point Sample>
class Oscillator final : public SampleProcessor {
public:
    explicit Oscillator(OscillatorWaveform waveform)
        : triangle_(waveform == OscillatorWaveform::Triangle),
          tables_(waveform == OscillatorWaveform::Sine ? nullptr
              : triangle_ ? &wavetableBank<Sample, true>() : &wavetableBank<Sample, false>())
    {}

    void reset() noexcept override
    {
        phase_ = 0;
        previousSync_ = false;
    }

    double process(std::span<const double> inputs) noexcept override
    {
        const bool sync = inputs[2] != 0;
        if (sync && !previousSync_) phase_ = 0;
        previousSync_ = sync;

        const auto frequency = finiteSample(inputs[0]);
        const auto offset = finiteSample(inputs[1]);
        const auto outputPhase = wrap(phase_ + wrap(offset));
        phase_ = wrap(phase_ + frequency * Sample{0.5});

        if (!tables_) return static_cast<Sample>(std::sin(2 * std::numbers::pi * outputPhase));

        const auto magnitude = std::abs(frequency);
        constexpr auto minimumFrequency = Sample{1} / static_cast<Sample>(maxHarmonics);
        const auto harmonics = magnitude <= minimumFrequency ? maxHarmonics
            : static_cast<std::size_t>(Sample{1} / magnitude);
        const auto level = triangle_ ? (harmonics + 1) / 2 : harmonics;
        const auto& wave = (*tables_)[level];
        const auto position = outputPhase * static_cast<Sample>(tableSize);
        const auto index = static_cast<std::size_t>(position);
        const auto fraction = position - static_cast<Sample>(index);
        return std::lerp(wave[index % tableSize], wave[(index + 1) % tableSize], fraction);
    }

private:
    static Sample finiteSample(double input) noexcept
    {
        const auto sample = static_cast<Sample>(input);
        return std::isfinite(sample) ? sample : Sample{};
    }

    static Sample wrap(Sample phase) noexcept
    {
        return phase - std::floor(phase);
    }

    const bool triangle_;
    const std::vector<Wave<Sample>>* const tables_;
    Sample phase_{};
    bool previousSync_ = false;
};

}

std::unique_ptr<SampleProcessor> makeOscillator(OscillatorWaveform waveform, SamplePrecision precision)
{
    if (precision == SamplePrecision::Float64) return std::make_unique<Oscillator<double>>(waveform);
    return std::make_unique<Oscillator<float>>(waveform);
}

}
