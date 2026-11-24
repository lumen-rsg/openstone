pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Rectangle {
    id: surface

    required property var panel
    required property real pixelsPerUnit
    required property SchematicModel graph
    property real canvasZoom: 1
    property bool editing: false
    property bool schematicChrome: true
    property bool bitmapTarget: false
    property DropListPopup dropListHost
    readonly property rect bounds: panel.bounds ?? Qt.rect(0, 0, 0, 0)
    readonly property var drawings: panel.drawings ?? []
    readonly property var rubyMouseAreas: {
        const areas = []
        for (let i = 0; i < drawings.length; ++i) {
            if (drawings[i].rubyInput >= 0)
                areas.push({ drawing: drawings[i], layer: 2 * i + 1 })
        }
        return areas
    }
    readonly property var rubyEditDrawings: {
        const edits = {}
        for (const drawing of drawings) {
            if (drawing.rubySession)
                edits[drawing.rubySession] = drawing
        }
        return edits
    }
    readonly property string panelKey: panel.key ?? ""
    property string selectedItemKey: ""
    signal panelItemMoved(var itemPath, real x, real y, bool properties)
    signal panelItemAnchorsChanged(var itemPath, bool properties, int anchors)

    function syncRubyEdits() {
        if (!rubyEditModel)
            return
        for (let i = rubyEditModel.count - 1; i >= 0; --i) {
            if (!rubyEditDrawings[rubyEditModel.get(i).editToken])
                rubyEditModel.remove(i)
        }
        const current = new Set()
        for (let i = 0; i < rubyEditModel.count; ++i)
            current.add(rubyEditModel.get(i).editToken)
        for (const token of Object.keys(rubyEditDrawings)) {
            if (!current.has(token))
                rubyEditModel.append({ editToken: token })
        }
    }

    onRubyEditDrawingsChanged: syncRubyEdits()
    Component.onCompleted: syncRubyEdits()
    onPanelKeyChanged: selectedItemKey = ""
    onEditingChanged: {
        if (!editing)
            selectedItemKey = ""
    }

    radius: schematicChrome ? Theme.controlRadius : 0
    color: bitmapTarget ? "transparent" : Theme.canvas
    clip: true
    Accessible.name: editing ? "Front panel editor" : "Module front panel"

    Connections {
        target: surface.dropListHost
        function onFindAnchor(key, location) {
            if (surface.visible && surface.panel.key === key) {
                const x = (location.x - surface.bounds.x) * surface.pixelsPerUnit
                const y = (location.y - surface.bounds.y) * surface.pixelsPerUnit
                surface.dropListHost.anchorPoint = surface.mapToItem(surface.dropListHost.parent, x, y)
                surface.dropListHost.anchorFound = true
            }
        }
    }

    CanvasGrid {
        visible: surface.schematicChrome
        readonly property real panelZoom: surface.pixelsPerUnit / 24
        anchors.fill: parent
        zoom: panelZoom
        center: Qt.point(surface.bounds.x + width / (2 * panelZoom),
                         surface.bounds.y + height / (2 * panelZoom))
        dotColor: Theme.grid
    }

    MouseArea {
        anchors.fill: parent
        enabled: surface.editing
        onPressed: surface.selectedItemKey = ""
    }

    Repeater {
        model: surface.drawings.length
        delegate: PanelDrawing {
            required property int index
            readonly property var drawing: surface.drawings[index] ?? {}
            modelData: (drawing.rubyInput >= 0 || drawing.rubySession) ? {} : drawing
            bounds: surface.bounds
            graph: surface.graph
            dropListHost: surface.dropListHost
            anchors.fill: parent
            pixelsPerUnit: surface.pixelsPerUnit
            canvasZoom: surface.canvasZoom
            z: modelData.captured ? 1e6 : 2 * index + 1
        }
    }

    // A repaint can change the drawing count while Ruby holds a pointer grab.
    Repeater {
        model: surface.rubyMouseAreas.length
        delegate: PanelDrawing {
            required property int index
            modelData: surface.rubyMouseAreas[index]?.drawing ?? {}
            bounds: surface.bounds
            graph: surface.graph
            anchors.fill: parent
            pixelsPerUnit: surface.pixelsPerUnit
            canvasZoom: surface.canvasZoom
            z: surface.rubyMouseAreas[index]?.layer ?? 0
        }
    }

    ListModel {
        id: rubyEditModel
    }

    // Keep the focused field mounted when a callback changes the paint or opens another editor.
    Repeater {
        model: rubyEditModel
        delegate: PanelDrawing {
            required property string editToken
            modelData: surface.rubyEditDrawings[editToken] ?? {}
            bounds: surface.bounds
            graph: surface.graph
            anchors.fill: parent
            pixelsPerUnit: surface.pixelsPerUnit
            canvasZoom: surface.canvasZoom
            z: 1000001
        }
    }

    Repeater {
        model: surface.panel.items?.length ?? 0

        delegate: ViewClip {
            id: childMask
            required property int index
            readonly property var modelData: surface.panel.items?.[index]
            anchors.fill: parent
            regionData: modelData?.clip ?? null
            pixelsPerUnit: surface.pixelsPerUnit
            viewportZoom: surface.canvasZoom
            z: modelData?.layer ?? 0

            Item {
                id: child
                readonly property var modelData: childMask.modelData
                readonly property rect placement: modelData?.rect ?? Qt.rect(0, 0, 0, 0)
                readonly property real units: surface.pixelsPerUnit * (modelData?.scale ?? 1)
                readonly property string selectionKey: modelData ? modelData.path.join("/") + ":" + modelData.properties : ""
                readonly property bool selected: surface.editing && selectionKey === surface.selectedItemKey
                property point offset: Qt.point(0, 0)

                x: (placement.x - surface.bounds.x) * surface.pixelsPerUnit + offset.x
                y: (placement.y - surface.bounds.y) * surface.pixelsPerUnit + offset.y
                width: placement.width * surface.pixelsPerUnit
                height: placement.height * surface.pixelsPerUnit
                transformOrigin: Item.TopLeft
                rotation: modelData?.rotation ?? 0

                Loader {
                    anchors.fill: parent
                    z: 1
                    // Load descendants dynamically so the QML type can nest itself.
                    Component.onCompleted: setSource(Qt.resolvedUrl("FrontPanel.qml"), {
                        panel: Qt.binding(() => child.modelData?.panel ?? {}),
                        pixelsPerUnit: Qt.binding(() => child.units),
                        canvasZoom: Qt.binding(() => surface.canvasZoom),
                        schematicChrome: Qt.binding(() => surface.schematicChrome),
                        bitmapTarget: Qt.binding(() => surface.bitmapTarget),
                        graph: surface.graph,
                        dropListHost: Qt.binding(() => surface.dropListHost)
                    })
                }

                Canvas {
                    anchors.fill: parent
                    z: 2
                    visible: surface.editing
                    property color ink: child.selected ? Theme.focus : Theme.mutedText
                    onInkChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.strokeStyle = ink
                        ctx.lineWidth = child.selected ? 1.5 : 1
                        ctx.setLineDash([4, 3])
                        ctx.strokeRect(1, 1, Math.max(0, width - 2), Math.max(0, height - 2))
                    }
                }

                Label {
                    z: 2
                    x: 0
                    y: child.y + child.height + implicitHeight + 4 <= surface.height ? child.height + 3 : -implicitHeight - 3
                    width: Math.min(implicitWidth, surface.width)
                    visible: child.selected
                    leftPadding: 5
                    rightPadding: 5
                    topPadding: 2
                    bottomPadding: 2
                    text: child.modelData ? child.modelData.name + " · " +
                          Math.round(child.modelData.position.x * surface.pixelsPerUnit) + ", " +
                          Math.round(child.modelData.position.y * surface.pixelsPerUnit) + " px" : ""
                    elide: Text.ElideRight
                    color: Theme.text
                    font.pixelSize: 11
                    background: Rectangle {
                        radius: Theme.controlRadius
                        color: Theme.surface
                    }
                }

                PanelItemDrag {
                    anchors.fill: parent
                    z: 3
                }

                Repeater {
                    model: [
                        { bit: 1, arrow: "←", edge: "left", x: 0, y: 0.5 },
                        { bit: 2, arrow: "↑", edge: "top", x: 0.5, y: 0 },
                        { bit: 4, arrow: "→", edge: "right", x: 1, y: 0.5 },
                        { bit: 8, arrow: "↓", edge: "bottom", x: 0.5, y: 1 }
                    ]
                    delegate: FlatButton {
                        id: anchorButton
                        required property var modelData
                        readonly property int mask: child.modelData?.anchors ?? 3
                        objectName: "panel-anchor-" + modelData.edge
                        z: 4
                        visible: child.selected
                        width: 17
                        height: 17
                        padding: 0
                        x: (child.width - width) * modelData.x
                        y: (child.height - height) * modelData.y
                        text: modelData.arrow
                        Accessible.name: "Anchor to " + modelData.edge
                        checkable: true
                        checked: (mask & modelData.bit) !== 0
                        ToolTip.visible: hovered
                        ToolTip.delay: 350
                        ToolTip.text: Accessible.name
                        onClicked: surface.panelItemAnchorsChanged(child.modelData.path, child.modelData.properties,
                                                                  mask ^ modelData.bit)
                        contentItem: Label {
                            text: anchorButton.text
                            color: Theme.text
                            font.pixelSize: 13
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: width / 2
                            color: anchorButton.checked ? Theme.selection : Theme.surface
                            border.color: anchorButton.hovered || anchorButton.checked ? Theme.focus : Theme.outline
                        }
                    }
                }

                component PanelItemDrag: MouseArea {
                    id: handle
                    enabled: surface.editing
                    hoverEnabled: enabled
                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    Accessible.name: "Move " + (child.modelData?.name ?? "") + " on front panel"
                    property point grab

                    onPressed: mouse => {
                        surface.selectedItemKey = child.selectionKey
                        grab = handle.mapToItem(surface, mouse.x, mouse.y)
                    }
                    onPositionChanged: mouse => {
                        if (pressed) {
                            const p = handle.mapToItem(surface, mouse.x, mouse.y)
                            // Keep at least one unit inside the panel so the component stays reachable.
                            const k = surface.pixelsPerUnit
                            const visual = child.mapToItem(surface, Qt.rect(0, 0, child.width, child.height))
                            const left = visual.x - child.offset.x
                            const top = visual.y - child.offset.y
                            child.offset = Qt.point(
                                Math.max(k - left - visual.width, Math.min(p.x - grab.x, surface.width - k - left)),
                                Math.max(k - top - visual.height, Math.min(p.y - grab.y, surface.height - k - top)))
                        }
                    }
                    onReleased: mouse => {
                        const shift = Qt.point(child.offset.x, child.offset.y)
                        child.offset = Qt.point(0, 0)
                        if (Math.hypot(shift.x, shift.y) < 0.5)
                            return
                        const modifier = Qt.ControlModifier | (Qt.platform.os === "osx" ? Qt.MetaModifier : 0)
                        const fine = (mouse.modifiers & modifier) !== 0
                        const zoom = surface.canvasZoom
                        const offset = fine ? Qt.point(Math.round(shift.x * zoom) / zoom,
                                                       Math.round(shift.y * zoom) / zoom) : shift
                        const at = child.mapFromItem(surface, offset)
                        const base = child.mapFromItem(surface, 0, 0)
                        const px = child.modelData.position.x
                        const py = child.modelData.position.y
                        const dx = (at.x - base.x) / child.units
                        const dy = (at.y - base.y) / child.units
                        const x = fine || Math.abs(dx) < 0.001 ? px + dx : Math.round(px + dx)
                        const y = fine || Math.abs(dy) < 0.001 ? py + dy : Math.round(py + dy)
                        if (x !== px || y !== py)
                            surface.panelItemMoved(child.modelData.path, x, y, child.modelData.properties)
                    }
                    onCanceled: child.offset = Qt.point(0, 0)
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        z: 2 * surface.drawings.length + 1
        visible: surface.schematicChrome && !surface.bitmapTarget && surface.panel.border === true
        radius: surface.radius
        color: "transparent"
        border.color: Theme.outline
    }

    HoverHandler {
        id: pointer
        enabled: surface.panel.mouseMoves === true
        onPointChanged: {
            if (hovered && point.pressedButtons === Qt.NoButton) {
                const position = Qt.point(surface.bounds.x + point.position.x / surface.pixelsPerUnit,
                                          surface.bounds.y + point.position.y / surface.pixelsPerUnit)
                surface.graph.movePanelPointer(surface.panel.path, surface.panel.node, position)
            }
        }
    }

    Label {
        z: 2 * surface.drawings.length + 2
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 4
        visible: surface.schematicChrome && pointer.hovered
        text: (surface.bounds.x + pointer.point.position.x / surface.pixelsPerUnit).toFixed(2) + ", " +
              (surface.bounds.y + pointer.point.position.y / surface.pixelsPerUnit).toFixed(2)
        color: Theme.secondaryText
        font.pixelSize: 10
        padding: 3
        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.surface
        }
    }
}
