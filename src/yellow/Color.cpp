#include "yellow/Value.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openstone::yellow {
namespace {

constexpr std::int32_t fullTurn = 360;
constexpr std::uint32_t byteMask = 0xFF;

std::uint32_t channelByte(std::int32_t value) noexcept
{
    return static_cast<std::uint32_t>(value) & byteMask;
}

std::int32_t truncateChannel(float channel) noexcept
{
    if (static_cast<double>(channel) < std::numeric_limits<std::int32_t>::min() ||
        static_cast<double>(channel) > std::numeric_limits<std::int32_t>::max()) {
        return 0;
    }
    return static_cast<std::int32_t>(channel);
}

}

ColorChannels colorToArgb(Color color) noexcept
{
    const auto channel = [color](int shift) {
        return static_cast<std::int32_t>((color.argb >> shift) & byteMask);
    };
    return {channel(24), channel(16), channel(8), channel(0)};
}

Color colorFromArgb(ColorChannels channels) noexcept
{
    const auto [alpha, red, green, blue] = channels;
    return {(channelByte(alpha) << 24) | (channelByte(red) << 16) |
            (channelByte(green) << 8) | channelByte(blue)};
}

ColorChannels colorToAhsv(Color color) noexcept
{
    const auto [alpha, red, green, blue] = colorToArgb(color);
    const double r = red / 255.0;
    const double g = green / 255.0;
    const double b = blue / 255.0;
    const auto peak = std::max({r, g, b});
    const auto range = peak - std::min({r, g, b});
    const auto saturation = peak > 0 ? range / peak : 0;
    double hue = 0;
    if (range > 0) {
        if (peak == r && peak != g) {
            hue = (g - b) / range;
        } else if (peak == g && peak != b) {
            hue = 2 + (b - r) / range;
        } else {
            hue = 4 + (r - g) / range;
        }
        hue *= 60;
        if (hue < 0) {
            hue += fullTurn;
        }
    }
    return {alpha, static_cast<std::int32_t>(std::lround(hue)),
            static_cast<std::int32_t>(std::lround(saturation * 255)),
            static_cast<std::int32_t>(std::lround(peak * 255))};
}

Color colorFromAhsv(ColorChannels channels) noexcept
{
    const auto [alpha, hue, saturation, value] = channels;
    const auto angle = static_cast<float>((hue % fullTurn + fullTurn) % fullTurn);
    const auto sat = static_cast<float>(saturation) / 255.0F;
    const auto val = static_cast<float>(value) / 255.0F;
    const auto toByte = [&](float weight) {
        const float chroma = std::min(weight, 1.0F) * sat;
        const float intensity = (chroma + (1.0F - sat)) * val;
        return truncateChannel(intensity * 255.0F);
    };

    float red = 0;
    float green = 0;
    float blue = 0;
    if (angle < 120) {
        red = (120 - angle) / 60;
        green = angle / 60;
    } else if (angle < 240) {
        green = (240 - angle) / 60;
        blue = (angle - 120) / 60;
    } else {
        blue = (360 - angle) / 60;
        red = (angle - 240) / 60;
    }
    return colorFromArgb({alpha, toByte(red), toByte(green), toByte(blue)});
}

}
