#pragma once

#include "stream/Runtime.hpp"

namespace openstone::stream {

// Processor inputs are target, time in milliseconds, sample rate, and reset trigger count.
[[nodiscard]] std::unique_ptr<SampleProcessor> makeStreamDeZipper();
[[nodiscard]] std::unique_ptr<SampleProcessor> makeFloatDeZipper();
// Glide uses the same stream ramp with inputs for the target and length in samples.
[[nodiscard]] std::unique_ptr<SampleProcessor> makeGlide();

}
