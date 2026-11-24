#include "green/Fourier.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace openstone::green {
namespace {

using Complex = std::complex<double>;
constexpr double twoPi = 2 * std::numbers::pi;

void radixTwo(std::span<Complex> values, bool inverse)
{
    const auto size = values.size();
    for (std::size_t i = 1, j = 0; i < size; ++i) {
        auto bit = size >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }

    for (std::size_t half = 1; half < size; half *= 2) {
        const auto width = 2 * half;
        const auto turn = std::polar(1.0, (inverse ? twoPi : -twoPi) / static_cast<double>(width));
        Complex factor{1, 0};
        for (std::size_t column = 0; column < half; ++column) {
            for (std::size_t row = column; row < size; row += width) {
                const auto left = values[row];
                const auto right = values[row + half] * factor;
                values[row] = left + right;
                values[row + half] = left - right;
            }
            factor *= turn;
        }
    }
    if (inverse) {
        for (auto& value : values) value /= static_cast<double>(size);
    }
}

std::vector<Complex> bluestein(std::span<const Complex> input)
{
    // Convert an arbitrary-length DFT to a power-of-two convolution.
    const auto size = input.size();
    if (size > std::bit_floor(std::vector<Complex>().max_size()) / 2) {
        throw std::length_error("FFT inputs are too large");
    }
    const auto convolutionSize = std::bit_ceil(2 * size - 1);
    std::vector<Complex> signal(convolutionSize);
    std::vector<Complex> kernel(convolutionSize);
    for (std::size_t i = 0; i < size; ++i) {
        const double angle = std::numbers::pi * static_cast<double>(i) / static_cast<double>(size) *
                             static_cast<double>(i);
        const auto chirp = std::polar(1.0, angle);
        signal[i] = input[i] * std::conj(chirp);
        kernel[i] = chirp;
        if (i != 0) kernel[convolutionSize - i] = chirp;
    }

    radixTwo(signal, false);
    radixTwo(kernel, false);
    for (std::size_t i = 0; i < convolutionSize; ++i) signal[i] *= kernel[i];
    radixTwo(signal, true);

    for (std::size_t i = 0; i < size; ++i) {
        const double angle = std::numbers::pi * static_cast<double>(i) / static_cast<double>(size) *
                             static_cast<double>(i);
        signal[i] *= std::polar(1.0, -angle);
    }
    signal.resize(size);
    return signal;
}

}

Spectrum fourierTransform(std::span<const float> real, std::span<const float> imaginary, bool inverse)
{
    const auto count = std::max(real.size(), imaginary.size());
    if (count == 0) return {};

    std::vector<Complex> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double re = i < real.size() ? real[i] : 0;
        const double im = i < imaginary.size() ? imaginary[i] : 0;
        values[i] = {re, im};
    }
    if (std::has_single_bit(count)) {
        radixTwo(values, inverse);
    } else {
        if (inverse) {
            for (auto& value : values) value = std::conj(value);
        }
        values = bluestein(values);
        if (inverse) {
            for (auto& value : values) value = std::conj(value) / static_cast<double>(count);
        }
    }

    Spectrum result{FloatArray(count), FloatArray(count)};
    for (std::size_t i = 0; i < count; ++i) {
        result[0][i] = static_cast<float>(values[i].real());
        result[1][i] = static_cast<float>(values[i].imag());
    }
    return result;
}

}
