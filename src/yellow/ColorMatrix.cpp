#include "yellow/Value.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace openstone::yellow {
namespace {

using Channels = std::array<std::uint8_t, 4>; // Straight R, G, B, A.

void validateColorMatrix(const ColorMatrix& matrix)
{
    for (const auto& row : matrix.coefficients) {
        for (std::size_t column = 0; column < Channels{}.size(); ++column) {
            if (!std::isfinite(row[column])) {
                throw std::invalid_argument("Color Matrix coefficients must be finite");
            }
        }
    }
}

Channels transform(const ColorMatrix& matrix, Channels channels)
{
    std::array<double, ColorMatrix::size> components{
        channels[0] / 255.0, channels[1] / 255.0, channels[2] / 255.0, channels[3] / 255.0, 1.0,
    };
    for (std::size_t column = 0; column < channels.size(); ++column) {
        double value = 0.0;
        for (std::size_t row = 0; row < components.size(); ++row) {
            value += components[row] * matrix.coefficients[row][column];
        }
        channels[column] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255));
    }
    return channels;
}

}

Color applyColorMatrix(const ColorMatrix& matrix, Color color)
{
    validateColorMatrix(matrix);
    const auto [r, g, b, a] = transform(matrix, {
        static_cast<std::uint8_t>(color.argb >> 16), static_cast<std::uint8_t>(color.argb >> 8),
        static_cast<std::uint8_t>(color.argb), static_cast<std::uint8_t>(color.argb >> 24),
    });
    return {(std::uint32_t{a} << 24) | (std::uint32_t{r} << 16) | (std::uint32_t{g} << 8) | b};
}

Bitmap applyBitmapColorMatrix(const ColorMatrix& matrix, const Bitmap& bitmap)
{
    if (bitmap.empty() || matrix == ColorMatrix{}) {
        return bitmap;
    }
    validateColorMatrix(matrix);
    const auto source = bitmap.rgba();
    std::vector<std::uint8_t> pixels(source.size());
    for (std::size_t i = 0; i < source.size(); i += 4) {
        const auto rgba = transform(matrix, {source[i], source[i + 1], source[i + 2], source[i + 3]});
        std::ranges::copy(rgba, pixels.data() + i);
    }
    return Bitmap(bitmap.width(), bitmap.height(), std::move(pixels));
}

}
