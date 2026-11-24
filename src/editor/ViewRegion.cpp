#include "ViewRegion.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace openstone::yellow {
namespace {

using green::Point;

std::array<Point, 4> corners(Area area)
{
    return {{{area.x, area.y}, {area.x + area.width, area.y},
             {area.x + area.width, area.y + area.height}, {area.x, area.y + area.height}}};
}

double edgeDistance(Point a, Point b, Point point)
{
    return (double(b.x) - a.x) * (double(point.y) - a.y) -
           (double(b.y) - a.y) * (double(point.x) - a.x);
}

bool hasArea(const ClipPolygon& polygon)
{
    if (polygon.size() < 3) {
        return false;
    }
    const auto base = polygon.front();
    double area = 0;
    for (std::size_t i = 2; i < polygon.size(); ++i) {
        area += edgeDistance(base, polygon[i - 1], polygon[i]);
    }
    return std::isfinite(area) && area > 0;
}

std::vector<ClipPolygon> asPolygons(const ClipRegion& region)
{
    if (const auto* polygons = std::get_if<std::vector<ClipPolygon>>(&region)) {
        return *polygons;
    }
    std::vector<ClipPolygon> polygons;
    for (const auto& area : std::get<std::vector<Area>>(region)) {
        const auto points = corners(area);
        polygons.emplace_back(points.begin(), points.end());
    }
    return polygons;
}

// Clip a convex polygon to one side of a directed edge.
ClipPolygon halfPlane(const ClipPolygon& subject, Point a, Point b, bool inner)
{
    ClipPolygon result;
    if (subject.empty() || a == b) {
        return inner ? subject : result;
    }
    auto previous = subject.back();
    double distance = edgeDistance(a, b, previous);
    for (const auto point : subject) {
        const double next = edgeDistance(a, b, point);
        const bool keepPrevious = inner ? distance >= 0 : distance <= 0;
        const bool keepPoint = inner ? next >= 0 : next <= 0;
        if (keepPrevious != keepPoint && distance != 0 && next != 0) {
            const auto t = distance / (distance - next);
            result.push_back({static_cast<float>(std::lerp(double(previous.x), double(point.x), t)),
                              static_cast<float>(std::lerp(double(previous.y), double(point.y), t))});
        }
        if (keepPoint) {
            result.push_back(point);
        }
        previous = point;
        distance = next;
    }
    return hasArea(result) ? result : ClipPolygon{};
}

Area intersection(Area first, Area second)
{
    return intersectAreas(first, second);
}

ClipPolygon intersection(ClipPolygon subject, const ClipPolygon& mask)
{
    auto previous = mask.back();
    for (const auto point : mask) {
        subject = halfPlane(subject, previous, point, true);
        if (subject.empty()) {
            break;
        }
        previous = point;
    }
    return subject;
}

void subtract(Area area, Area cut, std::vector<Area>& result)
{
    const auto overlap = intersectAreas(area, cut);
    if (!hasArea(overlap)) {
        result.push_back(area);
        return;
    }
    const float right = overlap.x + overlap.width;
    const float bottom = overlap.y + overlap.height;
    for (const auto part : std::array{
            Area{area.x, area.y, area.width, overlap.y - area.y},
            Area{area.x, bottom, area.width, area.y + area.height - bottom},
            Area{area.x, overlap.y, overlap.x - area.x, overlap.height},
            Area{right, overlap.y, area.x + area.width - right, overlap.height},
        }) {
        if (hasArea(part)) {
            result.push_back(part);
        }
    }
}

void subtract(ClipPolygon area, const ClipPolygon& cut, std::vector<ClipPolygon>& result)
{
    auto previous = cut.back();
    for (const auto point : cut) {
        auto outside = halfPlane(area, previous, point, false);
        if (!outside.empty()) {
            result.push_back(std::move(outside));
        }
        area = halfPlane(area, previous, point, true);
        if (area.empty()) {
            break;
        }
        previous = point;
    }
}

template<typename Shape>
std::vector<Shape> difference(std::vector<Shape> source, const std::vector<Shape>& cuts)
{
    for (const auto& cut : cuts) {
        std::vector<Shape> next;
        for (auto& piece : source) {
            subtract(std::move(piece), cut, next);
        }
        source = std::move(next);
    }
    return source;
}

template<typename Shape>
std::vector<Shape> combine(const std::vector<Shape>& current, const std::vector<Shape>& operand, std::int32_t mode)
{
    if (mode == 0) {
        std::vector<Shape> parts;
        for (const auto& first : current) {
            for (const auto& second : operand) {
                auto overlap = intersection(first, second);
                if (hasArea(overlap)) {
                    parts.push_back(std::move(overlap));
                }
            }
        }
        return parts;
    }
    if (mode == 2) {
        return difference(operand, current);
    }
    if (mode == 3) {
        return difference(current, operand);
    }
    auto parts = mode == 1 ? current : difference(current, operand);
    auto added = difference(operand, current);
    parts.insert(parts.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    return parts;
}

bool contains(Area area, Point point)
{
    return point.x >= area.x && point.x < area.x + area.width &&
           point.y >= area.y && point.y < area.y + area.height;
}

bool contains(const ClipPolygon& polygon, Point point)
{
    auto previous = polygon.back();
    for (const auto vertex : polygon) {
        if (!(edgeDistance(previous, vertex, point) >= 0)) {
            return false;
        }
        previous = vertex;
    }
    return true;
}

}

bool hasArea(Area area)
{
    return std::isfinite(area.x) && std::isfinite(area.y) && area.width > 0 && area.height > 0 &&
           std::isfinite(area.x + area.width) && std::isfinite(area.y + area.height);
}

Area intersectAreas(Area first, Area second)
{
    const float x = std::max(first.x, second.x);
    const float y = std::max(first.y, second.y);
    return {x, y, std::min(first.x + first.width, second.x + second.width) - x,
                  std::min(first.y + first.height, second.y + second.height) - y};
}

ClipRegion regionOf(Area area)
{
    return hasArea(area) ? std::vector{area} : std::vector<Area>{};
}

ClipRegion mapRegion(const ViewMapping& mapping, Area area)
{
    if (!hasArea(area)) {
        return {};
    }
    if (std::fmod(mapping.rotation, 90.0) == 0) {
        return regionOf(mapping.mapArea(area));
    }
    const auto points = corners(area);
    ClipPolygon polygon;
    std::ranges::transform(points, std::back_inserter(polygon), [&](Point point) { return mapping.toPanel(point); });
    return hasArea(polygon) ? std::vector<ClipPolygon>{std::move(polygon)} : std::vector<ClipPolygon>{};
}

bool regionEmpty(const ClipRegion& region)
{
    return std::visit([](const auto& pieces) { return pieces.empty(); }, region);
}

bool regionContains(const ClipRegion& region, Point point)
{
    return std::visit([&](const auto& pieces) {
        return std::ranges::any_of(pieces, [&](const auto& shape) { return contains(shape, point); });
    }, region);
}

ClipRegion combineClip(const ClipRegion& current, const ClipRegion& operand, std::int32_t mode)
{
    if (const auto* rectangles = std::get_if<std::vector<Area>>(&current)) {
        if (const auto* other = std::get_if<std::vector<Area>>(&operand)) {
            return combine(*rectangles, *other, mode);
        }
    }
    return combine(asPolygons(current), asPolygons(operand), mode);
}

}
