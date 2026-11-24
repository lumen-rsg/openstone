pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    required property var spec
    required property int selectionCount
    signal triggered(string command)

    width: buttons.implicitWidth + 8
    height: 31
    radius: Theme.controlRadius
    color: Theme.chrome
    border.color: Theme.outline
    Accessible.name: spec.module ? "Module actions" : "Primitive actions"

    readonly property bool hasPanel: spec.displayedPanel.bounds !== undefined
    readonly property var actions: [
        { command: "delete", glyph: "×", label: "Delete" },
        { command: "rename", glyph: "N", label: "Rename", enabled: selectionCount === 1 },
        { command: "enter", glyph: "▾", label: "Move into module", enabled: selectionCount === 1, moduleOnly: true },
        { command: "makeModule", glyph: "□", label: "Make Module", enabled: !spec.boundary },
        { command: "toolbox", glyph: "+", label: "Add to toolbox", moduleOnly: true },
        { command: "properties", glyph: "P", label: spec.propertiesOpen ? "Close properties"
            : spec.hasProperties ? "Show properties" : "Enable properties", active: spec.propertiesOpen, moduleOnly: true },
        { command: "enableFront", glyph: "G", label: "Enable front panel", active: spec.hasFrontPanel,
            enabled: !spec.hasFrontPanel, moduleOnly: true },
        { command: "panel", icon: spec.panelHidden ? "maximize" : "minimize",
            label: spec.panelHidden ? "Maximize front panel" : "Minimize front panel",
            enabled: spec.hasFrontPanel && !spec.propertiesOpen, moduleOnly: true },
        { command: "showInParent", icon: spec.showPanelInParent ? "eye" : "eyeOff",
            label: spec.showPanelInParent ? "Hide in parent panel" : "Show in parent panel",
            enabled: spec.hasFrontPanel || spec.hasProperties, moduleOnly: true },
        { command: "editPanel", icon: spec.panelUnlocked ? "unlocked" : "locked",
            label: spec.panelUnlocked ? "Lock panel" : "Edit panel", active: spec.panelUnlocked,
            enabled: hasPanel, moduleOnly: true },
        { command: "focus", icon: "focus", label: "Focus mode", enabled: spec.hasFrontPanel, moduleOnly: true }
    ].filter(action => spec.module || !action.moduleOnly)

    Row {
        id: buttons
        anchors.centerIn: parent
        spacing: 1
        Repeater {
            model: bar.actions.length
            delegate: FlatButton {
                id: button
                required property int index
                readonly property var actionSpec: bar.actions[index]
                objectName: "node-action-" + actionSpec.command
                width: 24
                height: 25
                padding: 0
                enabled: actionSpec.enabled ?? true
                checkable: actionSpec.active !== undefined
                checked: actionSpec.active ?? false
                Accessible.name: actionSpec.label
                ToolTip.visible: hovered
                ToolTip.delay: 350
                ToolTip.text: actionSpec.label
                onClicked: bar.triggered(actionSpec.command)

                background: Rectangle {
                    radius: 5
                    color: button.down ? Theme.selected : button.checked ? Theme.selection
                                       : button.hovered ? Theme.hover : "transparent"
                }
                contentItem: Item {
                    readonly property color ink: button.enabled ? Theme.text : Theme.mutedText
                    Label {
                        anchors.centerIn: parent
                        text: button.actionSpec.glyph ?? ""
                        color: parent.ink
                        font.pixelSize: 15
                        font.weight: Font.ExtraBold
                    }
                    Canvas {
                        id: icon
                        anchors.centerIn: parent
                        width: 16
                        height: 16
                        property string shape: button.actionSpec.icon ?? ""
                        property color ink: parent.ink
                        visible: shape !== ""
                        onShapeChanged: requestPaint()
                        onInkChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            ctx.strokeStyle = ink
                            ctx.lineWidth = 1.45
                            ctx.lineCap = "round"
                            ctx.lineJoin = "round"
                            ctx.beginPath()
                            if (shape === "eye" || shape === "eyeOff") {
                                ctx.moveTo(1, 8); ctx.quadraticCurveTo(8, 0, 15, 8)
                                ctx.quadraticCurveTo(8, 16, 1, 8)
                                ctx.moveTo(10.3, 8); ctx.arc(8, 8, 2.3, 0, 2 * Math.PI)
                                if (shape === "eyeOff") {
                                    ctx.moveTo(2, 14); ctx.lineTo(14, 2)
                                }
                            } else if (shape === "locked" || shape === "unlocked") {
                                ctx.rect(3, 7.5, 10, 6.5)
                                if (shape === "locked") {
                                    ctx.moveTo(5, 7.5); ctx.lineTo(5, 5)
                                    ctx.arc(8, 5, 3, Math.PI, 2 * Math.PI); ctx.lineTo(11, 7.5)
                                } else {
                                    ctx.moveTo(5, 7.5); ctx.lineTo(5, 5)
                                    ctx.arc(8, 5, 3, Math.PI, 2 * Math.PI)
                                }
                            } else if (shape === "focus") {
                                ctx.arc(6.75, 6.5, 4.4, 0, 2 * Math.PI)
                                ctx.moveTo(10, 9.9); ctx.lineTo(14, 14)
                            } else {
                                ctx.rect(2, 6, 9, 7)
                                ctx.moveTo(6, 6); ctx.lineTo(6, 3); ctx.lineTo(14, 3)
                                ctx.lineTo(14, 10); ctx.lineTo(11, 10)
                                if (shape === "maximize") {
                                    ctx.moveTo(3.5, 8); ctx.lineTo(9.5, 8)
                                }
                            }
                            ctx.stroke()
                        }
                    }
                }
            }
        }
    }
}
