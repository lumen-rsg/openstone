#include "Schematic.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace openstone {
namespace {

bool hasExtent(const yellow::Area& area, PrimitiveKind kind)
{
    if (!std::isfinite(area.x) || !std::isfinite(area.y) ||
        !std::isfinite(area.width) || !std::isfinite(area.height)) {
        return false;
    }
    if (kind == PrimitiveKind::Line) {
        return area.width != 0 || area.height != 0;
    }
    return area.width != 0 && area.height != 0;
}

bool fitsPlotGeometry(double coordinate)
{
    // Leave room for the renderer to scale the coordinate up from grid units.
    return std::isfinite(coordinate) && std::abs(coordinate) <= std::numeric_limits<float>::max() / 256.0;
}

bool validPlotRange(double first, double last)
{
    return std::isfinite(first) && std::isfinite(last) && first != last;
}

bool samePosition(const yellow::GraphVertex& first, const yellow::GraphVertex& second)
{
    return first.x == second.x && first.y == second.y;
}

// Keep the source-to-destination mapping when an edge falls outside the bitmap.
bool clipBitmapAxis(float& source, float& extent, float& dest, float& destExtent, int pixels)
{
    const double start = std::min<double>(source, static_cast<double>(source) + extent);
    const double length = std::abs(static_cast<double>(extent));
    const double first = std::max(0.0, start);
    const double last = std::min<double>(pixels, start + length);
    if (first >= last) {
        return false;
    }
    const double destStart = std::min<double>(dest, static_cast<double>(dest) + destExtent);
    const double scale = std::abs(static_cast<double>(destExtent)) / length;
    dest = static_cast<float>(destStart + (first - start) * scale);
    destExtent = static_cast<float>((last - first) * scale);
    source = static_cast<float>(first);
    extent = static_cast<float>(last - first);
    return destExtent > 0;
}

}

