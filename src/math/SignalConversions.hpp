#pragma once

#include <cmath>
#include <concepts>

namespace openstone::math {

enum class PitchConversion { FrequencyToHz, FrequencyToPitch, HzToFrequency, HzToPitch, PitchToFrequency, PitchToHz };

template<std::floating_point Number>
Number convertPitch(PitchConversion operation, Number value, Number sampleRate) noexcept
{
    constexpr Number a4{440};
    constexpr Number octave{12};
    // FlowStone's normalized-frequency primitives count pitch an octave below MIDI note numbers.
    constexpr Number normalizedA4{57};
    constexpr Number midiA4{69};
    switch (operation) {
    case PitchConversion::FrequencyToHz: return value * (sampleRate / Number{2});
    case PitchConversion::FrequencyToPitch:
        return value <= Number{} ? Number{} : normalizedA4 + octave * std::log2(value * sampleRate / a4);
    case PitchConversion::HzToFrequency: return value * Number{2} / sampleRate;
    case PitchConversion::HzToPitch:
        return value <= Number{} ? Number{} : midiA4 + octave * std::log2(value / a4);
    case PitchConversion::PitchToFrequency: return a4 * std::exp2((value - normalizedA4) / octave) / sampleRate;
    case PitchConversion::PitchToHz: return a4 * std::exp2((value - midiA4) / octave);
    }
    return value;
}

}
