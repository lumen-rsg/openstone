import QtQuick
import QtQuick.Shapes

Shape {
    id: wire
    required property point start
    required property point end
    property bool pulse: false
    property bool yellow: false
    property bool bus: false
    property string palette: ""
    property bool untyped: false
    property bool preview: false
    property bool straight: false
    property var connection
    readonly property real bend: Math.max(28, Math.abs(end.x - start.x) * 0.48)
    preferredRendererType: Shape.CurveRenderer

    function distanceTo(point: point, radius: real): real {
        if (straight) {
            const dx = end.x - start.x
            const dy = end.y - start.y
            const length = dx * dx + dy * dy
            const t = length === 0 ? 0 : Math.max(0, Math.min(1,
                ((point.x - start.x) * dx + (point.y - start.y) * dy) / length))
            return Math.hypot(point.x - start.x - t * dx, point.y - start.y - t * dy)
        }
        const x1 = start.x + bend
        const x2 = end.x - bend
        if (point.x < Math.min(start.x, end.x, x1, x2) - radius ||
                point.x > Math.max(start.x, end.x, x1, x2) + radius ||
                point.y < Math.min(start.y, end.y) - radius ||
                point.y > Math.max(start.y, end.y) + radius)
            return Infinity

        const segments = Math.min(128, Math.max(16, Math.ceil(
            (Math.abs(end.x - start.x) + Math.abs(end.y - start.y) + 2 * bend) / (2 * radius))))
        let nearest = Infinity
        let previousX = start.x
        let previousY = start.y
        // Measure against the displayed Bézier instead of its bounding box.
        for (let i = 1; i <= segments; ++i) {
            const t = i / segments
            const u = 1 - t
            const x = u * u * u * start.x + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * end.x
            const y = u * u * (1 + 2 * t) * start.y + t * t * (3 - 2 * t) * end.y
            const dx = x - previousX
            const dy = y - previousY
            const lengthSquared = dx * dx + dy * dy
            const along = lengthSquared === 0 ? 0 : Math.max(0, Math.min(1,
                ((point.x - previousX) * dx + (point.y - previousY) * dy) / lengthSquared))
            nearest = Math.min(nearest, Math.hypot(point.x - previousX - along * dx,
                                                   point.y - previousY - along * dy))
            previousX = x
            previousY = y
        }
        return nearest
    }

    ShapePath {
        strokeColor: wire.bus ? (wire.preview ? Theme.text : Theme.secondaryText)
                    : wire.untyped ? Theme.mutedText
                    : wire.pulse ? (wire.preview ? Theme.amberText : Theme.amber)
                    : wire.palette ? (wire.preview ? Theme.connectorPalettes[wire.palette].text
                                                   : Theme.connectorPalettes[wire.palette].rim)
                    : wire.yellow ? (wire.preview ? Theme.yellowText : Theme.yellow)
                                : (wire.preview ? Theme.greenText : Theme.green)
        strokeWidth: wire.bus ? 3 : wire.preview ? 1.7 : 2.25
        strokeStyle: wire.pulse || wire.preview ? ShapePath.DashLine : ShapePath.SolidLine
        dashPattern: [3, 2]
        capStyle: ShapePath.RoundCap
        fillColor: "transparent"
        startX: wire.start.x
        startY: wire.start.y

        PathCubic {
            control1X: wire.straight ? wire.start.x + (wire.end.x - wire.start.x) / 3 : wire.start.x + wire.bend
            control1Y: wire.straight ? wire.start.y + (wire.end.y - wire.start.y) / 3 : wire.start.y
            control2X: wire.straight ? wire.start.x + (wire.end.x - wire.start.x) * 2 / 3 : wire.end.x - wire.bend
            control2Y: wire.straight ? wire.start.y + (wire.end.y - wire.start.y) * 2 / 3 : wire.end.y
            x: wire.end.x
            y: wire.end.y
        }
    }
}
