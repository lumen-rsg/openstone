#include "Schematic.hpp"
#include "ViewRegion.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <ranges>
#include <set>
#include <utility>

namespace openstone {
namespace {

using namespace yellow;
using green::Point;

std::pair<double, double> rotatePoint(double x, double y, double degrees)
{
    if (degrees == 0) {
        return {x, y};
    }
    if (degrees == 90) {
        return {-y, x};
    }
    if (degrees == -90) {
        return {y, -x};
    }
    if (std::abs(degrees) == 180) {
        return {-x, -y};
    }
    const double radians = degrees * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    return {x * cosine - y * sine, x * sine + y * cosine};
}

ClipRegion viewClip(const View& view)
{
    if (view.mapping.clip) {
        return *view.mapping.clip;
    }
    return mapRegion(view.mapping, view.bounds);
}

// A missing extent means the drawing can extend beyond its declared Area.
std::optional<Area> paintExtent(const yellow::PanelDrawing& drawing)
{
    using namespace yellow;
    if (const auto* cached = std::get_if<CachedDrawCommand>(&drawing.command)) {
        return cached->bounds;
    }
    if (const auto* effect = std::get_if<EffectDrawCommand>(&drawing.command)) {
        return effect->bounds;
    }
    if (const auto* bitmap = std::get_if<BitmapDrawCommand>(&drawing.command)) {
        return drawing.mapping.mapArea(bitmap->area);
    }
    if (const auto* graph = std::get_if<GraphDrawCommand>(&drawing.command)) {
        return drawing.mapping.mapArea(graph->area);
    }
    return std::nullopt;
}

}

Point yellow::ViewMapping::toPanel(Point local) const
{
    const auto [x, y] = rotatePoint(local.x, local.y, rotation);
    return {static_cast<float>(origin.x + scale * x), static_cast<float>(origin.y + scale * y)};
}

Point yellow::ViewMapping::mapVector(Point local) const
{
    const auto [x, y] = rotatePoint(local.x, local.y, rotation);
    return {static_cast<float>(scale * x), static_cast<float>(scale * y)};
}

Point yellow::ViewMapping::fromPanel(Point position) const
{
    const auto [x, y] = rotatePoint(double(position.x) - origin.x, double(position.y) - origin.y, -rotation);
    return {static_cast<float>(x / scale), static_cast<float>(y / scale)};
}

Area yellow::ViewMapping::mapArea(Area local) const
{
    const auto first = toPanel({local.x, local.y});
    if (rotation == 0) {
        return {first.x, first.y, local.width * scale, local.height * scale};
    }
    const auto second = toPanel({local.x + local.width, local.y});
    const auto third = toPanel({local.x + local.width, local.y + local.height});
    const auto fourth = toPanel({local.x, local.y + local.height});
    const auto [left, right] = std::minmax({first.x, second.x, third.x, fourth.x});
    const auto [top, bottom] = std::minmax({first.y, second.y, third.y, fourth.y});
    return {left, top, right - left, bottom - top};
}

bool yellow::ViewMapping::fullyClipped() const
{
    return clip && regionEmpty(*clip);
}

bool yellow::ViewMapping::contains(Point local) const
{
    return !clip || regionContains(*clip, toPanel(local));
}

yellow::View Schematic::transformView(const SchematicNode& entry) const
{
    auto view = std::get<yellow::View>(readInput(entry.id, 0));
    if (view.panel == 0 || entry.kind == PrimitiveKind::ViewSmooth || tracingDrawRoutes()) {
        return view;
    }
    if (entry.kind == PrimitiveKind::ViewRotate) {
        const Point pivot{std::get<float>(readInput(entry.id, 1)), std::get<float>(readInput(entry.id, 2))};
        const double inputAngle = std::get<float>(readInput(entry.id, 3));
        const double angle = std::isfinite(inputAngle) ? std::remainder(inputAngle, 360.0) : 0;
        if (angle != 0 &&
            std::isfinite(pivot.x) && std::isfinite(pivot.y)) {
            const auto [shiftX, shiftY] = rotatePoint(pivot.x, pivot.y, angle);
            const auto [x, y] = rotatePoint(pivot.x - shiftX, pivot.y - shiftY, view.mapping.rotation);
            const Point origin{static_cast<float>(view.mapping.origin.x + view.mapping.scale * x),
                               static_cast<float>(view.mapping.origin.y + view.mapping.scale * y)};
            if (std::isfinite(origin.x) && std::isfinite(origin.y)) {
                view.mapping.origin = origin;
                view.mapping.rotation = std::remainder(view.mapping.rotation + angle, 360.0);
            }
        }
        return view;
    }
    if (entry.kind == PrimitiveKind::ViewScale) {
        const float ratio = std::abs(std::get<float>(readInput(entry.id, 1)));
        const float scale = view.mapping.scale * ratio;
        const float width = view.bounds.width / ratio;
        const float height = view.bounds.height / ratio;
        if (scale > 0 && std::isfinite(scale) && std::isfinite(width) && std::isfinite(height)) {
            view.mapping.scale = scale;
            view.bounds.width = width;
            view.bounds.height = height;
        }
        return view;
    }
    if (entry.kind == PrimitiveKind::Clip) {
        const auto area = std::get<Area>(readInput(entry.id, 1));
        const auto mode = std::get<std::int32_t>(readInput(entry.id, 2));
        if (mode >= 0 && mode <= 4) {
            view.mapping.clip = combineClip(viewClip(view), mapRegion(view.mapping, area), mode);
        }
        return view;
    }

    const auto dimension = [&](int input, float inherited) {
        const bool connected = std::ranges::any_of(links_, [&](const auto& link) {
            return link.target == entry.id && link.input == input;
        });
        return connected ? std::get<float>(readInput(entry.id, input)) : inherited;
    };
    const Area section{std::get<float>(readInput(entry.id, 1)), std::get<float>(readInput(entry.id, 2)),
                       dimension(3, view.bounds.width), dimension(4, view.bounds.height)};
    const auto origin = view.mapping.toPanel({section.x, section.y});
    view.mapping.clip = combineClip(viewClip(view), mapRegion(view.mapping, section), 0);
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y)) {
        return {};
    }
    view.mapping.origin = origin;
    view.bounds = {0, 0, std::isfinite(section.width) ? std::max(0.0F, section.width) : 0.0F,
                         std::isfinite(section.height) ? std::max(0.0F, section.height) : 0.0F};
    return view;
}

