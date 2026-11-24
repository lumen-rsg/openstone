#pragma once

#include "green/Value.hpp"

#include <array>
#include <span>

namespace openstone::green {

using Spectrum = std::array<FloatArray, 2>;

// Shorter components are zero-padded. The inverse divides by the transform length.
[[nodiscard]] Spectrum fourierTransform(std::span<const float> real, std::span<const float> imaginary, bool inverse);

}
