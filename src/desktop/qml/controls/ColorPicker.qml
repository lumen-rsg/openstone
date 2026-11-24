pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: picker

    property int nodeId: -1
    property color draft: "transparent"
    property real hue: 0
    property real saturation: 0
    property real brightness: 0
    signal colorChosen(int id, color chosen)

    parent: Overlay.overlay
    popupType: Popup.Item
    width: 250
    padding: 12
    margins: 8
    focus: true
    modal: true
    dim: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function hexByte(component: real): string {
        return Math.round(component * 255).toString(16).padStart(2, "0").toUpperCase()
    }

    function hexColor(): string {
        return "#" + hexByte(draft.a) + hexByte(draft.r) + hexByte(draft.g) + hexByte(draft.b)
    }

    function loadColor(value: color) {
        draft = value
        hue = Math.max(0, value.hsvHue)
        saturation = value.hsvSaturation
        brightness = value.hsvValue
        hexField.text = hexColor()
    }

    function applyHex() {
        if (!hexField.acceptableInput)
            return
        const text = hexField.text.startsWith("#") ? hexField.text : "#" + hexField.text
        if (text.toUpperCase() !== hexColor())
            loadColor(Qt.color(text))
    }

    function updateColor() {
        draft = Qt.hsva(hue, saturation, brightness, draft.a)
        hexField.text = hexColor()
    }

    function openAt(id: int, anchor: Item, value: color) {
        nodeId = id
        loadColor(value)
        const right = anchor.mapToItem(parent, anchor.width, anchor.height / 2)
        const left = anchor.mapToItem(parent, 0, 0).x
        x = Math.max(margins, right.x + 14 + width <= parent.width - margins
            ? right.x + 14 : left - width - 14)
        y = Math.max(margins, Math.min(right.y - 22, parent.height - height - margins))
        open()
    }

    component ColorSlider: Slider {
        id: slider
        Layout.fillWidth: true
        Layout.preferredHeight: 24

        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + (slider.availableHeight - height) / 2
            implicitWidth: 14
            implicitHeight: 14
            radius: width / 2
            color: slider.pressed ? Theme.selected : Theme.surface
            border.color: slider.visualFocus ? Theme.focus : Theme.text
        }
    }

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.outline
        radius: Theme.panelRadius
    }

    contentItem: ColumnLayout {
        spacing: 9

        Rectangle {
            id: plane
            Layout.fillWidth: true
            Layout.preferredHeight: 112
            color: Qt.hsva(picker.hue, 1, 1, 1)
            clip: true

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "#FFFFFFFF" }
                    GradientStop { position: 1; color: "#00FFFFFF" }
                }
            }
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: "#00000000" }
                    GradientStop { position: 1; color: "#FF000000" }
                }
            }
            Rectangle {
                x: Math.max(0, Math.min(plane.width - width, picker.saturation * plane.width - width / 2))
                y: Math.max(0, Math.min(plane.height - height, (1 - picker.brightness) * plane.height - height / 2))
                width: 12
                height: width
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: picker.brightness > 0.7 && picker.saturation < 0.35 ? "#252525" : "#FFFFFF"
            }
            MouseArea {
                anchors.fill: parent
                preventStealing: true
                cursorShape: Qt.CrossCursor

                function pick(x: real, y: real) {
                    picker.saturation = Math.max(0, Math.min(1, x / width))
                    picker.brightness = 1 - Math.max(0, Math.min(1, y / height))
                    picker.updateColor()
                }

                onPressed: mouse => pick(mouse.x, mouse.y)
                onPositionChanged: mouse => { if (pressed) pick(mouse.x, mouse.y) }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 9
            rowSpacing: 3

            Label { text: "Hue"; color: Theme.secondaryText; font.pixelSize: 12 }
            ColorSlider {
                id: hueSlider
                value: picker.hue
                stepSize: 1 / 360
                Accessible.name: "Hue"
                onMoved: {
                    picker.hue = value
                    picker.updateColor()
                }
                background: Rectangle {
                    x: hueSlider.leftPadding
                    y: hueSlider.topPadding + (hueSlider.availableHeight - height) / 2
                    width: hueSlider.availableWidth
                    height: 8
                    radius: 4
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: "#FF0000" }
                        GradientStop { position: 1 / 6; color: "#FFFF00" }
                        GradientStop { position: 2 / 6; color: "#00FF00" }
                        GradientStop { position: 3 / 6; color: "#00FFFF" }
                        GradientStop { position: 4 / 6; color: "#0000FF" }
                        GradientStop { position: 5 / 6; color: "#FF00FF" }
                        GradientStop { position: 1; color: "#FF0000" }
                    }
                }
            }

            Label { text: "Alpha"; color: Theme.secondaryText; font.pixelSize: 12 }
            ColorSlider {
                id: alphaSlider
                to: 255
                stepSize: 1
                value: Math.round(picker.draft.a * 255)
                Accessible.name: "Alpha"
                onMoved: {
                    picker.draft = Qt.rgba(picker.draft.r, picker.draft.g, picker.draft.b, value / 255)
                    hexField.text = picker.hexColor()
                }
                background: Item {
                    x: alphaSlider.leftPadding
                    y: alphaSlider.topPadding + (alphaSlider.availableHeight - height) / 2
                    width: alphaSlider.availableWidth
                    height: 8
                    ColorSwatch { anchors.fill: parent; fill: "transparent" }
                    Rectangle {
                        anchors.fill: parent
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0; color: Qt.rgba(picker.draft.r, picker.draft.g, picker.draft.b, 0) }
                            GradientStop { position: 1; color: Qt.rgba(picker.draft.r, picker.draft.g, picker.draft.b, 1) }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            ColorSwatch {
                Layout.preferredWidth: 35
                Layout.fillHeight: true
                fill: picker.draft
            }
            TextField {
                id: hexField
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                placeholderText: "#AARRGGBB"
                Accessible.name: "ARGB hex value"
                validator: RegularExpressionValidator { regularExpression: /#?[0-9A-Fa-f]{8}/ }
                selectByMouse: true
                onEditingFinished: picker.applyHex()
                background: Rectangle {
                    radius: Theme.controlRadius
                    color: Theme.field
                    border.color: hexField.activeFocus ? Theme.focus : Theme.outline
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            FlatButton { text: "Cancel"; onClicked: picker.close() }
            FlatButton {
                text: "Apply"
                filled: true
                enabled: hexField.acceptableInput
                onClicked: {
                    picker.applyHex()
                    picker.colorChosen(picker.nodeId, picker.draft)
                    picker.close()
                }
            }
        }
    }
}