const Schematic* Schematic::panelOwner(const yellow::View& view) const
{
    const auto* owner = &rootGraph();
    for (int step : view.schematicPath) {
        owner = owner->module(step);
        if (!owner) {
            return nullptr;
        }
    }
    const auto entry = std::ranges::find(owner->nodes_, view.panel, &SchematicNode::id);
    return entry != owner->nodes_.end() && ownsPanel(entry->kind) ? owner : nullptr;
}

green::Value Schematic::readViewAttribute(const SchematicNode& entry, int output) const
{
    const auto view = std::get<yellow::View>(readInput(entry.id, 0));
    if (entry.kind == PrimitiveKind::ViewSize) {
        return output == 0 ? view.bounds.width : view.bounds.height;
    }
    if (entry.kind == PrimitiveKind::ViewArea) {
        return view.bounds;
    }
    auto area = Area{};
    if (const auto* owner = panelOwner(view)) {
        const auto panel = owner->readPanel(view.panel);
        area = panel.view.bounds;
        area.x = panel.position.x;
        area.y = panel.position.y;
    }
    return area;
}

void Schematic::applyPanelArea(int panel, Area area)
{
    auto& entry = node(panel);
    if (entry.kind == PrimitiveKind::Window) {
        const auto pixels = [](float units) {
            return static_cast<int>(std::round(std::min(double(units) * windowPixelsPerUnit, double(maximumWindowExtent))));
        };
        static_cast<void>(setWindowSize(entry, pixels(area.width), pixels(area.height)));
        return;
    }
    auto& bounds = entry.panelBounds;
    const auto previous = entry.anchorReference ? std::optional{readModulePanel(panel)} : std::nullopt;
    if (entry.panelAnchors & (AnchorRight | AnchorBottom)) {
        const auto view = moduleParentView(entry);
        if (view.panel != 0) {
            entry.anchorReference = Point{view.bounds.width, view.bounds.height};
        }
    } else {
        entry.anchorReference.reset();
    }
    bool moved = false;
    if (parent_) {
        auto& module = parent_->node(path_.back());
        auto& position = entry.kind == PrimitiveKind::ModulePropertiesGui ? module.propertiesPosition : module.panelPosition;
        const Point next{area.x, area.y};
        moved = position != next;
        position = next;
    }
    const Area local{bounds.x, bounds.y, area.width, area.height};
    if (moved || bounds != local ||
        (previous && (previous->view.bounds != local || previous->position != Point{area.x, area.y}))) {
        bounds = local;
        ++rootGraph().panelGeometryRevision_;
        fireOutputs(panel);
    }
}

void Schematic::receiveSetGuiArea(SchematicNode& entry, int input)
{
    if (input == 0) {
        fireOutput(entry, 0);
    } else if (input == 2) {
        const auto view = std::get<yellow::View>(readInput(entry.id, 0));
        const auto area = std::get<Area>(readInput(entry.id, 1));
        if (auto* owner = const_cast<Schematic*>(panelOwner(view)); owner && hasArea(area)) {
            owner->applyPanelArea(view.panel, area);
        }
    }
}

