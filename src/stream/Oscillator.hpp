#pragma once

#include "stream/Runtime.hpp"

namespace openstone::stream {

enum class OscillatorWaveform { Sine, Sawtooth, Triangle };

// Processor inputs are normalized frequency, phase offset in cycles, and hard sync.
[[nodiscard]] std::unique_ptr<SampleProcessor> makeOscillator(OscillatorWaveform waveform, SamplePrecision precision);

}
