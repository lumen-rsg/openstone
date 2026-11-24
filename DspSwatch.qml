pragma ComponentBehavior: Bound

import QtQuick

Rectangle {
    id: swatch

    required property var samples
    required property string paletteName
    readonly property var colors: Theme.connectorPalettes[paletteName]

    implicitHeight: 90
    radius: Theme.controlRadius
    color: colors.fill
    border.color: colors.rim
    Accessible.name: "Signal waveform"

    Canvas {
        id: plot
        objectName: "dspWaveform"
        anchors.fill: parent
        anchors.margins: 6

        onAvailableChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            const values = swatch.samples
            let peak = 0
            for (const sample of values) {
                if (Number.isFinite(sample))
                    peak = Math.max(peak, Math.abs(sample))
            }
            peak = peak || 1
            const margin = 2
            const span = Math.max(0, width - 2 * margin)
            const middle = height / 2
            const gain = Math.max(0, middle - margin)

            ctx.strokeStyle = swatch.colors.rim
            ctx.globalAlpha = 0.4
            ctx.lineWidth = 1
            ctx.beginPath()
            ctx.moveTo(margin, middle)
            ctx.lineTo(width - margin, middle)
            ctx.stroke()

            ctx.globalAlpha = 1
            ctx.strokeStyle = swatch.colors.text
            ctx.lineWidth = 1.5
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.beginPath()
            let previous = false
            for (let i = 0; i < values.length; ++i) {
                if (!Number.isFinite(values[i])) {
                    previous = false
                    continue
                }
                const x = values.length > 1 ? margin + span * i / (values.length - 1) : width / 2
                const y = middle - values[i] / peak * gain
                if (previous) {
                    ctx.lineTo(x, y)
                } else {
                    ctx.moveTo(x, y)
                    ctx.lineTo(x + 0.01, y)
                }
                previous = true
            }
            ctx.stroke()
        }
    }

    onSamplesChanged: plot.requestPaint()
    onPaletteNameChanged: plot.requestPaint()
}
