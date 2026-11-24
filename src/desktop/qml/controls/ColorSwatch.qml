import QtQuick

Item {
    id: swatch
    required property color fill

    Canvas {
        anchors.fill: parent
        visible: swatch.fill.a < 1
        onVisibleChanged: if (visible) requestPaint()
        onPaint: {
            const context = getContext("2d")
            context.fillStyle = "#a6a6a3"
            context.fillRect(0, 0, width, height)
            context.fillStyle = "#686865"
            const tile = 5
            for (let row = 0; row * tile < height; ++row) {
                for (let column = row % 2; column * tile < width; column += 2)
                    context.fillRect(column * tile, row * tile, tile, tile)
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: swatch.fill
    }
}
