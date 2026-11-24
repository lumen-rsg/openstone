#include "Schematic.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openstone {
namespace {

constexpr float maximumBlurRadius = 32;
constexpr float maximumKernelSize = 65;

float finite(float value)
{
    return std::isfinite(value) ? value : 0;
}

green::Point blurRadius(green::Point radius)
{
    return {std::clamp(finite(radius.x), 0.0F, maximumBlurRadius),
            std::clamp(finite(radius.y), 0.0F, maximumBlurRadius)};
}

float kernelSize(float size)
{
    // Each Gaussian kernel has a center tap and equally many taps on each side.
    return static_cast<float>(static_cast<int>(std::clamp(finite(size), 1.0F, maximumKernelSize)) | 1);
}

char lowerAscii(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

std::uint8_t blurChannels(std::string_view channels)
{
    constexpr std::string_view rgba = "rgba";
    if (channels.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return 0xF;
    }
    std::uint8_t mask = 0;
    for (const auto c : channels) {
        const auto index = rgba.find(lowerAscii(c));
        if (index != std::string_view::npos) {
            mask |= static_cast<std::uint8_t>(1U << index);
        }
    }
    return mask;
}

yellow::ShadowMix shadowMix(std::string_view mode)
{
    const auto first = mode.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos) {
        mode = mode.substr(first, mode.find_last_not_of(" \t\r\n") - first + 1);
    }
    if (std::ranges::equal(mode, std::string_view{"add"}, {}, lowerAscii)) {
        return yellow::ShadowMix::Add;
    }
    if (std::ranges::equal(mode, std::string_view{"sub"}, {}, lowerAscii)) {
        return yellow::ShadowMix::Subtract;
    }
    return yellow::ShadowMix::Interpolate;
}

}

yellow::ViewEffect Schematic::readViewEffect(const SchematicNode& entry) const
{
    const auto id = entry.id;
    const auto point = [&](int index) { return std::get<green::Point>(readInput(id, index)); };
    const auto scalar = [&](int index) { return finite(std::get<float>(readInput(id, index))); };
    const auto text = [&](int index) { return std::get<std::string>(readInput(id, index)); };
    const auto color = [&](int index) { return std::get<yellow::Color>(readInput(id, index)); };
    const auto toggle = [&](int index) { return std::get<bool>(readInput(id, index)); };

    switch (entry.kind) {
    case PrimitiveKind::ViewBlur: {
        const auto size = point(1);
        const auto sigma = point(2);
        return yellow::ViewBlur{{kernelSize(size.x), kernelSize(size.y)},
                               {std::max(0.0F, finite(sigma.x)), std::max(0.0F, finite(sigma.y))},
                               blurChannels(text(3))};
    }
    case PrimitiveKind::ViewShadow: {
        const auto distance = point(1);
        const auto hasColor = std::ranges::any_of(links_, [this, id](const auto& link) {
            return link.target == id && link.input == 4 &&
                   outputType(link.source, link.output).value_or(green::Type::Trigger) != green::Type::Trigger;
        });
        return yellow::ViewShadow{{finite(distance.x), finite(distance.y)}, blurRadius(point(2)),
                                  std::clamp(scalar(3), 0.0F, 1.0F),
                                  hasColor ? std::optional{color(4)} : std::nullopt, shadowMix(text(5)), toggle(6)};
    }
    case PrimitiveKind::ViewGlow:
        return yellow::ViewGlow{blurRadius(point(1)), color(2), std::max(0.0F, scalar(3)), toggle(4)};
    case PrimitiveKind::ViewBloom:
        return yellow::ViewBloom{blurRadius(point(1)), std::clamp(scalar(2), 0.0F, 1.0F), std::max(0.0F, scalar(3))};
    case PrimitiveKind::ViewShader:
        return yellow::ViewShader{text(1), scalar(2), scalar(3)};
    default:
        throw std::logic_error("Invalid View effect primitive");
    }
}

}
