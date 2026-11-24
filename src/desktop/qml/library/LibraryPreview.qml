pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: preview
    required property var entry
    readonly property real connectorSize: Math.max(6, Math.min(12,
        body.height / (Math.max(entry.inputs.length, entry.outputs.length) + 1)))

    Accessible.ignored: true

    Rectangle {
        id: body
        x: 14
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(preview.width - 28, Math.max(40, Math.min(114, preview.entry.width * 0.58)))
        height: Math.min(preview.height - 8, Math.max(21, preview.entry.height * 0.55))
        radius: 5
        border.color: Theme.outline
        color: Theme.hover

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            y: body.height > 30 && !preview.entry.operatorNode ? 3 : (body.height - height) / 2
            width: parent.width - 15
            text: preview.entry.module ? "Module" : preview.entry.dspNode ? "DSP Code" : preview.entry.symbol
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            color: preview.entry.operatorNode ? Theme.green : Theme.text
            font.pixelSize: preview.entry.operatorNode ? 13 : 9
            font.bold: true
        }

        Rectangle {
            visible: (preview.entry.valueField || preview.entry.triggerNode || preview.entry.dspNode) && body.height > 30
            x: 9
            y: 16
            width: body.width - 18
            height: body.height - 22
            radius: 3
            color: preview.entry.triggerNode ? Theme.amberSurface : Theme.field
            border.color: Theme.divider
        }

        Repeater {
            model: preview.entry.inputs
            LibraryConnector {
                required property var modelData
                port: modelData
                width: preview.connectorSize
                height: width
                x: -width / 2
                y: port.centerY / preview.entry.height * body.height - height / 2
            }
        }

        Repeater {
            model: preview.entry.outputs
            LibraryConnector {
                required property var modelData
                port: modelData
                width: preview.connectorSize
                height: width
                x: body.width - width / 2
                y: port.centerY / preview.entry.height * body.height - height / 2
            }
        }
    }
}
