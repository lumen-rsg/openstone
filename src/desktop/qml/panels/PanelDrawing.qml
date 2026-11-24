pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

ViewClip {
    id: draw

    required property var modelData
    required property rect bounds
    required property SchematicModel graph
    required property real canvasZoom
    property DropListPopup dropListHost
    readonly property rect area: modelData?.area ?? Qt.rect(0, 0, 0, 0)
    readonly property point origin: modelData?.origin ?? Qt.point(0, 0)
    readonly property real units: pixelsPerUnit * (modelData?.scale ?? 1)
    readonly property real originX: (origin.x - bounds.x) * pixelsPerUnit + area.x * units
    readonly property real originY: (origin.y - bounds.y) * pixelsPerUnit + area.y * units
    readonly property real spanX: area.width * units
    readonly property real spanY: area.height * units

    regionData: modelData?.captured ? null : (modelData?.clip ?? null)
    viewportZoom: draw.canvasZoom
    contentAngle: modelData?.rotation ?? 0
    contentPivot: Qt.point((origin.x - bounds.x) * pixelsPerUnit, (origin.y - bounds.y) * pixelsPerUnit)
    sourceComponent: {
        if (modelData?.brush !== undefined)
            return brushLayer
        switch (modelData?.kind) {
        case "bitmap": return bitmapLayer
        case "graph": return graphLayer
        case "text": return textLayer
        case "edit": return editLayer
        case "popupList": return popupListLayer
        case "dropFiles": return dropFilesLayer
        case "mouseArea": return mouseAreaLayer
        case "viewCache":
        case "viewEffect": return captureLayer
        case undefined: return null
        default: return shapeLayer
        }
    }

    Component {
        id: captureLayer
        Item {
            id: capture
            readonly property var commands: draw.modelData?.commands ?? []
            readonly property real pixelScale: draw.canvasZoom * Screen.devicePixelRatio
            readonly property real scaleLimit: Math.min(pixelScale, 4096 / Math.max(draw.spanX, draw.spanY, 1))
            readonly property size rasterSize: Qt.size(Math.max(1, Math.ceil(draw.spanX * scaleLimit)),
                                                       Math.max(1, Math.ceil(draw.spanY * scaleLimit)))

            Item {
                id: captureContent
                x: draw.originX
                y: draw.originY
                width: draw.spanX
                height: draw.spanY
                clip: true

                Repeater {
                    model: capture.commands.length
                    delegate: Loader {
                        id: child
                        required property int index
                        anchors.fill: parent
                        z: index

                        // Capture nodes can nest, so load each child dynamically.
                        Component.onCompleted: setSource(Qt.resolvedUrl("PanelDrawing.qml"), {
                            modelData: Qt.binding(() => capture.commands[child.index] ?? {}),
                            bounds: Qt.binding(() => draw.area),
                            graph: draw.graph,
                            pixelsPerUnit: Qt.binding(() => draw.pixelsPerUnit),
                            canvasZoom: Qt.binding(() => draw.canvasZoom)
                        })
                    }
                }
            }

            Loader {
                anchors.fill: captureContent
                sourceComponent: draw.modelData.kind === "viewCache" ? cachedTexture : effectTexture
            }

            Component {
                id: cachedTexture
                ShaderEffectSource {
                    readonly property string revision: draw.modelData?.revision ?? ""
                    sourceItem: captureContent
                    hideSource: true
                    live: false
                    textureSize: capture.rasterSize
                    onRevisionChanged: scheduleUpdate()
                    onTextureSizeChanged: scheduleUpdate()
                    Component.onCompleted: scheduleUpdate()
                }
            }

            Component {
                id: effectTexture
                PanelViewEffect {
                    contentItem: captureContent
                    options: draw.modelData.effect ?? {}
                    textureSize: capture.rasterSize
                }
            }
        }
    }

    Component {
        id: brushLayer
        Item {
            id: brushSurface

            // Crop the raster to the visible panel in the drawing's unrotated coordinates.
            readonly property rect visibleBounds: {
                if (draw.contentAngle === 0)
                    return Qt.rect(0, 0, draw.width, draw.height)
                const pivot = draw.contentPivot
                const radians = -draw.contentAngle * Math.PI / 180
                const cosine = Math.cos(radians)
                const sine = Math.sin(radians)
                const corners = [[0, 0], [draw.width, 0], [draw.width, draw.height], [0, draw.height]]
                const x = corners.map(p => pivot.x + cosine * (p[0] - pivot.x) - sine * (p[1] - pivot.y))
                const y = corners.map(p => pivot.y + sine * (p[0] - pivot.x) + cosine * (p[1] - pivot.y))
                return Qt.rect(Math.min(...x), Math.min(...y), Math.max(...x) - Math.min(...x), Math.max(...y) - Math.min(...y))
            }

            PanelBrushFill {
                x: Math.max(brushSurface.visibleBounds.x, draw.originX)
                y: Math.max(brushSurface.visibleBounds.y, draw.originY)
                width: Math.max(0, Math.min((brushSurface.visibleBounds.x + brushSurface.visibleBounds.width), draw.originX + draw.spanX) - x)
                height: Math.max(0, Math.min((brushSurface.visibleBounds.y + brushSurface.visibleBounds.height), draw.originY + draw.spanY) - y)
                viewOrigin: Qt.point(draw.area.x + (x - draw.originX) / draw.units,
                                     draw.area.y + (y - draw.originY) / draw.units)
                drawing: draw.modelData
                pixelsPerUnit: draw.units
                rasterScale: draw.canvasZoom * Screen.devicePixelRatio
            }
        }
    }

    Component {
        id: bitmapLayer
        Item {
            PanelBitmap {
                x: draw.originX
                y: draw.originY
                width: draw.spanX
                height: draw.spanY
                drawing: draw.modelData
                opacity: draw.modelData.alpha
            }
        }
    }

    Component {
        id: textLayer
        Item {
            PanelText {
                x: draw.originX
                y: draw.originY
                width: draw.modelData.pointOrigin ? draw.width : draw.spanX
                height: draw.modelData.pointOrigin ? draw.height : draw.spanY
                drawing: draw.modelData
                pixelsPerUnit: draw.units
                Accessible.role: Accessible.StaticText
                Accessible.name: draw.modelData.text
            }
        }
    }

    Component {
        id: editLayer
        Item {
            PanelEditControl {
                x: draw.originX
                y: draw.originY
                width: draw.spanX
                height: draw.spanY
                drawing: draw.modelData
                graph: draw.graph
                pixelsPerUnit: draw.units
            }
        }
    }

    Component {
        id: popupListLayer
        Item {
            PanelPopupList {
                x: draw.originX
                y: draw.originY
                width: draw.spanX
                height: draw.spanY
                drawing: draw.modelData
                graph: draw.graph
                menuHost: draw.dropListHost
            }
        }
    }

    Component {
        id: dropFilesLayer
        Item {
            PanelFileDrop {
                x: draw.originX
                y: draw.originY
                width: draw.spanX
                height: draw.spanY
                drawing: draw.modelData
                graph: draw.graph
                pixelsPerUnit: draw.units
            }
        }
    }

    Component {
        id: mouseAreaLayer
        Item {
            MouseArea {
                id: mouseArea
                readonly property var polygon: draw.modelData?.polygon ?? null
                readonly property string tip: draw.modelData?.tip ?? ""
                readonly property int rubyInput: draw.modelData?.rubyInput ?? -1
                readonly property string sourceKey: (draw.modelData?.key ?? "") + ":" + rubyInput
                readonly property bool captured: draw.modelData?.captured === true
                property var rubyPress: null
                property int rubyCursor: Qt.PointingHandCursor
                x: captured ? 0 : draw.originX
                y: captured ? 0 : draw.originY
                width: captured ? draw.width : draw.spanX
                height: captured ? draw.height : draw.spanY
                acceptedButtons: Qt.LeftButton | Qt.RightButton | (rubyInput < 0 ? Qt.MiddleButton : Qt.NoButton)
                preventStealing: rubyInput >= 0
                hoverEnabled: true
                cursorShape: rubyInput >= 0 ? rubyCursor : draw.modelData?.cursor
                containmentMask: captured || (polygon === null && rubyInput < 0) ? null : regionMask
                ToolTip.text: tip
                ToolTip.delay: 500
                ToolTip.visible: containsMouse && !pressed && tip.length > 0

                QtObject {
                    id: regionMask

                    function contains(point: point): bool {
                        if (point.x < 0 || point.y < 0 || point.x > mouseArea.width || point.y > mouseArea.height)
                            return false
                        return mouseArea.rubyInput >= 0
                            ? draw.graph.rubyMouseContains(draw.modelData.path, draw.modelData.node,
                                                          mouseArea.rubyInput, mouseArea.panelPoint(point.x, point.y))
                            : draw.graph.mousePolygonContains(mouseArea.polygon, mouseArea.viewPoint(point.x, point.y))
                    }
                }

                function viewPoint(x: real, y: real): point {
                    return Qt.point(draw.area.x + (x + mouseArea.x - draw.originX) / draw.units,
                                    draw.area.y + (y + mouseArea.y - draw.originY) / draw.units)
                }

                function panelPoint(x: real, y: real): point {
                    const point = mouseArea.mapToItem(draw, x, y)
                    return Qt.point(draw.bounds.x + point.x / draw.pixelsPerUnit,
                                    draw.bounds.y + point.y / draw.pixelsPerUnit)
                }

                function updateRubyCursor(x: real, y: real) {
                    if (rubyInput < 0 || pressed)
                        return
                    const cursor = draw.graph.rubyMouseCursor(draw.modelData.path, draw.modelData.node, rubyInput, panelPoint(x, y))
                    rubyCursor = cursor >= 0 ? cursor : Qt.PointingHandCursor
                }

                function cancel() {
                    if (rubyPress !== null) {
                        draw.graph.cancelRubyMouseDrag(rubyPress.path, rubyPress.node, rubyPress.input)
                        rubyPress = null
                    }
                    draw.graph.cancelMouseDrag(mouseArea)
                }

                function send(kind: int, x: real, y: real, buttons: int, delta: point): bool {
                    const screenPosition = kind === SchematicModel.PointerDown || kind === SchematicModel.PointerDrag
                        ? mouseArea.mapToGlobal(x, y) : Qt.point(0, 0)
                    const position = rubyInput >= 0 ? panelPoint(x, y) : viewPoint(x, y)
                    return draw.graph.sendMouseMessage(draw.modelData.path, draw.modelData.node,
                                                       kind, position, buttons, delta,
                                                       mouseArea, screenPosition, draw.units, rubyInput)
                }

                onPressed: mouse => {
                    if (rubyInput >= 0)
                        rubyPress = { path: draw.modelData.path, node: draw.modelData.node, input: rubyInput }
                    mouse.accepted = send(SchematicModel.PointerDown, mouse.x, mouse.y, mouse.button,
                                          Qt.point(0, 0))
                    if (!mouse.accepted)
                        rubyPress = null
                }
                onReleased: mouse => {
                    send(SchematicModel.PointerUp, mouse.x, mouse.y, mouse.button, Qt.point(0, 0))
                    if (mouse.buttons === Qt.NoButton)
                        rubyPress = null
                    updateRubyCursor(mouse.x, mouse.y)
                }
                onEntered: updateRubyCursor(mouseX, mouseY)
                onCanceled: cancel()
                onSourceKeyChanged: {
                    cancel()
                    rubyCursor = Qt.PointingHandCursor
                }
                Component.onDestruction: {
                    if (rubyPress && draw.graph)
                        cancel()
                }
                onClicked: mouse => send(SchematicModel.PointerClick, mouse.x, mouse.y, mouse.button, Qt.point(0, 0))
                onDoubleClicked: mouse => {
                    send(SchematicModel.PointerDoubleClick, mouse.x, mouse.y, mouse.button, Qt.point(0, 0))
                    // Keep the press and release signals for the second click.
                    mouse.accepted = false
                }
                onPositionChanged: mouse => {
                    if (pressed || captured || draw.modelData?.mouseMoves === true) {
                        const kind = pressed ? SchematicModel.PointerDrag : SchematicModel.PointerMove
                        send(kind, mouse.x, mouse.y, mouse.buttons, Qt.point(0, 0))
                    }
                    updateRubyCursor(mouse.x, mouse.y)
                }
                onWheel: wheel => {
                    wheel.accepted = (wheel.angleDelta.x !== 0 || wheel.angleDelta.y !== 0) &&
                        send(SchematicModel.PointerWheel, wheel.x, wheel.y, Qt.NoButton, wheel.angleDelta)
                }
            }
        }
    }

    Component {
        id: graphLayer
        Item {
            Item {
                x: draw.originX
                y: draw.originY
                width: draw.spanX
                height: draw.spanY
                clip: draw.modelData.clipToArea

                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        scale: Qt.size(draw.units, draw.units)
                        strokeColor: draw.modelData.stroke
                        strokeWidth: draw.modelData.width > 0 ? draw.modelData.width * draw.units
                                                              : 1 / draw.canvasZoom
                        fillColor: draw.modelData.fill
                        fillRule: draw.modelData.closed ? ShapePath.OddEvenFill : ShapePath.WindingFill
                        capStyle: ShapePath.FlatCap
                        joinStyle: ShapePath.RoundJoin
                        strokeStyle: draw.modelData.dashes.length ? ShapePath.DashLine : ShapePath.SolidLine
                        dashPattern: draw.modelData.dashes.length ? draw.modelData.dashes : [4, 2]
                        PathSvg { path: draw.modelData.path }
                    }
                }
            }
        }
    }

    Component {
        id: shapeLayer
        Shape {
            id: shape
            readonly property real centerX: draw.originX + draw.spanX / 2
            readonly property real centerY: draw.originY + draw.spanY / 2
            readonly property bool sector: draw.modelData.kind === "pie" && Math.abs(draw.modelData.sweep) < 360
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                strokeColor: draw.modelData.stroke
                strokeWidth: draw.modelData.width > 0 ? draw.modelData.width * draw.units : 1 / draw.canvasZoom
                fillColor: draw.modelData.fill
                capStyle: ShapePath.FlatCap
                joinStyle: ShapePath.MiterJoin
                strokeStyle: draw.modelData.dashes.length ? ShapePath.DashLine : ShapePath.SolidLine
                dashPattern: draw.modelData.dashes.length ? draw.modelData.dashes : [4, 2]
                startX: shape.sector ? shape.centerX : draw.originX
                startY: shape.sector ? shape.centerY : draw.originY
                pathElements: {
                    switch (draw.modelData.kind) {
                    case "line": return [lineSegment]
                    case "rect": return [rectSegment]
                    default: return shape.sector ? [curveSegment, lineSegment] : [curveSegment]
                    }
                }

                property PathLine lineSegment: PathLine {
                    x: shape.sector ? shape.centerX : draw.originX + draw.spanX
                    y: shape.sector ? shape.centerY : draw.originY + draw.spanY
                }
                property PathAngleArc curveSegment: PathAngleArc {
                    centerX: shape.centerX
                    centerY: shape.centerY
                    radiusX: draw.spanX / 2
                    radiusY: draw.spanY / 2
                    startAngle: draw.modelData.start
                    sweepAngle: draw.modelData.sweep
                    moveToStart: !shape.sector
                }
                property PathRectangle rectSegment: PathRectangle {
                    x: draw.originX
                    y: draw.originY
                    width: draw.spanX
                    height: draw.spanY
                    radius: draw.modelData.corner * draw.units
                }
            }
        }
    }
}
