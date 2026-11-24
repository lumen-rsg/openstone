pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: icon
    required property var connector
    property bool highlighted: false

    readonly property var colors: Theme.connectorPalettes[connector.palette]
    readonly property bool memory: connector.shape === "memory" || connector.shape === "memoryArray"
    readonly property bool stacked: connector.shape === "memoryArray"
    readonly property bool array: connector.shape === "array"
    readonly property bool doublePrecision: connector.shape === "double"
    readonly property color fill: highlighted ? colors.hover : colors.fill

    Rectangle {
        visible: icon.stacked
        width: icon.width - 6
        height: icon.height * 0.49
        x: 2
        y: (icon.height - height) / 2 - 2
        radius: 2
        color: icon.fill
        border.color: icon.colors.rim
        antialiasing: true
    }

    Rectangle {
        width: icon.stacked ? icon.width - 6 : icon.memory || icon.array ? icon.width - 2 : icon.width
        height: icon.memory ? icon.height * 0.49 : (icon.array ? icon.height - 2 : icon.height)
        x: (icon.width - width) / 2 + (icon.stacked ? 2 : 0)
        y: (icon.height - height) / 2 + (icon.stacked ? 2 : 0)
        radius: icon.memory ? 2 : icon.array ? 6 : width / 2
        topLeftRadius: icon.doublePrecision ? 3 : radius
        bottomLeftRadius: topLeftRadius
        color: icon.fill
        border.color: icon.colors.rim
        border.width: icon.highlighted ? 2 : 1
        antialiasing: true
    }

    Repeater {
        model: icon.array ? 4 : 0
        Rectangle {
            required property int index
            x: index % 2 === 0 ? 0 : icon.width - width
            y: index < 2 ? 0 : icon.height - height
            width: 5
            height: width
            radius: width / 2
            color: icon.colors.rim
            antialiasing: true
        }
    }

    Grid {
        visible: icon.connector.glyph === "∷"
        anchors.centerIn: parent
        columns: 2
        spacing: 2
        Repeater {
            model: 4
            Rectangle {
                width: 2.5
                height: width
                radius: width / 2
                color: icon.colors.text
                antialiasing: true
            }
        }
    }

    Text {
        visible: !icon.memory && icon.connector.glyph !== "∷"
        anchors.centerIn: parent
        text: icon.connector.glyph
        color: icon.colors.text
        font.pixelSize: Math.min(12, Math.round(icon.width * 0.57))
        font.bold: true
    }
}
