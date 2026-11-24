pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: badge
    required property var port
    property bool highlighted: false

    readonly property bool pulse: port.type === "Trigger"
    readonly property bool untyped: port.type === "Template"
    readonly property bool array: port.glyph.endsWith("[]")
    readonly property color rim: untyped ? Theme.mutedText : port.bus ? Theme.secondaryText
        : pulse ? Theme.amber : port.yellow ? Theme.yellow : Theme.green

    Loader {
        anchors.fill: parent
        active: !!badge.port.palette
        sourceComponent: SignalConnectorIcon {
            connector: badge.port
            highlighted: badge.highlighted
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: !badge.port.palette
        radius: width * (badge.array ? 0.29 : 0.5)
        antialiasing: true
        color: badge.untyped ? Theme.field : badge.port.bus ? Theme.hover : badge.pulse ? Theme.amberSurface
            : badge.port.yellow ? Theme.yellowSurface : Theme.greenSurface
        border.color: badge.rim
        border.width: badge.highlighted ? 1.5 : 1

        Text {
            anchors.centerIn: parent
            text: badge.port.type.startsWith("Point") ? "⊙" : badge.array ? badge.port.glyph.slice(0, -2) : badge.port.glyph
            color: badge.rim
            font.pixelSize: parent.width * 0.55
            font.bold: true
        }
    }
}