void Schematic::clearViewCache(SchematicNode& entry, const yellow::PanelControlId* panel)
{
    if (auto& state = entry.viewCache) {
        ++state->clears;
        if (panel) {
            std::erase_if(state->frames, [panel](const auto& frame) { return frame.first.first == *panel; });
        } else {
            state->frames.clear();
        }
    }
}

void Schematic::receiveRedraw(SchematicNode& entry, int input)
{
    if (input == 0) {
        fireOutput(entry, 0);
        return;
    }

    const bool partial = entry.kind == PrimitiveKind::RedrawArea;
    if (input != (partial ? 2 : 1)) {
        return;
    }
    const auto view = std::get<yellow::View>(readInput(entry.id, 0));
    requestRedraw(view, partial ? std::optional{std::get<Area>(readInput(entry.id, 1))} : std::nullopt);
}

bool Schematic::requestRedraw(const yellow::View& view, std::optional<yellow::Area> area)
{
    auto* owner = const_cast<Schematic*>(panelOwner(view));
    if (!owner) {
        return false;
    }

    auto& pending = owner->node(view.panel).redraw;
    if (area) {
        const auto bounds = regionOf(owner->readPanel(view.panel).view.bounds);
        const auto bounded = combineClip(mapRegion(view.mapping, *area), bounds, 0);
        const auto visible = view.mapping.clip ? combineClip(*view.mapping.clip, bounded, 0) : bounded;
        if (regionEmpty(visible)) {
            return true;
        }
        if (!pending.wholePanel) {
            pending.areas = combineClip(pending.areas, visible, 1);
        }
    } else {
        pending = {.wholePanel = true};
    }

    // A redraw travels toward the MGUI through the caches on its View route.
    auto& root = rootGraph();
    const yellow::PanelControlId surface{view.schematicPath, view.panel};
    for (const auto& boundary : view.drawRoute) {
        if (boundary.kind == yellow::DrawBoundaryKind::Cache) {
            auto* graph = &root;
            for (const int module : boundary.id.ownerPath) {
                graph = graph->module(module);
            }
            clearViewCache(graph->node(boundary.id.node), &surface);
        }
    }
    ++root.redrawRevision_;
    return true;
}

void Schematic::updatePanelPaint(std::vector<yellow::PanelDrawing>& previous,
                                 std::vector<yellow::PanelDrawing> fresh,
                                 const ClipRegion& dirty, Area bounds)
{
    std::set<std::uint64_t> unmaskedCaches;
    for (const auto& drawing : previous) {
        const auto* cache = std::get_if<yellow::CachedDrawCommand>(&drawing.command);
        if (cache && !drawing.mapping.clip) {
            unmaskedCaches.insert(cache->revision);
        }
    }
    std::set<std::uint64_t> reusedCaches;
    for (const auto& drawing : fresh) {
        const auto* cache = std::get_if<yellow::CachedDrawCommand>(&drawing.command);
        if (cache && unmaskedCaches.contains(cache->revision)) {
            reusedCaches.insert(cache->revision);
        }
    }
    const auto restrictTo = [&](yellow::PanelDrawing& drawing, bool inside) {
        const auto* cache = std::get_if<yellow::CachedDrawCommand>(&drawing.command);
        if (cache && reusedCaches.contains(cache->revision)) {
            // An unchanged texture can still cover the whole View.
            return !inside;
        }
        const auto extent = regionOf(intersectAreas(paintExtent(drawing).value_or(bounds), bounds));
        if (regionEmpty(extent)) {
            return false;
        }
        const auto& viewClip = drawing.mapping.clip;
        const auto shape = viewClip ? combineClip(*viewClip, extent, 0) : extent;
        auto parts = combineClip(shape, dirty, inside ? 0 : 3);
        if (parts == shape) {
            return !regionEmpty(shape);
        }
        drawing.mapping.clip = std::move(parts);
        return !drawing.mapping.fullyClipped();
    };
    const auto mask = [&](std::vector<yellow::PanelDrawing>& drawings, bool inside) {
        auto kept = drawings.begin();
        for (auto draw = drawings.begin(); draw != drawings.end(); ++draw) {
            if (restrictTo(*draw, inside)) {
                if (kept != draw) {
                    *kept = std::move(*draw);
                }
                ++kept;
            }
        }
        drawings.erase(kept, drawings.end());
    };
    mask(previous, false);
    mask(fresh, true);
    previous.insert(previous.end(), std::make_move_iterator(fresh.begin()), std::make_move_iterator(fresh.end()));
}

}
