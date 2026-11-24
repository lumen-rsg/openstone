#include "Schematic.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <set>
#include <tuple>

namespace openstone {
namespace {

using yellow::MouseButton;
using yellow::MouseEventKind;
using yellow::MouseMessage;
using green::Point;

constexpr int dragScaleInput = 3;
constexpr int dragSetInput = 4;
constexpr int dragDownOutput = 0;
constexpr int dragPositionOutput = 1;
constexpr int dragUpOutput = 2;
constexpr int draggingOutput = 3;

Point clampAccumulator(Point value)
{
    return {std::min(std::max(0.0F, value.x), 1.0F), std::min(std::max(0.0F, value.y), 1.0F)};
}

bool finite(Point point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

double crossProduct(Point a, Point b, Point c)
{
    const double dx = static_cast<double>(b.x) - a.x;
    const double dy = static_cast<double>(b.y) - a.y;
    return dx * (static_cast<double>(c.y) - a.y) - dy * (static_cast<double>(c.x) - a.x);
}

bool between(float coordinate, float a, float b)
{
    return coordinate >= std::min(a, b) && coordinate <= std::max(a, b);
}

std::optional<yellow::MouseRegion> makeMousePolygon(green::PointArray vertices)
{
    if (vertices.size() < 3 || !std::ranges::all_of(vertices, finite)) {
        return std::nullopt;
    }
    const auto first = vertices.front();
    const auto second = std::ranges::find_if(vertices, [first](Point point) { return point != first; });
    if (second == vertices.end() || std::ranges::none_of(vertices, [first, second](Point point) {
            return crossProduct(first, *second, point) != 0;
        })) {
        return std::nullopt;
    }
    const auto [left, right] = std::ranges::minmax_element(vertices, {}, &Point::x);
    const auto [top, bottom] = std::ranges::minmax_element(vertices, {}, &Point::y);
    return yellow::MousePolygon{{left->x, top->y}, {right->x, bottom->y}, std::move(vertices)};
}

bool contains(yellow::Area area, green::Point position)
{
    const double right = static_cast<double>(area.x) + area.width;
    const double bottom = static_cast<double>(area.y) + area.height;
    return std::isfinite(right) && std::isfinite(bottom) && area.width != 0 && area.height != 0 &&
           position.x >= std::min<double>(area.x, right) && position.x <= std::max<double>(area.x, right) &&
           position.y >= std::min<double>(area.y, bottom) && position.y <= std::max<double>(area.y, bottom);
}

std::string_view rubyMouseCallback(const MouseMessage& message)
{
    const bool left = message.button == MouseButton::Left;
    if (message.kind == MouseEventKind::Move && message.button == MouseButton::None) return "mouseMove";
    if (!left && message.button != MouseButton::Right) return {};
    switch (message.kind) {
    case MouseEventKind::Drag: return "mouseMoveCaptured";
    case MouseEventKind::ButtonDown: return left ? "mouseLDown" : "mouseRDown";
    case MouseEventKind::ButtonUp: return left ? "mouseLUp" : "mouseRUp";
    case MouseEventKind::DoubleClick: return left ? "mouseLDouble" : "mouseRDouble";
    default: return {};
    }
}

bool matches(PrimitiveKind kind, const MouseMessage& message)
{
    switch (kind) {
    case PrimitiveKind::MouseLDbl:
        return message.button == MouseButton::Left && message.kind == MouseEventKind::DoubleClick;
    case PrimitiveKind::MouseLDown:
        return message.button == MouseButton::Left && message.kind == MouseEventKind::ButtonDown;
    case PrimitiveKind::MouseLUp:
        return message.button == MouseButton::Left && message.kind == MouseEventKind::ButtonUp;
    case PrimitiveKind::MouseRDbl:
        return message.button == MouseButton::Right && message.kind == MouseEventKind::DoubleClick;
    case PrimitiveKind::MouseRDown:
        return message.button == MouseButton::Right && message.kind == MouseEventKind::ButtonDown;
    case PrimitiveKind::MouseRUp:
        return message.button == MouseButton::Right && message.kind == MouseEventKind::ButtonUp;
    case PrimitiveKind::MouseWheel:
        return message.kind == MouseEventKind::Wheel && message.wheelDelta.y != 0;
    default:
        return false;
    }
}

}

bool yellow::MousePolygon::contains(Point position) const
{
    if (!finite(position) || position.x < minimum.x || position.x > maximum.x ||
        position.y < minimum.y || position.y > maximum.y || vertices.size() < 3) {
        return false;
    }
    bool inside = false;
    auto previous = vertices.back();
    for (const auto current : vertices) {
        const double cross = crossProduct(previous, current, position);
        if (cross == 0 && between(position.x, previous.x, current.x) && between(position.y, previous.y, current.y)) {
            return true;
        }
        // Count crossings to the right, using half-open edges to count shared vertices once.
        if ((previous.y > position.y) != (current.y > position.y) && (cross > 0) == (current.y > previous.y)) {
            inside = !inside;
        }
        previous = current;
    }
    return inside;
}

std::optional<yellow::MouseRegion> Schematic::readMouseRegion(const SchematicNode& entry) const
{
    auto shape = readInput(entry.id, 1);
    if (auto* vertices = std::get_if<green::PointArray>(&shape)) {
        return makeMousePolygon(std::move(*vertices));
    }
    const auto area = std::get<yellow::Area>(shape);
    if (!contains(area, {area.x, area.y})) {
        return std::nullopt;
    }
    return area;
}

std::optional<yellow::PanelControlId> Schematic::findMouseAreaSource(int id) const
{
    const auto* graph = this;
    int input = 0;
    std::set<std::tuple<const Schematic*, int, int>> visited;
    while (visited.emplace(graph, id, input).second) {
        const auto wire = std::ranges::find_if(graph->links_, [&](const SchematicLink& link) {
            return link.target == id && link.input == input &&
                   graph->outputType(link.source, link.output) == green::Type::MouseMessage;
        });
        if (wire == graph->links_.end()) {
            return std::nullopt;
        }
        const auto& source = graph->node(wire->source);
        switch (primitive(source.kind).behavior) {
        case PrimitiveBehavior::MouseArea:
            return yellow::PanelControlId{graph->path_, source.id};
        case PrimitiveBehavior::ModuleInput:
            id = graph->path_.back();
            input = source.boundaryIndex;
            graph = graph->parent_;
            break;
        case PrimitiveBehavior::Module:
            id = source.moduleOutputs.at(wire->output).endpoint;
            input = 0;
            graph = graph->module(source.id);
            break;
        case PrimitiveBehavior::ReceiveWireless: {
            const auto senders = graph->wirelessSources(source.id);
            if (senders.empty()) {
                return std::nullopt;
            }
            graph = senders.front().graph;
            id = senders.front().id;
            input = 0;
            break;
        }
        case PrimitiveBehavior::ExtractBus: {
            const auto bus = graph->findBusSource(source.id);
            if (!bus) {
                return std::nullopt;
            }
            input = graph->busChannelInput(*bus, source.channelNames.at(wire->output));
            graph = bus->graph;
            id = bus->id;
            break;
        }
        case PrimitiveBehavior::MouseCaptureControl:
            id = source.id;
            input = 0;
            break;
        case PrimitiveBehavior::Switch:
            if (!std::get<bool>(graph->readInput(source.id, 1))) {
                return std::nullopt;
            }
            id = source.id;
            input = 0;
            break;
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void Schematic::applyMouseInfo(MouseAreas& areas) const
{
    if (areas.empty()) {
        return;
    }
    for (const auto& entry : nodes_) {
        if (entry.kind != PrimitiveKind::MouseInfo) {
            continue;
        }
        if (const auto source = findMouseAreaSource(entry.id)) {
            if (const auto found = areas.find(*source); found != areas.end()) {
                found->second->hover = {
                    yellow::parseMouseCursor(std::get<std::string>(readInput(entry.id, 1))),
                    std::get<std::string>(readInput(entry.id, 2)),
                };
                // The first matching Mouse Info in schematic order owns this region.
                areas.erase(found);
            }
        }
    }
    for (const auto& [id, child] : modules_) {
        child->applyMouseInfo(areas);
    }
}

bool Schematic::sendMouseMessage(int id, const MouseMessage& message, MouseDragEffects* effects)
{
    if (effects) {
        *effects = {};
    }
    const auto entry = std::ranges::find(nodes_, id, &SchematicNode::id);
    if (entry == nodes_.end() || primitive(entry->kind).behavior != PrimitiveBehavior::MouseArea ||
        message.kind == MouseEventKind::None || !finite(message.position)) {
        return false;
    }
    const auto captured = capturesMouseArea({path_, id});
    const auto view = std::get<yellow::View>(readInput(id, 0));
    if (view.panel == 0 || (message.kind == MouseEventKind::Move && !view.mouseMoves && !captured)) {
        return false;
    }
    const auto region = readMouseRegion(*entry);
    if (!region) {
        return false;
    }
    // A grabbed button can move and release outside the area that received the press.
    if (!captured && message.kind != MouseEventKind::Drag && message.kind != MouseEventKind::ButtonUp) {
        const auto* area = std::get_if<yellow::Area>(&*region);
        const bool hit = area ? contains(*area, message.position)
                              : std::get<yellow::MousePolygon>(*region).contains(message.position);
        if (!hit || !view.mapping.contains(message.position)) {
            return false;
        }
    }

    entry->lastMouseMessage = message;
    auto& root = rootGraph();
    MouseDispatch dispatch{message, {path_, id}, {}};
    auto& active = root.mouseDispatch_;
    const auto previous = std::exchange(active, &dispatch);
    try {
        fireOutput(*entry, 0);
    } catch (...) {
        active = previous;
        if (dispatch.drag.fileDrag) {
            root.dragFilesSession_.reset();
        }
        throw;
    }
    active = previous;
    if (message.kind == MouseEventKind::ButtonUp && message.button == MouseButton::Left) {
        // A route can close mid-gesture. Any listener that missed the release still needs to leave Dragging.
        root.clearMouseDrags(dispatch.source);
    }
    if (effects) {
        *effects = std::move(dispatch.drag);
    } else if (dispatch.drag.fileDrag) {
        root.finishDragFiles(dispatch.drag.fileDrag->token, false);
    }
    refreshConnections();
    return true;
}

SchematicNode* Schematic::findRubyMouseNode(int id, int input)
{
    const auto entry = std::ranges::find(nodes_, id, &SchematicNode::id);
    if (entry == nodes_.end() || !entry->rubyCode || !entry->rubyCode->running ||
        !entry->rubyCode->handlesMouse || input < 0 || input >= std::ssize(entry->rubyCode->ports.inputs) ||
        entry->rubyCode->ports.inputs[input].type != green::Type::View) {
        return nullptr;
    }
    return &*entry;
}

std::optional<yellow::MouseCursor> Schematic::hitRubyMouse(const SchematicNode& entry, int input,
                                                          const yellow::View& view, Point position, Point local,
                                                          bool queryCursor) const
{
    const auto* owner = panelOwner(view);
    if (!owner || !finite(local) || !view.mapping.contains(local) || !contains(view.bounds, local) ||
        !contains(owner->readPanel(view.panel).view.bounds, position)) {
        return std::nullopt;
    }
    updateRubyContext(entry);
    return entry.rubyCode->instance->mouseHit(input, local, rubySeconds(rubyNow()), queryCursor);
}

bool Schematic::rubyMouseContains(int id, int input, Point position)
{
    return queryRubyMouse(id, input, position, false).has_value();
}

std::optional<yellow::MouseCursor> Schematic::rubyMouseCursor(int id, int input, Point position)
{
    return queryRubyMouse(id, input, position, true);
}

std::optional<yellow::MouseCursor> Schematic::queryRubyMouse(int id, int input, Point position, bool queryCursor)
{
    auto* entry = findRubyMouseNode(id, input);
    if (!entry || !finite(position)) return std::nullopt;
    try {
        const auto view = std::get<yellow::View>(readInput(id, input));
        return hitRubyMouse(*entry, input, view, position, view.mapping.fromPanel(position), queryCursor);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool Schematic::sendRubyMouseMessage(int id, int input, const MouseMessage& message)
{
    const auto callback = rubyMouseCallback(message);
    auto* entry = findRubyMouseNode(id, input);
    if (!entry || callback.empty() || !finite(message.position)) return false;

    const auto view = std::get<yellow::View>(readInput(id, input));
    auto& code = *entry->rubyCode;
    const auto local = view.mapping.fromPanel(message.position);
    const yellow::PanelControlId panel{view.schematicPath, view.panel};
    const bool captured = code.mouseCapture && code.mouseCapture->input == input &&
                          code.mouseCapture->panel == panel && code.mouseCapture->button == message.button;
    const bool releasing = captured && message.kind == MouseEventKind::ButtonUp;
    if (releasing) code.mouseCapture.reset();
    if (view.panel == 0 || !finite(local) ||
        (message.kind == MouseEventKind::Move && (!view.mouseMoves || code.mouseCapture)) ||
        (message.kind == MouseEventKind::Drag && !captured)) {
        return false;
    }

    const auto now = rubyNow();
    std::optional<RubyReply> reply;
    try {
        if (!captured && !hitRubyMouse(*entry, input, view, message.position, local)) return false;
        updateRubyContext(*entry);
        const auto name = releasing ? (message.button == MouseButton::Left ? "mouseLUpCaptured" : "mouseRUpCaptured")
                                    : callback;
        reply = code.instance->mouseEvent(input, name, local, rubySeconds(now));
    } catch (const std::exception& error) {
        code.status = error.what();
        code.error = true;
        return true;
    }
    if (reply) {
        if (reply->captureMouse == true && message.kind == MouseEventKind::ButtonDown && !code.mouseCapture) {
            code.mouseCapture = RubyMouseCapture{input, message.button, panel};
        } else if (captured && (releasing || reply->captureMouse == false)) {
            code.mouseCapture.reset();
            // Cursor calls in the release callback belong to the gesture that just ended.
            std::erase_if(reply->actions, [](const RubyAction& action) {
                return action.kind == RubyActionKind::ShowCursor;
            });
        }
        publishRuby(*entry, std::move(*reply), now);
        refreshConnections();
    }
    // A View can accept a press even if it only defines the release callback.
    return reply.has_value() || releasing || message.kind == MouseEventKind::ButtonDown;
}

void Schematic::cancelRubyMouseDrag(int id, int input)
{
    if (auto* entry = findRubyMouseNode(id, input)) {
        auto& capture = entry->rubyCode->mouseCapture;
        if (capture && capture->input == input) capture.reset();
    }
}

bool Schematic::capturesMouseArea(const yellow::PanelControlId& source) const
{
    const auto& root = rootGraph();
    if (!root.mouseCaptureOwner_) return false;
    const auto& capture = *root.mouseCaptureOwner_;
    const auto* owner = &root;
    for (const auto child : capture.ownerPath) {
        owner = owner->module(child);
        if (!owner) return false;
    }
    const auto node = std::ranges::find(owner->nodes_, capture.node, &SchematicNode::id);
    return node != owner->nodes_.end() && node->kind == PrimitiveKind::MouseCapture &&
           owner->findMouseAreaSource(node->id) == source;
}

bool Schematic::releaseMouseCapture()
{
    auto& root = rootGraph();
    if (!root.mouseCaptureOwner_) return false;
    root.mouseCaptureOwner_.reset();
    ++root.redrawRevision_;
    return true;
}

void Schematic::receiveMouseCapture(SchematicNode& entry, int input)
{
    auto& root = rootGraph();
    const yellow::PanelControlId capture{path_, entry.id};
    if (input == 1 && findMouseAreaSource(entry.id)) {
        root.mouseCaptureOwner_ = capture;
        ++root.redrawRevision_;
    } else if (input == 2 && root.mouseCaptureOwner_ == capture) {
        releaseMouseCapture();
    } else if (input == 0 && root.mouseDispatch_ &&
               findMouseAreaSource(entry.id) == root.mouseDispatch_->source) {
        entry.lastMouseMessage = root.mouseDispatch_->message;
        fireOutput(entry, 0);
    }
}

void Schematic::receiveMouseMessage(SchematicNode& entry)
{
    const auto* active = rootGraph().mouseDispatch_;
    if (!active || !matches(entry.kind, active->message)) {
        return;
    }
    // Link creation and unrelated triggers can read the last message, but do not replay it.
    const auto message = std::get<MouseMessage>(readInput(entry.id, 0));
    if (message == active->message) {
        entry.lastMouseMessage = message;
        fireOutput(entry, 0);
    }
}

green::Value Schematic::readMouseDrag(const SchematicNode& entry, int output) const
{
    if (output == draggingOutput) {
        return entry.mouseDrag && entry.mouseDrag->source.has_value();
    }
    return entry.mouseDrag ? entry.mouseDrag->positions.at(output) : Point{};
}

void Schematic::receiveMouseDrag(SchematicNode& entry, int input)
{
    const bool accumulating = entry.kind == PrimitiveKind::MouseDragAccumulate;
    if (accumulating && input == dragSetInput) {
        auto& state = entry.mouseDrag ? *entry.mouseDrag : entry.mouseDrag.emplace();
        if (!state.source) {
            state.positions[dragPositionOutput] = clampAccumulator(std::get<Point>(readInput(entry.id, dragSetInput)));
        }
        return;
    }
    if (input != 0) {
        return;
    }
    auto* dispatch = rootGraph().mouseDispatch_;
    if (!dispatch || dispatch->message.button != MouseButton::Left ||
        std::get<MouseMessage>(readInput(entry.id, 0)) != dispatch->message) {
        return;
    }
    const auto& message = dispatch->message;
    if (message.kind == MouseEventKind::ButtonDown) {
        auto& state = entry.mouseDrag ? *entry.mouseDrag : entry.mouseDrag.emplace();
        if (state.source) {
            return;
        }
        state.source = dispatch->source;
        auto& position = state.positions[dragPositionOutput];
        if (accumulating) {
            state.accumulator = MouseDragAccumulator{message.position, position,
                                                    std::get<Point>(readInput(entry.id, dragScaleInput))};
        } else {
            position = message.position;
        }
        state.positions[dragDownOutput] = position;
        dispatch->drag.started = true;
        dispatch->drag.accumulating |= accumulating;
        if (!dispatch->drag.cursor) {
            dispatch->drag.cursor = yellow::parseMouseCursor(std::get<std::string>(readInput(entry.id, 1)));
        }
        if (accumulating) {
            fireOutput(entry, draggingOutput);
            fireOutput(entry, dragDownOutput);
            fireOutput(entry, dragPositionOutput);
        } else {
            fireOutput(entry, dragDownOutput);
            fireOutput(entry, draggingOutput);
        }
        return;
    }
    if (!entry.mouseDrag || entry.mouseDrag->source != dispatch->source) {
        return;
    }
    auto& state = *entry.mouseDrag;
    auto& position = state.positions[dragPositionOutput];
    if (message.kind == MouseEventKind::Drag) {
        if (auto& accumulator = state.accumulator) {
            const auto scale = std::get<Point>(readInput(entry.id, dragScaleInput));
            if (scale != accumulator->scale) {
                // Apply a changed sensitivity from the current pointer and value.
                *accumulator = {message.position, position, scale};
            }
            position = clampAccumulator({
                accumulator->anchorValue.x + (message.position.x - accumulator->anchorPosition.x) * scale.x,
                accumulator->anchorValue.y + (message.position.y - accumulator->anchorPosition.y) * scale.y,
            });
        } else {
            position = message.position;
        }
        fireOutput(entry, dragPositionOutput);
    } else if (message.kind == MouseEventKind::ButtonUp) {
        state.positions[dragUpOutput] = accumulating ? position : message.position;
        state.source.reset();
        state.accumulator.reset();
        dispatch->drag.resetPointer |= std::get<bool>(readInput(entry.id, 2));
        fireOutput(entry, dragUpOutput);
        fireOutput(entry, draggingOutput);
    }
}

bool Schematic::clearMouseDrags(const yellow::PanelControlId& source)
{
    bool changed = false;
    for (auto& entry : nodes_) {
        if (entry.mouseDrag && entry.mouseDrag->source == source) {
            entry.mouseDrag->source.reset();
            entry.mouseDrag->accumulator.reset();
            fireOutput(entry, draggingOutput);
            changed = true;
        }
    }
    for (const auto& [id, child] : modules_) {
        changed |= child->clearMouseDrags(source);
    }
    return changed;
}

bool Schematic::cancelMouseDrag(const yellow::PanelControlId& source)
{
    if (!rootGraph().clearMouseDrags(source)) {
        return false;
    }
    refreshConnections();
    return true;
}

void Schematic::rebaseMouseDrag(const yellow::PanelControlId& source, Point position)
{
    rootGraph().rebaseMouseDrags(source, position);
}

void Schematic::rebaseMouseDrags(const yellow::PanelControlId& source, Point position)
{
    for (auto& entry : nodes_) {
        if (entry.mouseDrag && entry.mouseDrag->source == source && entry.mouseDrag->accumulator) {
            auto& accumulator = *entry.mouseDrag->accumulator;
            accumulator.anchorValue = entry.mouseDrag->positions[dragPositionOutput];
            accumulator.anchorPosition = position;
        }
    }
    for (const auto& [id, child] : modules_) {
        child->rebaseMouseDrags(source, position);
    }
}

std::int64_t Schematic::rubyCursorBalance() const
{
    std::int64_t balance = 0;
    for (const auto& entry : nodes_) {
        if (const auto& code = entry.rubyCode; code && code->running) {
            balance += code->cursorBalance;
            if (code->mouseCapture) balance += code->mouseCapture->cursorBalance;
        }
    }
    for (const auto& [id, child] : modules_) balance += child->rubyCursorBalance();
    return balance;
}

CursorVisibility Schematic::cursorVisibility() const
{
    auto state = nativeCursorVisibility();
    state.hidden |= rootGraph().rubyCursorBalance() < 0;
    return state;
}

CursorVisibility Schematic::nativeCursorVisibility() const
{
    const auto& root = rootGraph();
    const auto revision = root.cursorVisibilityRevision_;
    const auto& hidden = root.hiddenCursor_;
    if (!hidden) {
        return {.revision = revision};
    }
    const auto* owner = &root;
    for (const auto module : hidden->source.ownerPath) {
        owner = owner->module(module);
        if (!owner) {
            return {.revision = revision};
        }
    }

    const auto id = hidden->source.node;
    const auto source = std::ranges::find(owner->nodes_, id, &SchematicNode::id);
    if (source == owner->nodes_.end() || source->kind != PrimitiveKind::ShowCursor) {
        return {.revision = revision};
    }
    const bool wired = std::ranges::any_of(owner->links_, [&](const auto& link) {
        return link.target == id && link.input == 0 &&
               owner->outputType(link.source, link.output).value_or(green::Type::Trigger) != green::Type::Trigger;
    });
    return {.hidden = wired, .hold = wired && hidden->hold, .revision = revision};
}

void Schematic::receiveShowCursor(int id)
{
    auto& root = rootGraph();
    const bool hide = !std::get<bool>(readInput(id, 0));
    if (hide == root.nativeCursorVisibility().hidden) {
        return;
    }
    ++root.cursorVisibilityRevision_;
    if (hide) {
        root.hiddenCursor_ = HiddenCursor{{path_, id}, std::get<bool>(readInput(id, 1))};
    } else {
        root.hiddenCursor_.reset();
    }
}

bool Schematic::movePanelPointer(int panelId, green::Point position)
{
    const auto panel = std::ranges::find(nodes_, panelId, &SchematicNode::id);
    if (panel == nodes_.end() || !ownsPanel(panel->kind)) {
        return false;
    }
    const auto view = readPanel(panelId).view;
    if (!view.mouseMoves || !contains(view.bounds, position) ||
        !rootGraph().publishMouseMove(view, {MouseEventKind::Move, position})) {
        return false;
    }
    refreshConnections();
    return true;
}

bool Schematic::publishMouseMove(const yellow::View& target, const MouseMessage& message)
{
    bool delivered = false;
    for (auto& entry : nodes_) {
        if (entry.kind != PrimitiveKind::MouseMove) {
            continue;
        }
        const auto view = std::get<yellow::View>(readInput(entry.id, 0));
        if (!view.isSamePanel(target)) {
            continue;
        }
        auto local = message;
        local.position = view.mapping.fromPanel(message.position);
        if (!view.mapping.contains(local.position) || entry.lastMouseMessage == local) {
            continue;
        }
        entry.lastMouseMessage = local;
        fireOutput(entry, 0);
        delivered = true;
    }
    for (const auto& [id, child] : modules_) {
        delivered |= child->publishMouseMove(target, message);
    }
    return delivered;
}

}
