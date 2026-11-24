#include "Schematic.hpp"
#include "ViewRegion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <ranges>

namespace openstone {
namespace {

using Clock = std::chrono::steady_clock;
using green::Value;
using yellow::PanelControlId;
using yellow::DrawBoundaryKind;
using yellow::View;
using namespace std::chrono_literals;

constexpr int valueOutput = 1;
constexpr int indexOutput = 2;
constexpr int stepOutput = 3;
constexpr int finishedOutput = 4;
constexpr auto drawingBudget = 50ms;
constexpr auto extendedNumberBudget = 600ms;
constexpr auto extendedArrayBudget = 3s;

std::optional<yellow::Area> captureBounds(const View& view, const View& target)
{
    const auto mapped = view.mapping.mapArea(view.bounds);
    const auto panel = target.bounds;
    const auto x = std::max(mapped.x, panel.x);
    const auto y = std::max(mapped.y, panel.y);
    const yellow::Area bounds{x, y, std::min(mapped.x + mapped.width, panel.x + panel.width) - x,
                                   std::min(mapped.y + mapped.height, panel.y + panel.height) - y};
    if (bounds.width > 0 && bounds.height > 0 && std::isfinite(bounds.width) && std::isfinite(bounds.height)) {
        return bounds;
    }
    return std::nullopt;
}

}

struct Schematic::DrawingPass {
    struct Operation {
        const Schematic* graph;
        const SchematicNode* entry;
        View view;
        bool paint;
        int input = 0;
    };

    struct Iteration {
        const Operation& loop;
        LoopValue current;
        std::size_t ordinal;
        const Iteration* outer;
    };

    using Route = std::vector<PanelControlId>;
    const View& target;
    double rasterScale;
    std::map<Route, std::vector<Operation>> operations;
    const Iteration* active = nullptr;
    bool tracing = true;
    bool explicitRedraw = false;
    bool rubyView = false;
    bool paint = true;

