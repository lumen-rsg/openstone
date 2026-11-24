pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Rectangle {
    id: crumb

    required property string name
    required property bool current
    required property point viewCenter
    required property real viewZoom
    required property real canvasWidth
    required property real canvasHeight
    property var nodeBounds: []
    signal activated()
    signal centerRequested(real x, real y)

    implicitWidth: content.implicitWidth + 8
    implicitHeight: 40
    radius: Theme.controlRadius + 1
    border.color: current ? Theme.outline : Theme.divider
    color: current ? Theme.hover : Theme.field

    Row {
        id: content
        anchors.left: parent.left
        anchors.leftMargin: 3
        anchors.verticalCenter: parent.verticalCenter
        spacing: 1

        FlatButton {
            text: crumb.name
            height: 32
            onClicked: crumb.activated()
        }

        Rectangle {
            id: map
            anchors.verticalCenter: parent.verticalCenter
            width: 62
            height: 30
            radius: 5
            clip: true
            color: Theme.canvas

            readonly property real halfViewWidth: crumb.canvasWidth / (2 * crumb.viewZoom)
            readonly property real halfViewHeight: crumb.canvasHeight / (2 * crumb.viewZoom)
            readonly property real mapScale: Math.min(
                (width - 6) / (2 * Math.max(800, Math.abs(crumb.viewCenter.x) + halfViewWidth)),
                (height - 6) / (2 * Math.max(450, Math.abs(crumb.viewCenter.y) + halfViewHeight)))

            // The grid and outline share the same world-space origin.
            Rectangle { x: parent.width / 2; width: 1; height: parent.height; color: Theme.divider }
            Rectangle { y: parent.height / 2; width: parent.width; height: 1; color: Theme.divider }

            Rectangle {
                width: 2 * map.halfViewWidth * map.mapScale
                height: 2 * map.halfViewHeight * map.mapScale
                x: (map.width - width) / 2 + crumb.viewCenter.x * map.mapScale
                y: (map.height - height) / 2 + crumb.viewCenter.y * map.mapScale
                color: Theme.hover
                border.color: crumb.current ? Theme.focus : Theme.outline
            }

            Repeater {
                model: crumb.nodeBounds
                Rectangle {
                    required property rect modelData
                    x: map.width / 2 + modelData.x * map.mapScale
                    y: map.height / 2 + modelData.y * map.mapScale
                    width: Math.max(1, modelData.width * map.mapScale)
                    height: Math.max(1, modelData.height * map.mapScale)
                    color: Theme.green
                }
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                property real dragScale: 1
                Accessible.name: crumb.name + " minimap"
                ToolTip.visible: containsMouse && !pressed
                ToolTip.delay: 500
                ToolTip.text: "Pan " + crumb.name

                function recenter(mouseX: real, mouseY: real) {
                    crumb.centerRequested((mouseX - width / 2) / dragScale,
                                          (mouseY - height / 2) / dragScale)
                }
                onPressed: mouse => {
                    dragScale = map.mapScale
                    recenter(mouse.x, mouse.y)
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        recenter(mouse.x, mouse.y)
                }
            }
        }
    }
}