namespace {

std::optional<float> matrixEntry(std::string_view field)
{
    constexpr std::string_view spaces = " \r\n\t\f\v";
    const auto start = field.find_first_not_of(spaces);
    if (start == field.npos) return std::nullopt;
    field = field.substr(start, field.find_last_not_of(spaces) - start + 1);
    if (field.starts_with('+')) {
        field.remove_prefix(1);
        if (field.empty() || field.starts_with('-')) return std::nullopt;
    }
    float value;
    const auto end = field.data() + field.size();
    const auto result = std::from_chars(field.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<yellow::ColorMatrix> parseMatrix(std::string_view text)
{
    yellow::ColorMatrix matrix;
    std::size_t index = 0;
    constexpr auto count = yellow::ColorMatrix::size * yellow::ColorMatrix::size;
    for (auto& row : matrix.coefficients) {
        for (auto& coefficient : row) {
            const auto comma = text.find(',');
            const auto value = matrixEntry(text.substr(0, comma));
            if (!value || (++index < count) != (comma != text.npos)) return std::nullopt;
            coefficient = *value;
            if (comma != text.npos) text.remove_prefix(comma + 1);
        }
    }
    return matrix;
}

}

green::Value Schematic::readParsedColorMatrix(const SchematicNode& entry, int output) const
{
    if (output == 1) return !entry.capturedSignals.empty() && std::get<bool>(entry.capturedSignals.at(1));
    if (entry.capturedSignals.empty()) return yellow::ColorMatrix{};
    return entry.capturedSignals.front();
}

void Schematic::receiveColorMatrix(SchematicNode& entry)
{
    const auto matrix = parseMatrix(std::get<std::string>(readInput(entry.id, 0)));
    if (entry.capturedSignals.empty()) entry.capturedSignals = {yellow::ColorMatrix{}, false};
    entry.capturedSignals[1] = matrix.has_value();
    if (matrix) {
        entry.capturedSignals[0] = *matrix;
        fireOutput(entry, 0);
    }
    fireOutput(entry, 1);
}

std::optional<yellow::DrawCommand> Schematic::readShape(const SchematicNode& entry, yellow::Area area) const
{
    using enum yellow::ShapeKind;
    yellow::DrawCommand command{.area = area, .tool = std::get<yellow::PaintTool>(readInput(entry.id, 2))};
    switch (entry.kind) {
    case PrimitiveKind::Arc: command.shape = Arc; break;
    case PrimitiveKind::Ellipse: command.shape = Ellipse; break;
    case PrimitiveKind::Line: command.shape = Line; break;
    case PrimitiveKind::Pie: command.shape = Pie; break;
    case PrimitiveKind::Rect:
    case PrimitiveKind::RoundRectangle: command.shape = Rect; break;
    default: return std::nullopt;
    }
    if (command.shape == Line &&
        !std::holds_alternative<yellow::PenStroke>(command.tool)) {
        return std::nullopt;
    }
    if (command.shape == Arc || command.shape == Pie) {
        const float start = std::get<float>(readInput(entry.id, 3));
        const float sweep = std::get<float>(readInput(entry.id, 4));
        if (!std::isfinite(start) || !std::isfinite(sweep) || sweep == 0) {
            return std::nullopt;
        }
        command.startAngle = std::fmod(start, 360.0F);
        command.sweepAngle = std::clamp(sweep, -360.0F, 360.0F);
    }
    if (command.shape == Rect) {
        const float radius = entry.kind == PrimitiveKind::RoundRectangle && !hasValueInput(entry.id, 3)
            ? 1.0F : std::get<float>(readInput(entry.id, 3));
        const float limit = std::min(std::abs(area.width), std::abs(area.height)) / 2;
        command.cornerRadius = std::isnan(radius) ? 0 : std::clamp(radius, 0.0F, limit);
    }
    return command;
}

std::optional<yellow::BitmapDrawCommand> Schematic::readBitmapDrawing(const SchematicNode& entry) const
{
    using enum PrimitiveKind;
    const bool hasCrop = entry.kind == BitmapArea;
    const auto id = entry.id;
    yellow::BitmapDrawCommand command;
    command.bitmap = std::get<yellow::Bitmap>(readInput(id, hasCrop ? 1 : 2));
    if (command.bitmap.empty()) {
        return std::nullopt;
    }
    command.area = std::get<yellow::Area>(readInput(id, hasCrop ? 3 : 1));
    if (!hasExtent(command.area, entry.kind)) {
        return std::nullopt;
    }

    const int width = command.bitmap.width();
    const int height = command.bitmap.height();
    command.sourcePixels = hasCrop ? std::get<yellow::Area>(readInput(id, 2))
                                    : yellow::Area{0, 0, static_cast<float>(width), static_cast<float>(height)};
    auto& src = command.sourcePixels;
    auto& dest = command.area;
    if (!hasExtent(src, entry.kind) ||
        !clipBitmapAxis(src.x, src.width, dest.x, dest.width, width) ||
        !clipBitmapAxis(src.y, src.height, dest.y, dest.height, height)) {
        return std::nullopt;
    }

    if (entry.kind == BitmapDraw) {
        command.alpha = static_cast<std::uint8_t>(std::clamp(std::get<std::int32_t>(readInput(id, 3)), 0, 255));
    } else {
        command.matrix = std::get<yellow::ColorMatrix>(readInput(id, hasCrop ? 4 : 3));
    }
    command.bitmap = command.bitmap.copy();
    return command;
}

std::optional<yellow::GraphDrawCommand> Schematic::readGraphDrawing(const SchematicNode& entry,
                                                                   yellow::Area area) const
{
    // The plot uses a top-left origin, like other bounded drawing commands.
    if (area.width < 0) {
        area.x += area.width;
        area.width = -area.width;
    }
    if (area.height < 0) {
        area.y += area.height;
        area.height = -area.height;
    }
    if (!std::isfinite(area.x) || !std::isfinite(area.y)) {
        return std::nullopt;
    }
    if (isPointPlot(entry.kind)) {
        return readPointPlot(entry, area);
    }

    const auto id = entry.id;
    const auto top = static_cast<double>(std::get<float>(readInput(id, 4)));
    const auto bottom = static_cast<double>(std::get<float>(readInput(id, 5)));
    if (!validPlotRange(top, bottom)) {
        return std::nullopt;
    }
    const auto values = std::get<green::FloatArray>(readInput(id, 3));
    if (values.empty()) {
        return std::nullopt;
    }
    const bool dots = entry.kind == PrimitiveKind::GraphDots;
    auto tool = std::get<yellow::PaintTool>(readInput(id, 2));
    if (!dots && !std::holds_alternative<yellow::PenStroke>(tool)) {
        return std::nullopt;
    }
    const float radius = dots ? std::get<float>(readInput(id, 7)) : 0;
    if (dots && (!std::isfinite(radius) || radius <= 0)) {
        return std::nullopt;
    }
    const auto style = dots ? yellow::GraphStyle::Dots
        : std::get<bool>(readInput(id, 7)) ? yellow::GraphStyle::Curve : yellow::GraphStyle::Lines;
    const bool logX = std::get<bool>(readInput(id, 6));
    yellow::GraphDrawCommand command{area, std::move(tool), style, radius, {}};
    command.vertices.reserve(values.size());
    const double last = static_cast<double>(values.size() - 1);
    // Log X maps positive sample positions 1..N linearly in log space.
    const double xSpan = logX ? std::log1p(last) : last;
    const double yScale = area.height / (bottom - top);
    bool startsRun = true;
    bool hasSegment = false;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double y = (values[i] - top) * yScale;
        if (!fitsPlotGeometry(y)) {
            startsRun = true;
            continue;
        }
        const double index = static_cast<double>(i);
        const double x = last == 0 ? area.width / 2.0
                        : area.width * ((logX ? std::log1p(index) : index) / xSpan);
        command.vertices.push_back({x, y, startsRun});
        hasSegment |= !startsRun;
        startsRun = false;
    }
    if (command.vertices.empty() || (!dots && !hasSegment)) {
        return std::nullopt;
    }
    return command;
}

std::optional<yellow::GraphDrawCommand> Schematic::readPointPlot(const SchematicNode& entry,
                                                                yellow::Area area) const
{
    const auto id = entry.id;
    const auto left = static_cast<double>(std::get<float>(readInput(id, 4)));
    const auto right = static_cast<double>(std::get<float>(readInput(id, 5)));
    const auto top = static_cast<double>(std::get<float>(readInput(id, 6)));
    const auto bottom = static_cast<double>(std::get<float>(readInput(id, 7)));
    if (!validPlotRange(left, right) || !validPlotRange(top, bottom)) {
        return std::nullopt;
    }

    const bool dots = entry.kind == PrimitiveKind::Dots;
    const bool closed = entry.kind == PrimitiveKind::DrawPolygon;
    auto tool = std::get<yellow::PaintTool>(readInput(id, 2));
    if (!dots && !closed && !std::holds_alternative<yellow::PenStroke>(tool)) {
        return std::nullopt;
    }
    const float radius = dots ? std::get<float>(readInput(id, 8)) : 0.0F;
    if (dots && (!std::isfinite(radius) || radius <= 0)) {
        return std::nullopt;
    }
    const auto style = dots ? yellow::GraphStyle::Dots
        : std::get<bool>(readInput(id, 8)) ? yellow::GraphStyle::PointCurve : yellow::GraphStyle::Lines;

    const auto points = std::get<green::PointArray>(readInput(id, 3));
    yellow::GraphDrawCommand command{area, std::move(tool), style, radius, {}, closed};
    command.vertices.reserve(points.size());
    const double xScale = area.width / (right - left);
    const double yScale = area.height / (bottom - top);
    bool startsRun = true;
    bool hasSegment = false;
    for (const auto& point : points) {
        const double x = (point.x - left) * xScale;
        const double y = (point.y - top) * yScale;
        if (!fitsPlotGeometry(x) || !fitsPlotGeometry(y)) {
            // A missing vertex makes the polygon's boundary ambiguous.
            if (closed) {
                return std::nullopt;
            }
            startsRun = true;
            continue;
        }
        const bool beginsPath = dots || startsRun;
        const yellow::GraphVertex vertex{x, y, beginsPath};
        if (!beginsPath && samePosition(vertex, command.vertices.back())) {
            // A zero-length segment has no direction for curve interpolation.
            continue;
        }
        command.vertices.push_back(vertex);
        hasSegment |= !beginsPath;
        startsRun = false;
    }
    if (command.vertices.empty() || (!dots && !hasSegment)) {
        return std::nullopt;
    }
    if (closed) {
        auto& vertices = command.vertices;
        if (samePosition(vertices.front(), vertices.back())) {
            vertices.pop_back();
        }
        if (vertices.size() < 3 || std::ranges::none_of(vertices, [&](const auto& vertex) {
            return !samePosition(vertex, vertices[0]) && !samePosition(vertex, vertices[1]);
        })) {
            return std::nullopt;
        }
    }
    return command;
}

yellow::Area Schematic::measureText(int id) const
{
    const auto& measure = rootGraph().textMeasurer_;
    if (!measure) {
        return {};
    }
    const auto area = std::get<yellow::Area>(readInput(id, 1));
    if (!hasExtent(area, PrimitiveKind::Text) || !std::isfinite(area.x + area.width) ||
        !std::isfinite(area.y + area.height)) {
        return {};
    }
    const auto text = std::get<std::string>(readInput(id, 0));
    if (text.empty()) {
        return {};
    }
    return measure(text, area, std::get<yellow::Font>(readInput(id, 2)),
                   std::get<yellow::StringFormat>(readInput(id, 3)));
}

void Schematic::appendDrawing(const SchematicNode& entry, yellow::View view,
                              std::vector<yellow::PanelDrawing>& drawings) const
{
    if (view.mapping.fullyClipped()) {
        return;
    }
    const auto append = [&](auto command) {
        drawings.push_back({std::move(command), std::move(view.drawOrder), std::move(view.mapping)});
    };
    if (entry.kind == PrimitiveKind::BitmapArea || entry.kind == PrimitiveKind::BitmapDraw ||
        entry.kind == PrimitiveKind::BitmapDrawTransform) {
        if (auto command = readBitmapDrawing(entry)) {
            append(std::move(*command));
        }
        return;
    }
    if (primitive(entry.kind).behavior == PrimitiveBehavior::MouseArea) {
        if (auto region = readMouseRegion(entry)) {
            const yellow::PanelControlId source{path_, entry.id};
            append(yellow::MouseAreaCommand{source, std::move(*region), view.mouseMoves, {}, capturesMouseArea(source)});
        }
        return;
    }
    const auto area = std::get<yellow::Area>(readInput(entry.id, 1));
    if (!hasExtent(area, entry.kind)) {
        return;
    }
    if (entry.kind == PrimitiveKind::EditControl) {
        append(readEditControlDrawing(entry, area));
    } else if (entry.kind == PrimitiveKind::PopupList) {
        append(readPopupListDrawing(entry, area));
    } else if (entry.kind == PrimitiveKind::DropFiles) {
        const bool draggingOver = entry.fileDrag && entry.fileDrag->preview.has_value();
        append(yellow::DropFilesCommand{{path_, entry.id}, area, draggingOver});
    } else if (isPointPlot(entry.kind) || entry.kind == PrimitiveKind::GraphDots || entry.kind == PrimitiveKind::LineGraph) {
        if (auto command = readGraphDrawing(entry, area)) {
            append(std::move(*command));
        }
    } else if (entry.kind == PrimitiveKind::Text) {
        auto text = std::get<std::string>(readInput(entry.id, 3));
        if (!text.empty()) {
            yellow::TextCommand command{area, std::get<yellow::Color>(readInput(entry.id, 2)), std::move(text),
                                        std::get<yellow::Font>(readInput(entry.id, 4)),
                                        std::get<yellow::StringFormat>(readInput(entry.id, 5))};
            append(std::move(command));
        }
    } else if (auto command = readShape(entry, area)) {
        append(std::move(*command));
    }
}

}