    [[nodiscard]] const Iteration* find(const Schematic& graph, int node) const;
    void draw(const Route& route, std::vector<yellow::PanelDrawing>& drawings, Clock::time_point deadline, bool publish);
    void cache(const Operation& operation, const Route& route, std::vector<yellow::PanelDrawing>& drawings,
               Clock::time_point deadline, bool publish);
    void effect(const Operation& operation, const Route& route, std::vector<yellow::PanelDrawing>& drawings,
                Clock::time_point deadline, bool publish);
    void run(const Operation& loop, const Route& route, std::vector<yellow::PanelDrawing>& drawings,
             Clock::time_point deadline, bool publish);
};

const Schematic::DrawingPass::Iteration* Schematic::DrawingPass::find(const Schematic& graph, int node) const
{
    for (auto frame = active; frame; frame = frame->outer) {
        if (frame->loop.graph == &graph && frame->loop.entry->id == node) {
            return frame;
        }
    }
    return nullptr;
}

Value Schematic::readViewLoop(const SchematicNode& entry, int output, Value fallback) const
{
    const auto* pass = rootGraph().drawingPass_;
    const auto* iteration = pass ? pass->find(*this, entry.id) : nullptr;
    if (output == 0) {
        auto view = std::get<View>(std::move(fallback));
        if (view.panel != 0 && pass) {
            if (pass->tracing) {
                view.drawRoute.push_back({{path_, entry.id}, DrawBoundaryKind::Loop});
            } else if (iteration) {
                view.drawOrder.push_back(iteration->ordinal);
            }
        }
        return view;
    }

    const auto* current = iteration ? &iteration->current : entry.viewLoopValue ? &*entry.viewLoopValue : nullptr;
    if (!current) {
        return fallback;
    }
    if (output == indexOutput) {
        return current->index;
    }
    return green::typeOf(current->value) == green::typeOf(fallback) ? current->value : fallback;
}

View Schematic::readDrawBoundary(const SchematicNode& entry, DrawBoundaryKind kind) const
{
    auto view = std::get<View>(readInput(entry.id, 0));
    if (view.panel != 0) {
        view.drawRoute.push_back({{path_, entry.id}, kind});
    }
    return view;
}

bool Schematic::tracingDrawRoutes() const
{
    const auto* pass = rootGraph().drawingPass_;
    return pass && pass->tracing;
}

bool Schematic::hasDrawingBehavior(PrimitiveBehavior behavior) const
{
    return std::ranges::any_of(nodes_, [behavior](const SchematicNode& entry) {
        if (behavior == PrimitiveBehavior::RubyCode) {
            return entry.rubyCode && entry.rubyCode->running &&
                   std::ranges::any_of(entry.rubyCode->ports.inputs, [](const RubyPort& port) {
                       return port.type == green::Type::View;
                   });
        }
        return primitive(entry.kind).behavior == behavior;
    }) || std::ranges::any_of(modules_, [behavior](const auto& module) {
        return module.second->hasDrawingBehavior(behavior);
    });
}

void Schematic::planPanelDrawings(const View& target, DrawingPass& pass) const
{
    for (const auto& entry : nodes_) {
        const auto behavior = primitive(entry.kind).behavior;
        if (entry.rubyCode) {
            const auto& code = *entry.rubyCode;
            if (!code.running) continue;
            for (int input = 0; input < std::ssize(code.ports.inputs); ++input) {
                if (code.ports.inputs[input].type != green::Type::View) continue;
                auto view = std::get<View>(readInput(entry.id, input));
                if (!view.isSamePanel(target)) continue;
                pass.rubyView = true;
                pass.explicitRedraw |= code.usesRedraw;
                if ((code.handlesMouse || !code.edits.empty()) &&
                    std::ranges::none_of(view.drawRoute, [](const auto& boundary) {
                        return boundary.kind == DrawBoundaryKind::Loop;
                    })) {
                    pass.operations[{}].push_back({this, &entry, view, false, input});
                }
                if (code.drawsView) {
                    DrawingPass::Route route;
                    for (const auto& boundary : view.drawRoute) route.push_back(boundary.id);
                    pass.operations[route].push_back({this, &entry, std::move(view), true, input});
                }
            }
            continue;
        }
        const bool drawn = behavior == PrimitiveBehavior::DrawView || behavior == PrimitiveBehavior::ViewLoop ||
                           behavior == PrimitiveBehavior::ViewCache || behavior == PrimitiveBehavior::ViewEffect;
        if (!drawn && behavior != PrimitiveBehavior::Redraw && behavior != PrimitiveBehavior::MouseArea &&
            entry.kind != PrimitiveKind::EditControl && entry.kind != PrimitiveKind::PopupList &&
            entry.kind != PrimitiveKind::DropFiles) {
            continue;
        }

        auto view = std::get<View>(readInput(entry.id, 0));
        if (!view.isSamePanel(target)) {
            continue;
        }
        if (behavior == PrimitiveBehavior::Redraw) {
            pass.explicitRedraw = true;
            continue;
        }
        // Mouse regions and native controls stay live and have a single panel instance.
        if (!drawn && std::ranges::any_of(view.drawRoute, [](const auto& boundary) {
                return boundary.kind == DrawBoundaryKind::Loop;
            })) {
            continue;
        }
        DrawingPass::Route route;
        if (drawn) {
            for (const auto& boundary : view.drawRoute) {
                route.push_back(boundary.id);
            }
        }
        pass.operations[route].push_back({this, &entry, std::move(view), drawn});
    }
    for (const auto& [id, child] : modules_) {
        child->planPanelDrawings(target, pass);
    }
}

void Schematic::DrawingPass::draw(const Route& route, std::vector<yellow::PanelDrawing>& drawings,
                                  Clock::time_point deadline, bool publish)
{
    const auto found = operations.find(route);
    if (found == operations.end()) {
        return;
    }
    for (const auto& operation : found->second) {
        if (!paint && operation.paint) {
            continue;
        }
        const auto behavior = primitive(operation.entry->kind).behavior;
        if (behavior == PrimitiveBehavior::ViewLoop) {
            run(operation, route, drawings, deadline, publish);
        } else if (behavior == PrimitiveBehavior::ViewCache) {
            cache(operation, route, drawings, deadline, publish);
        } else if (behavior == PrimitiveBehavior::ViewEffect) {
            effect(operation, route, drawings, deadline, publish);
        } else {
            auto view = std::get<View>(operation.graph->readInput(operation.entry->id, operation.input));
            if (!view.isSamePanel(operation.view)) continue;
            if (behavior == PrimitiveBehavior::RubyCode) {
                if (operation.paint) {
                    if (Clock::now() < deadline)
                        operation.graph->appendRubyDrawing(*operation.entry, operation.input, view, drawings, publish);
                } else if (!view.mapping.fullyClipped() && view.bounds.width > 0 && view.bounds.height > 0) {
                    if (operation.entry->rubyCode->handlesMouse) {
                        yellow::MouseAreaCommand area{{operation.graph->path_, operation.entry->id}, view.bounds, view.mouseMoves,
                                                       {yellow::MouseCursor::HandPointer}, false, operation.input};
                        drawings.push_back({std::move(area), view.drawOrder, view.mapping});
                    }
                    operation.graph->appendRubyEditControls(*operation.entry, operation.input, view, drawings);
                }
            } else {
                operation.graph->appendDrawing(*operation.entry, std::move(view), drawings);
            }
        }
    }
}

void Schematic::DrawingPass::cache(const Operation& operation, const Route& route,
                                   std::vector<yellow::PanelDrawing>& drawings, Clock::time_point deadline, bool publish)
{
    const auto& graph = *operation.graph;
    const auto& entry = *operation.entry;
    const auto view = std::get<View>(graph.readInput(entry.id, 0));
    if (!view.isSamePanel(target)) {
        return;
    }

    const auto bounds = captureBounds(view, target);
    if (!bounds) {
        return;
    }
    const auto panel = target.bounds;

    const auto& root = graph.rootGraph();
    ViewCacheState* writable = nullptr;
    if (publish) {
        auto& storage = const_cast<Schematic&>(graph).node(entry.id).viewCache;
        if (!storage || storage->topology != root.drawingTopology_) {
            storage.emplace().topology = root.drawingTopology_;
        }
        writable = &*storage;
    }

    const auto& state = entry.viewCache;
    const auto key = std::pair{PanelControlId{target.schematicPath, target.panel}, view.drawOrder};
    if (state && state->topology == root.drawingTopology_) {
        const auto found = state->frames.find(key);
        if (found != state->frames.end() && found->second.view == view && found->second.panelBounds == panel &&
            (!publish || found->second.scale == rasterScale)) {
            const auto& cached = found->second.command;
            if (!cached.drawings->empty()) {
                drawings.push_back({cached, view.drawOrder, {}});
            }
            return;
        }
    }
    if (Clock::now() >= deadline) {
        return;
    }

    const auto clears = state ? state->clears : 0;
    const auto topology = root.drawingTopology_;
    auto children = std::make_shared<std::vector<yellow::PanelDrawing>>();
    auto downstream = route;
    downstream.push_back({graph.path_, entry.id});
    draw(downstream, *children, deadline, publish);
    std::ranges::stable_sort(*children, {}, &yellow::PanelDrawing::order);
    const yellow::CachedDrawCommand command{*bounds, ++root.nextCacheRevision_, std::move(children)};
    // A trigger from a downstream loop can clear this cache during capture.
    if (writable && writable->clears == clears && root.drawingTopology_ == topology && Clock::now() < deadline) {
        writable->frames.insert_or_assign(key, ViewCacheFrame{view, panel, rasterScale, command});
    }
    if (!command.drawings->empty()) {
        drawings.push_back({command, view.drawOrder, {}});
    }
}

void Schematic::DrawingPass::effect(const Operation& operation, const Route& route,
                                    std::vector<yellow::PanelDrawing>& drawings, Clock::time_point deadline, bool publish)
{
    const auto& graph = *operation.graph;
    const auto& entry = *operation.entry;
    auto view = std::get<View>(graph.readInput(entry.id, 0));
    if (!view.isSamePanel(target) || Clock::now() >= deadline) {
        return;
    }
    const auto bounds = captureBounds(view, target);
    if (!bounds) {
        return;
    }

    auto settings = graph.readViewEffect(entry);
    auto children = std::make_shared<std::vector<yellow::PanelDrawing>>();
    auto downstream = route;
    downstream.push_back({graph.path_, entry.id});
    draw(downstream, *children, deadline, publish);
    if (children->empty() && !std::holds_alternative<yellow::ViewShader>(settings)) {
        return;
    }
    std::ranges::stable_sort(*children, {}, &yellow::PanelDrawing::order);
    yellow::EffectDrawCommand command{*bounds, view.mapping.scale, view.mapping.rotation, std::move(settings), std::move(children)};
    // The command is in panel coordinates. Keep the inherited clip on the final composite.
    drawings.push_back({std::move(command), std::move(view.drawOrder), {.clip = std::move(view.mapping.clip)}});
}

void Schematic::DrawingPass::run(const Operation& loop, const Route& route,
                                 std::vector<yellow::PanelDrawing>& drawings, Clock::time_point deadline, bool publish)
{
    const auto& graph = *loop.graph;
    const auto& entry = *loop.entry;
    const auto type = graph.outputType(entry.id, valueOutput);
    if (!type || Clock::now() >= deadline || !std::get<View>(graph.readInput(entry.id, 0)).isSamePanel(loop.view)) {
        return;
    }

    const bool array = entry.kind == PrimitiveKind::ViewArrayLoop;
    const bool extended = std::get<bool>(graph.readInput(entry.id, array ? 3 : 4));
    const auto budget = extended ? (array ? extendedArrayBudget : extendedNumberBudget) : drawingBudget;
    const auto stop = std::min(deadline, Clock::now() + budget);
    auto downstream = route;
    downstream.push_back({graph.path_, entry.id});
    std::size_t ordinal = 0;
    auto* publisher = publish ? const_cast<Schematic*>(&graph) : nullptr;

    const auto step = [&](Value value, std::int32_t index) {
        Iteration frame{loop, {std::move(value), index}, ordinal++, active};
        active = &frame;
        try {
            if (publisher) {
                auto& node = publisher->node(entry.id);
                node.viewLoopValue = frame.current;
                publisher->fireOutput(node, stepOutput);
            }
            draw(downstream, drawings, stop, publish);
        } catch (...) {
            active = frame.outer;
            throw;
        }
        active = frame.outer;
    };

    if (array) {
        const bool reverse = std::get<bool>(graph.readInput(entry.id, 2));
        const auto drawArray = [&](const auto& values) {
            const auto size = values.size();
            if (size > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) + 1) {
                return;
            }
            for (std::size_t i = 0; i < size && Clock::now() < stop; ++i) {
                const auto index = reverse ? size - 1 - i : i;
                step(values[index], static_cast<std::int32_t>(index));
            }
        };
        const auto values = graph.readInput(entry.id, 1);
        switch (*type) {
        case green::Type::Int: drawArray(std::get<green::IntArray>(values)); break;
        case green::Type::Float: drawArray(std::get<green::FloatArray>(values)); break;
        case green::Type::String: drawArray(std::get<green::StringArray>(values)); break;
        case green::Type::Point: drawArray(std::get<green::PointArray>(values)); break;
        default: throw std::logic_error("Invalid View Array Loop type");
        }
    } else if (*type == green::Type::Int) {
        const auto number = [&](int input) -> std::int64_t { return std::get<std::int32_t>(graph.readInput(entry.id, input)); };
        const auto start = number(1);
        const auto increment = number(2);
        const auto end = number(3);
        for (auto value = start; (increment > 0 ? value < end : increment < 0 && value > end) &&
             ordinal <= std::numeric_limits<std::int32_t>::max() && Clock::now() < stop; value += increment) {
            step(static_cast<std::int32_t>(value), static_cast<std::int32_t>(ordinal));
        }
    } else {
        const auto number = [&](int input) { return std::get<float>(graph.readInput(entry.id, input)); };
        const auto start = number(1);
        const auto increment = number(2);
        const auto end = number(3);
        if (!std::isfinite(start) || !std::isfinite(increment) || !std::isfinite(end)) {
            return;
        }
        for (auto value = start; (increment > 0 ? value < end : increment < 0 && value > end) &&
             ordinal <= std::numeric_limits<std::int32_t>::max() && Clock::now() < stop;) {
            step(value, static_cast<std::int32_t>(ordinal));
            const auto next = value + increment;
            if (next == value) {
                break;
            }
            value = next;
        }
    }

