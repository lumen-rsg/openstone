#include "Schematic.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openstone {

green::Value Schematic::brushInputDefault(const SchematicNode& entry, int input) const
{
    const auto& tool = std::get<yellow::PaintTool>(*entry.localValue);
    switch (entry.kind) {
    case PrimitiveKind::LinearGradientBrush: {
        const auto& brush = std::get<yellow::LinearGradientBrush>(tool);
        if (input == 0) return brush.start;
        if (input == 1) return brush.end;
        if (input == 2) return brush.area;
        if (input == 3) return brush.angle;
        return std::string(yellow::brushWrapName(brush.wrap));
    }
    case PrimitiveKind::RadialGradientBrush: {
        const auto& brush = std::get<yellow::RadialGradientBrush>(tool);
        if (input == 0) return brush.start;
        if (input == 1) return brush.end;
        if (input == 2) return brush.area;
        if (input == 3) return std::string(brush.shape == yellow::RadialShape::Rectangle ? "rectangle" : "ellipse");
        if (input == 4) return brush.center ? brush.center->x : brush.area.x + brush.area.width * .5f;
        if (input == 5) return brush.center ? brush.center->y : brush.area.y + brush.area.height * .5f;
        return input == 6 ? brush.focus.x : brush.focus.y;
    }
    case PrimitiveKind::HatchBrush: {
        const auto& brush = std::get<yellow::HatchBrush>(tool);
        if (input == 0) return brush.foreground;
        if (input == 1) return brush.background;
        return std::string(yellow::hatchStyleName(brush.style));
    }
    case PrimitiveKind::TextureBrush: {
        const auto& brush = std::get<yellow::TextureBrush>(tool);
        if (input == 0) return brush.bitmap;
        return std::string(yellow::brushWrapName(brush.wrap));
    }
    default: throw std::logic_error("Invalid stored brush primitive");
    }
}

yellow::PaintTool Schematic::readBrush(const SchematicNode& entry) const
{
    const auto id = entry.id;
    const auto color = [&](int input) { return std::get<yellow::Color>(readInput(id, input)); };
    const auto text = [&](int input) { return std::get<std::string>(readInput(id, input)); };
    const auto number = [&](int input) { return std::get<float>(readInput(id, input)); };
    const auto area = [&] { return std::get<yellow::Area>(readInput(id, 2)); };

    switch (entry.kind) {
    case PrimitiveKind::LinearGradientBrush: {
        const float angle = number(3);
        return yellow::LinearGradientBrush{color(0), color(1), area(),
            std::isfinite(angle) ? std::fmod(angle, 360.0F) : 0.0F, yellow::parseBrushWrap(text(4))};
    }
    case PrimitiveKind::RadialGradientBrush: {
        const auto focus = [&](int input) {
            const float scale = number(input);
            return std::isfinite(scale) ? std::clamp(scale, 0.0F, 1.0F) : 0.0F;
        };
        std::optional<green::Point> center;
        if (entry.localValue) center = std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(*entry.localValue)).center;
        if (center || std::ranges::any_of(links_, [id](auto link) {
                return link.target == id && (link.input == 4 || link.input == 5);
            })) {
            const green::Point point{number(4), number(5)};
            if (std::isfinite(point.x) && std::isfinite(point.y)) {
                center = point;
            }
        }
        yellow::RadialGradientBrush result{color(0), color(1), area(), yellow::parseRadialShape(text(3)),
                                           center, {focus(6), focus(7)}};
        if (entry.localValue) {
            const auto& stored = std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(*entry.localValue));
            const auto linked = [&](int input) { return std::ranges::any_of(links_, [=](auto link) { return link.target == id && link.input == input; }); };
            if (!linked(3)) {
                result.boundary = stored.boundary;
                result.surroundColors = stored.surroundColors;
                if (result.boundary && result.area != stored.area && stored.area.width != 0 && stored.area.height != 0) {
                    for (auto& p : result.boundary->points) {
                        p.x = result.area.x + (p.x - stored.area.x) * result.area.width / stored.area.width;
                        p.y = result.area.y + (p.y - stored.area.y) * result.area.height / stored.area.height;
                    }
                }
                if (linked(1)) std::fill(result.surroundColors.begin(), result.surroundColors.end(), result.end);
            }
        }
        return result;
    }
    case PrimitiveKind::HatchBrush:
        return yellow::HatchBrush{color(0), color(1), yellow::parseHatchStyle(text(2))};
    case PrimitiveKind::TextureBrush:
        return yellow::TextureBrush{std::get<yellow::Bitmap>(readInput(id, 0)).copy(), yellow::parseBrushWrap(text(1))};
    default:
        throw std::logic_error("Invalid brush primitive");
    }
}

}
