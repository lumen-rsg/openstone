pragma ComponentBehavior: Bound

import QtQuick

DropArea {
    id: target

    required property SchematicModel graph
    required property var drawing
    required property real pixelsPerUnit
    property var dragOwner: null
    readonly property string controlKey: drawing?.key ?? ""

    function positionInView(event: DragEvent): point {
        return Qt.point(drawing.area.x + event.x / pixelsPerUnit,
                        drawing.area.y + event.y / pixelsPerUnit)
    }

    function track(event: DragEvent, dropped: bool): bool {
        if (!drawing || !event.hasUrls || !(event.supportedActions & Qt.CopyAction))
            return false
        if (!graph.trackFileDrag(drawing.path, drawing.node, event.urls, positionInView(event), dropped))
            return false
        event.accept(Qt.CopyAction)
        return true
    }

    function leave(deferred: bool) {
        const owner = dragOwner
        dragOwner = null
        if (owner && graph) {
            if (deferred)
                Qt.callLater(graph.leaveFileDrag, owner.path, owner.id)
            else
                graph.leaveFileDrag(owner.path, owner.id)
        }
    }

    onEntered: event => {
        const owner = { key: controlKey, path: drawing?.path, id: drawing?.node }
        if (track(event, false))
            dragOwner = owner
        else
            event.accepted = false
    }
    onPositionChanged: event => {
        if (dragOwner?.key !== controlKey)
            return
        if (drawing.draggingOver) {
            graph.moveFileDrag(dragOwner.path, dragOwner.id, positionInView(event))
            event.accept(Qt.CopyAction)
        } else {
            event.accepted = track(event, false)
        }
    }
    onExited: leave(false)
    onDropped: event => {
        if (dragOwner?.key === controlKey && track(event, true))
            dragOwner = null
        else {
            event.accepted = false
            leave(false)
        }
    }

    onControlKeyChanged: leave(true)
    onVisibleChanged: {
        if (!visible)
            leave(true)
    }
    Component.onDestruction: leave(true)

    Rectangle {
        anchors.fill: parent
        visible: target.drawing?.draggingOver === true
        radius: Theme.controlRadius
        color: Qt.rgba(Theme.green.r, Theme.green.g, Theme.green.b, 0.12)
        border.color: Theme.green
    }
}
