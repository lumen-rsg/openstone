pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Popup {
    id: picker

    required property var groups
    property bool canDisconnect: false
    signal typeChosen(int type)
    signal disconnectRequested()
    property var rubyPort: null
    signal rubyEditRequested(string command, int value, string label)

    function editRuby(command: string, value: int, label: string) {
        close()
        rubyEditRequested(command, value, label)
    }

    readonly property int cellSize: 34
    readonly property int gap: 4
    readonly property int gridWidth: rubyPort ? 182 : 3 * cellSize + 2 * gap

    parent: Overlay.overlay
    popupType: Popup.Item
    modal: true
    dim: false
    focus: true
    padding: 6
    margins: 8
    width: gridWidth + leftPadding + rightPadding + 10
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function reveal(item: Item) {
        const viewport = scroll.contentItem as Flickable
        const top = item.mapToItem(choices, 0, 0).y
        if (top < viewport.contentY)
            viewport.contentY = top
        else if (top + item.height > viewport.contentY + viewport.height)
            viewport.contentY = top + item.height - viewport.height
    }

    function openAt(anchor: Item, input: bool) {
        canDisconnect = input
        if (rubyPort)
            nameField.text = rubyPort.title
        const position = anchor.mapToItem(parent, anchor.width, anchor.height / 2)
        const fitsRight = position.x + 4 + width <= parent.width - margins
        const leftEdge = anchor.mapToItem(parent, 0, 0).x
        x = Math.max(margins, fitsRight ? position.x + 4 : leftEdge - width - 4)
        y = Math.max(margins, Math.min(position.y - 8, parent.height - height - margins))
        open()
    }

    background: Rectangle {
        radius: Theme.controlRadius
        antialiasing: true
        color: Theme.surface
        border.color: Theme.outline
    }

    contentItem: ScrollView {
        id: scroll
        implicitHeight: Math.min(choices.implicitHeight,
                                 Math.max(1, (picker.parent?.height ?? 480) - 2 * (picker.margins + picker.padding)))
        contentWidth: picker.gridWidth
        contentHeight: choices.implicitHeight
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        Column {
            id: choices
            spacing: 4
            width: picker.gridWidth

            Column {
                width: picker.gridWidth
                spacing: 4
                visible: picker.rubyPort !== null

                TextField {
                    id: nameField
                    objectName: "rubyPortMenuName"
                    width: parent.width
                    placeholderText: "Connector name"
                    selectByMouse: true
                    Accessible.name: "Ruby connector name"
                    onAccepted: picker.editRuby("name", 0, text.trim())
                }

                Row {
                    spacing: 3

                    Repeater {
                        model: picker.rubyPort ? [
                            { glyph: "+", label: "Insert after", command: "insert", value: 0,
                              allowed: picker.rubyPort.count < picker.rubyPort.limit },
                            { glyph: "×", label: "Remove connector", command: "remove", value: 0, allowed: true },
                            { glyph: "↑", label: "Move up", command: "move", value: picker.rubyPort.index - 1,
                              allowed: picker.rubyPort.index > 0 },
                            { glyph: "↓", label: "Move down", command: "move", value: picker.rubyPort.index + 1,
                              allowed: picker.rubyPort.index + 1 < picker.rubyPort.count }
                        ] : []
                        delegate: FlatButton {
                            required property var modelData
                            width: 40
                            text: modelData.glyph
                            enabled: modelData.allowed
                            Accessible.name: modelData.label
                            ToolTip.visible: hovered
                            ToolTip.text: modelData.label
                            onClicked: picker.editRuby(modelData.command, modelData.value, "")
                        }
                    }
                }
            }

            Repeater {
                model: picker.groups
                delegate: Column {
                    id: typeGroup
                    required property var modelData
                    required property int index
                    spacing: 4

                    Rectangle {
                        visible: typeGroup.index > 0
                        width: picker.gridWidth
                        height: 1
                        color: Theme.outline
                    }

                    Grid {
                        columns: 3
                        spacing: picker.gap

                        Repeater {
                            model: typeGroup.modelData
                            delegate: ToolButton {
                                id: choice
                                required property var modelData
                                readonly property bool template: modelData.shape === "template"
                                readonly property bool array: modelData.shape === "array"
                                readonly property bool trigger: modelData.shape === "trigger"
                                readonly property bool yellow: modelData.yellow === true
                                readonly property bool bus: modelData.bus === true
                                readonly property bool hasPalette: !!modelData.palette
                                readonly property color rim: template ? Theme.mutedText
                                    : bus ? Theme.secondaryText : trigger ? Theme.amber : yellow ? Theme.yellow : Theme.green
                                readonly property color fill: template ? Theme.field
                                    : bus ? Theme.hover : trigger ? Theme.amberSurface
                                    : yellow ? Theme.yellowSurface : Theme.greenSurface
                                readonly property string typeLabel: template ? "Template (automatic)" : modelData.label
                                width: picker.cellSize
                                height: picker.cellSize
                                padding: 0
                                hoverEnabled: true
                                Accessible.name: typeLabel
                                Accessible.description: modelData.selected ? "Current type" : ""
                                ToolTip.visible: hovered && picker.opened
                                ToolTip.delay: 650
                                ToolTip.text: typeLabel
                                Keys.onReturnPressed: clicked()
                                Keys.onEnterPressed: clicked()
                                onActiveFocusChanged: {
                                    if (activeFocus && picker.opened)
                                        picker.reveal(choice)
                                }
                                Connections {
                                    target: picker
                                    function onOpened() {
                                        if (choice.modelData.selected) {
                                            choice.forceActiveFocus(Qt.OtherFocusReason)
                                        }
                                    }
                                }
                                onClicked: {
                                    const type = modelData.type
                                    picker.close()
                                    picker.typeChosen(type)
                                }

                                background: Rectangle {
                                    radius: Theme.controlRadius
                                    color: choice.modelData.selected ? Theme.selection
                                         : choice.down ? Theme.selected
                                         : choice.hovered || choice.visualFocus ? Theme.hover : "transparent"
                                    border.width: choice.modelData.selected || choice.visualFocus ? 1 : 0
                                    border.color: Theme.focus
                                }

                                contentItem: Item {
                                    Loader {
                                        anchors.centerIn: parent
                                        width: 22
                                        height: width
                                        active: choice.hasPalette
                                        sourceComponent: SignalConnectorIcon {
                                            connector: choice.modelData
                                            highlighted: choice.hovered
                                        }
                                    }
                                    Item {
                                        id: icon
                                        visible: !choice.hasPalette
                                        anchors.centerIn: parent
                                        width: 22
                                        height: width

                                        Rectangle {
                                            anchors.fill: parent
                                            anchors.margins: choice.array ? 1 : 0
                                            radius: choice.array ? 6 : width / 2
                                            color: choice.fill
                                            border.color: choice.rim
                                            antialiasing: true
                                        }

                                        Repeater {
                                            model: choice.array ? 4 : 0
                                            Rectangle {
                                                required property int index
                                                x: index % 2 === 0 ? 0 : icon.width - width
                                                y: index < 2 ? 0 : icon.height - height
                                                width: 5
                                                height: width
                                                radius: width / 2
                                                color: choice.rim
                                                antialiasing: true
                                            }
                                        }

                                        Text {
                                            anchors.centerIn: parent
                                            text: choice.modelData.glyph
                                            color: choice.bus ? Theme.text : choice.trigger ? Theme.amberText
                                                 : choice.yellow ? Theme.yellowText : Theme.greenText
                                            font: choice.font
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                visible: picker.canDisconnect
                width: picker.gridWidth
                height: 1
                color: Theme.outline
            }

            FlatButton {
                visible: picker.canDisconnect
                width: picker.gridWidth
                text: "Disconnect input"
                font.pixelSize: 11
                onClicked: {
                    picker.close()
                    picker.disconnectRequested()
                }
            }
        }
    }
}
