pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes

Item {
    id: region

    // The panel supplies nonoverlapping pieces in its own grid coordinates.
    required property var regionData
    required property real pixelsPerUnit
    property real contentAngle: 0
    property point contentPivot: Qt.point(0, 0)
    property real viewportZoom: 1
    readonly property var polygonPieces: regionData?.pieces ?? null
    readonly property bool useMask: polygonPieces !== null && GraphicsInfo.api !== GraphicsInfo.Software &&
                                    GraphicsInfo.api !== GraphicsInfo.Unknown
    readonly property var rectangles: polygonPieces === null ? regionData : [regionData.limit]
    default property alias content: contents.data
    property alias sourceComponent: loader.sourceComponent
    readonly property var visibleRects: {
        if (rectangles === null)
            return null
        const result = []
        for (const rect of rectangles) {
            const left = Math.max(0, rect.x * pixelsPerUnit - x)
            const top = Math.max(0, rect.y * pixelsPerUnit - y)
            const right = Math.min(width, (rect.x + rect.width) * pixelsPerUnit - x)
            const bottom = Math.min(height, (rect.y + rect.height) * pixelsPerUnit - y)
            if (right > left && bottom > top)
                result.push(Qt.rect(left, top, right - left, bottom - top))
        }
        return result
    }
    readonly property int rectCount: visibleRects?.length ?? -1
    readonly property rect first: rectCount === 1 ? visibleRects[0] : Qt.rect(0, 0, 0, 0)
    visible: rectCount !== 0

    data: [
        Item {
            id: viewport
            x: region.first.x
            y: region.first.y
            width: region.rectCount === 1 ? region.first.width : region.width
            height: region.rectCount === 1 ? region.first.height : region.height
            clip: region.rectCount > 0
            containmentMask: region.polygonPieces !== null || region.rectCount > 1 ? hitRegion : null

            QtObject {
                id: hitRegion

                function contains(point: point): bool {
                    if (region.polygonPieces !== null) {
                        const at = Qt.point((point.x + viewport.x + region.x) / region.pixelsPerUnit,
                                            (point.y + viewport.y + region.y) / region.pixelsPerUnit)
                        return region.polygonPieces.some(polygon => polygon.every((vertex, i) => {
                            const previous = polygon[(i + polygon.length - 1) % polygon.length]
                            return (vertex.x - previous.x) * (at.y - previous.y) >=
                                   (vertex.y - previous.y) * (at.x - previous.x)
                        }))
                    }
                    return region.visibleRects.some(rect => point.x >= rect.x && point.x < rect.x + rect.width &&
                                                            point.y >= rect.y && point.y < rect.y + rect.height)
                }
            }

            Item {
                id: contents
                x: -viewport.x
                y: -viewport.y
                width: region.width
                height: region.height

                Loader {
                    id: loader
                    anchors.fill: parent
                    transform: Rotation {
                        origin.x: region.contentPivot.x
                        origin.y: region.contentPivot.y
                        angle: region.contentAngle
                    }
                }
            }
        },
        Loader {
            anchors.fill: parent
            active: region.useMask
            sourceComponent: polygonMask
        },
        // Share one live subtree across all the visible pieces.
        Repeater {
            model: region.rectCount > 1 ? region.rectCount : 0

            ShaderEffectSource {
                required property int index
                readonly property rect piece: region.visibleRects?.[index] ?? Qt.rect(0, 0, 0, 0)
                x: piece.x
                y: piece.y
                width: piece.width
                height: piece.height
                sourceItem: contents
                sourceRect: piece
                hideSource: true
            }
        }
    ]

    Component {
        id: polygonMask

        ShaderEffect {
            id: masked
            readonly property real pixelScale: Math.min(region.viewportZoom * Screen.devicePixelRatio,
                                                        4096 / Math.max(1, width, height))
            readonly property size rasterSize: Qt.size(Math.max(1, Math.ceil(width * pixelScale)),
                                                       Math.max(1, Math.ceil(height * pixelScale)))
            property Item source: capturedDrawing
            property Item mask: capturedMask
            fragmentShader: "qrc:/openstone/shaders/view-clip.frag.qsb"

            ShaderEffectSource {
                id: capturedDrawing
                anchors.fill: parent
                sourceItem: contents
                hideSource: true
                sourceRect: Qt.rect(0, 0, region.width, region.height)
                textureSize: masked.rasterSize
                visible: false
            }

            Shape {
                id: shape
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer

                ShapePath {
                    strokeWidth: -1
                    fillColor: "white"
                    fillRule: ShapePath.WindingFill
                    scale: Qt.size(region.pixelsPerUnit, region.pixelsPerUnit)
                    PathSvg { path: region.regionData?.path ?? "" }
                }
            }

            ShaderEffectSource {
                id: capturedMask
                anchors.fill: parent
                sourceItem: shape
                hideSource: true
                textureSize: masked.rasterSize
                visible: false
            }
        }
    }

}