    if (publisher && (array || ordinal != 0)) {
        publisher->fireOutput(publisher->node(entry.id), finishedOutput);
    }
}

std::vector<yellow::PanelDrawing> Schematic::evaluatePanel(const View& view, double rasterScale,
                                                         SchematicNode* panel, bool publish) const
{
    std::vector<yellow::PanelDrawing> drawings;
    if (view.panel == 0) {
        return drawings;
    }
    const auto& root = rootGraph();
    DrawingPass pass{view, rasterScale};
    const auto topology = root.drawingTopology_;
    PanelRedraw request;
    bool retainPaint = false;
    const auto previous = std::exchange(root.drawingPass_, &pass);
    try {
        root.planPanelDrawings(view, pass);
        pass.tracing = false;
        if (panel) {
            request = std::exchange(panel->redraw, {});
            retainPaint = pass.explicitRedraw || pass.rubyView;
            if (retainPaint) {
                const auto& retained = panel->panelPaint;
                // Keep a baseline even before Ruby makes its first explicit redraw request.
                request.wholePanel |= !pass.explicitRedraw || !retained || retained->topology != topology ||
                                      retained->bounds != view.bounds || retained->rasterScale != rasterScale;
                pass.paint = request.wholePanel || !yellow::regionEmpty(request.areas);
            } else {
                panel->panelPaint.reset();
            }
        }
        for (auto& [route, operations] : pass.operations) {
            std::ranges::stable_sort(operations, {}, [](const auto& operation) -> const auto& {
                return operation.view.drawOrder;
            });
        }
        pass.draw({}, drawings, Clock::now() + extendedArrayBudget, publish);
    } catch (...) {
        root.drawingPass_ = previous;
        if (retainPaint && pass.paint) {
            panel->redraw.wholePanel = true;
        }
        throw;
    }
    root.drawingPass_ = previous;

    if (retainPaint) {
        const auto painted = [](const yellow::PanelDrawing& drawing) {
            return std::holds_alternative<yellow::DrawCommand>(drawing.command) ||
                   std::holds_alternative<yellow::TextCommand>(drawing.command) ||
                   std::holds_alternative<yellow::BitmapDrawCommand>(drawing.command) ||
                   std::holds_alternative<yellow::GraphDrawCommand>(drawing.command) ||
                   std::holds_alternative<yellow::CachedDrawCommand>(drawing.command) ||
                   std::holds_alternative<yellow::EffectDrawCommand>(drawing.command);
        };
        const auto controls = std::ranges::stable_partition(drawings, painted).begin();
        if (pass.paint) {
            std::vector<yellow::PanelDrawing> paint(std::make_move_iterator(drawings.begin()),
                                                   std::make_move_iterator(controls));
            if (request.wholePanel) {
                panel->panelPaint = PanelPaint{view.bounds, rasterScale, topology, std::move(paint)};
            } else {
                updatePanelPaint(panel->panelPaint->drawings, std::move(paint), request.areas, view.bounds);
            }
        }
        drawings.erase(drawings.begin(), controls);
        drawings.append_range(panel->panelPaint->drawings);
    }

    MouseAreas areas;
    for (auto& drawing : drawings) {
        if (auto* area = std::get_if<yellow::MouseAreaCommand>(&drawing.command); area && area->rubyInput < 0) {
            areas.emplace(area->id, area);
        }
    }
    root.applyMouseInfo(areas);
    std::ranges::stable_sort(drawings, {}, &yellow::PanelDrawing::order);
    return drawings;
}

std::vector<yellow::PanelDrawing> Schematic::panelDrawings(const View& view) const
{
    return evaluatePanel(view, 1, nullptr, false);
}

std::vector<yellow::PanelDrawing> Schematic::renderPanel(const View& view, double rasterScale)
{
    auto* owner = const_cast<Schematic*>(panelOwner(view));
    auto drawings = evaluatePanel(view, rasterScale, owner ? &owner->node(view.panel) : nullptr, owner != nullptr);
    refreshConnections();
    return drawings;
}

}
