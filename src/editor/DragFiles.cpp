#include "Schematic.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace openstone {
namespace {

constexpr int mouseMessagesInput = 0;
constexpr int filesInput = 1;
constexpr int modeInput = 2;
constexpr int draggingOutput = 0;
constexpr int droppedOutput = 1;

FileDragMode fileDragMode(std::string text)
{
    std::ranges::transform(text, text.begin(), [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    });
    if (text == "move") {
        return FileDragMode::Move;
    }
    if (text == "link") {
        return FileDragMode::Link;
    }
    return FileDragMode::Copy;
}

}

green::Value Schematic::readDragFiles(const SchematicNode& entry, int output) const
{
    if (output == droppedOutput) {
        return entry.fileDropSucceeded;
    }
    const auto& session = rootGraph().dragFilesSession_;
    return session && session->dragging && session->owner.node == entry.id && session->owner.ownerPath == path_;
}

void Schematic::receiveDragFilesTrigger(SchematicNode& entry, int input)
{
    auto& root = rootGraph();
    auto* dispatch = root.mouseDispatch_;
    if (input != mouseMessagesInput || root.dragFilesSession_ || !dispatch ||
        dispatch->message.kind != yellow::MouseEventKind::ButtonDown ||
        dispatch->message.button != yellow::MouseButton::Left ||
        std::get<yellow::MouseMessage>(readInput(entry.id, mouseMessagesInput)) != dispatch->message) {
        return;
    }

    entry.fileDropSucceeded = false;
    auto paths = std::get<green::StringArray>(readInput(entry.id, filesInput));
    if (paths.empty()) {
        fireOutput(entry, droppedOutput);
        return;
    }
    const auto mode = fileDragMode(std::get<std::string>(readInput(entry.id, modeInput)));
    const auto token = ++root.nextDragFilesToken_;
    root.dragFilesSession_ = DragFilesSession{token, {path_, entry.id}};
    dispatch->drag.fileDrag = DragFilesRequest{token, std::move(paths), mode};
}

Schematic* Schematic::dragFilesOwner(const DragFilesSession& session)
{
    auto* owner = &rootGraph();
    for (const int moduleId : session.owner.ownerPath) {
        owner = owner->module(moduleId);
        if (!owner) {
            return nullptr;
        }
    }
    const auto entry = std::ranges::find(owner->nodes_, session.owner.node, &SchematicNode::id);
    return entry != owner->nodes_.end() && entry->kind == PrimitiveKind::DragFiles ? owner : nullptr;
}

bool Schematic::beginDragFiles(std::uint64_t token)
{
    auto& session = rootGraph().dragFilesSession_;
    if (!session || session->token != token || session->dragging) {
        return false;
    }
    auto* owner = dragFilesOwner(*session);
    if (!owner) {
        session.reset();
        return false;
    }
    session->dragging = true;
    owner->fireOutput(owner->node(session->owner.node), draggingOutput);
    owner->refreshConnections();
    return true;
}

void Schematic::finishDragFiles(std::uint64_t token, bool dropped)
{
    auto& pending = rootGraph().dragFilesSession_;
    if (!pending || pending->token != token) {
        return;
    }
    const auto session = std::exchange(pending, std::nullopt);
    auto* owner = dragFilesOwner(*session);
    if (!owner) {
        return;
    }

    auto& entry = owner->node(session->owner.node);
    entry.fileDropSucceeded = session->dragging && dropped;
    if (session->dragging) {
        owner->fireOutput(entry, draggingOutput);
    }
    owner->fireOutput(entry, droppedOutput);
    owner->refreshConnections();
}

}
