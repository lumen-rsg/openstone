pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: node
    // The canvas supplies viewport coordinates and owns editor selection.
    required property var canvas
    required property int nodeId
    required property var spec
    required property string valueText
    required property bool editable
    readonly property bool selected: canvas.selectedNodes.includes(nodeId)
    readonly property bool showPanel: spec.module && spec.displayedPanel.bounds !== undefined
    property size rubyResizePreview: Qt.size(0, 0)
    width: rubyResizePreview.width > 0 ? rubyResizePreview.width : spec.width
    height: rubyResizePreview.height > 0 ? rubyResizePreview.height : spec.height
    z: selected ? 2 : 1

    function openTypePicker(anchor: Item, input: int, output: int) {
        if (spec.rubyNode) {
            const ports = input < 0 ? spec.outputs : spec.inputs
            const index = input < 0 ? output : input
            canvas.openRubyPortPicker(nodeId, input < 0, index, anchor, ports[index], ports.length, spec.rubyPortLimit)
        } else {
            canvas.openTypePicker(nodeId, input, anchor, spec.templateTypeGroups)
        }
    }

    function inputAt(worldX: real, worldY: real): int {
        if (Math.abs(worldX - x) > 12)
            return -1
        for (let i = 0; i < spec.inputs.length; ++i) {
            if (Math.abs(worldY - y - spec.inputs[i].centerY) <= 12)
                return i
        }
        return -1
    }

    // Ports extend half their width beyond the node's body.
    function portAt(worldX: real, worldY: real): var {
        const input = inputAt(worldX, worldY)
        if (input >= 0)
            return { index: input, output: false }
        for (let i = 0; i < spec.outputs.length; ++i) {
            if (Math.hypot(worldX - x - width, worldY - y - spec.outputs[i].centerY) <= 12)
                return { index: i, output: true }
        }
        return null
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.nodeRadius
        antialiasing: true
        color: node.selected ? Theme.hover : Theme.surface
        border.color: node.selected ? Theme.focus : Theme.outline
    }

    MouseArea {
        id: bodyPointer
        anchors.fill: parent
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property point grab
        property point grabWorld
        property bool moved: false

        onPressed: mouse => {
            node.canvas.focusNode(node.nodeId, true)
            grab = mapToItem(node.canvas, mouse.x, mouse.y)
            grabWorld = node.canvas.worldAt(grab.x, grab.y)
            moved = false
        }
        onDoubleClicked: mouse => {
            if (node.spec.module && (!node.canvas.graph.syncPainterActive || (mouse.modifiers & Qt.ControlModifier)))
                node.canvas.graph.enterModule(node.nodeId)
        }
        onPositionChanged: mouse => {
            if (pressed) {
                const p = mapToItem(node.canvas, mouse.x, mouse.y)
                if (!moved) {
                    if (Math.hypot(p.x - grab.x, p.y - grab.y) < 4) return
                    moved = node.canvas.graph.beginNodeMove(node.canvas.selectedNodes)
                    if (!moved) return
                }
                const world = node.canvas.worldAt(p.x, p.y)
                node.canvas.graph.moveNodeSelection(world.x - grabWorld.x, world.y - grabWorld.y)
                if (!(mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier)))
                    node.canvas.updateAutoWire()
                else
                    node.canvas.clearAutoWire()
            }
        }
        onReleased: mouse => {
            if (moved) {
                if (mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier))
                    node.canvas.clearAutoWire()
                else
                    node.canvas.acceptAutoWire()
                node.canvas.graph.finishEdit()
                moved = false
            } else if (node.canvas.selectedNodes.length > 1) {
                node.canvas.selectNode(node.nodeId)
            }
        }
        onCanceled: {
            node.canvas.clearAutoWire()
            if (moved) {
                node.canvas.graph.finishEdit()
                moved = false
            }
        }
    }

    HoverHandler {
        id: helpHover
        enabled: !!node.spec.helpTitle || !!node.spec.helpBody
    }

    Loader {
        active: helpHover.enabled
        sourceComponent: ToolTip {
            id: helpTip
            objectName: "moduleHelpTooltip"
            popupType: Popup.Item
            parent: node
            visible: helpHover.hovered && !bodyPointer.pressed
            delay: 500
            implicitWidth: 300
            contentItem: Column {
                spacing: 5
                Label {
                    width: parent.width
                    visible: text.length > 0
                    text: node.spec.helpTitle
                    color: helpTip.palette.toolTipText
                    textFormat: Text.PlainText
                    font.bold: true
                    wrapMode: Text.Wrap
                }
                Label {
                    width: parent.width
                    visible: text.length > 0
                    text: node.spec.helpBody
                    color: helpTip.palette.toolTipText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        // Let front-panel controls supply the cursor through the Shift-click overlay.
        cursorShape: undefined
        z: 4
        onPressed: mouse => {
            if (!(mouse.modifiers & Qt.ShiftModifier) || node.spec.boundary || node.canvas.graph.syncPainterActive ||
                ((node.spec.dspNode || node.spec.rubyNode) && mouse.x > 88 && mouse.x < node.width - 88 && mouse.y > 29))
                mouse.accepted = false
        }
        onClicked: node.canvas.toggleSelection(node.nodeId)
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton

        onPressed: mouse => {
            const port = node.portAt(node.x + mouse.x, node.y + mouse.y)
            if ((port && (!port.output || node.spec.outputs[port.index].canChooseType)) ||
                ((node.spec.dspNode || node.spec.rubyNode) && mouse.x > 88 && mouse.x < node.width - 88 && mouse.y > 29))
                mouse.accepted = false
        }
        onClicked: mouse => node.canvas.openNodeMenu(
            node.nodeId, node.spec, mapToItem(node.canvas, mouse.x, mouse.y))
        z: 2
    }

    MouseArea {
        anchors.fill: parent
        enabled: node.spec.module && node.canvas.graph.syncPainterActive
        z: 3
        cursorShape: enabled ? Qt.CrossCursor : undefined
        onPressed: mouse => {
            if (mouse.modifiers & Qt.ControlModifier)
                mouse.accepted = false
        }
        onClicked: node.canvas.paintModule(node.nodeId)
    }

    Label {
        x: 13
        y: 7
        width: node.width - 26
        visible: !node.spec.operatorNode && !node.spec.module && !node.spec.wirelessLabel
        text: node.spec.title
        color: Theme.secondaryText
        font.pixelSize: 12
        font.bold: true
        elide: Text.ElideRight
    }

    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        y: -height - 2
        visible: node.spec.operatorNode && node.spec.customLabel
        text: node.spec.title
        color: Theme.secondaryText
        font.pixelSize: 11
    }

    Loader {
        x: 15
        y: 27
        width: node.width - 30
        height: 28
        active: node.spec.boundary
        sourceComponent: TextField {
            id: portName
            text: node.spec.boundaryName
            selectByMouse: true
            Accessible.name: node.spec.title + " name"
            Accessible.description: "Press Return to rename"
            background: Rectangle {
                radius: Theme.controlRadius
                color: Theme.field
                border.color: portName.activeFocus ? Theme.focus : Theme.divider
            }
            onAccepted: {
                node.canvas.graph.renameNode(node.nodeId, text)
                text = Qt.binding(() => node.spec.boundaryName)
            }
            onActiveFocusChanged: {
                if (!activeFocus)
                    text = Qt.binding(() => node.spec.boundaryName)
            }
        }
    }

    FlatButton {
        id: portType
        x: 15
        y: 58
        width: node.width - 30
        height: 23
        visible: node.spec.boundary
        readonly property string connectorType: visible ? (node.spec.inputs[0] ?? node.spec.outputs[0]).type : ""
        text: connectorType + (node.spec.automaticType && connectorType !== "Template" ? " (auto)" : "") + "  ▾"
        font.pixelSize: 11
        Accessible.name: node.spec.title + " connector type"
        onClicked: node.openTypePicker(portType, node.spec.inputs.length ? 0 : -1, 0)
    }

    Label {
        x: 13
        y: 9
        visible: node.spec.module
        width: Math.min(implicitWidth, node.width - ((node.spec.syncCount ?? 0) > 0 ? 62 : 26))
        height: 28
        verticalAlignment: Text.AlignVCenter
        leftPadding: 9
        rightPadding: 9
        text: node.spec.title
        font.bold: true
        color: Theme.text
        elide: Text.ElideRight
        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.field
            border.color: Theme.divider
        }
    }

    Label {
        anchors.right: parent.right
        anchors.rightMargin: 12
        y: 16
        visible: (node.spec.syncCount ?? 0) > 0
        text: "×" + ((node.spec.syncCount ?? 0) + 1)
        color: Theme.focus
        font.bold: true
        Accessible.name: text + " synchronized instances"
    }

    Loader {
        x: 17
        y: 9
        width: node.width - 56
        height: 30
        active: node.spec.wirelessLabel

        sourceComponent: TextField {
            id: linkName
            text: node.spec.wirelessName
            placeholderText: "Link name"
            selectByMouse: true
            Accessible.name: node.spec.title + " name"
            color: Theme.text
            background: Rectangle {
                radius: Theme.controlRadius
                color: Theme.field
                border.color: linkName.activeFocus ? Theme.focus : Theme.divider
            }
            onAccepted: {
                node.canvas.graph.renameNode(node.nodeId, text)
                text = Qt.binding(() => node.spec.wirelessName)
            }
            onActiveFocusChanged: {
                if (!activeFocus)
                    text = Qt.binding(() => node.spec.wirelessName)
            }
        }
    }

    Label {
        x: node.width - (node.spec.wirelessLabel ? 34 : 26)
        y: node.spec.wirelessLabel ? (node.height - height) / 2 : 5
        visible: node.spec.wireless
        text: node.spec.symbol
        color: node.spec.wirelessConnected ? Theme.green : Theme.mutedText
        font.pixelSize: 14
        Accessible.name: node.spec.wirelessConnected ? "Wireless connected" : "Wireless unconnected"
        ToolTip.visible: linkStatusHover.hovered
        ToolTip.delay: 400
        ToolTip.text: Accessible.name + (node.spec.wirelessName ? " · " + node.spec.wirelessName : "")
        HoverHandler { id: linkStatusHover }
    }

    Loader {
        id: panel
        z: 2
        x: 13
        y: node.spec.panelTop
        active: node.showPanel
        width: active ? node.spec.displayedPanel.bounds.width * node.spec.panelScale : 0
        height: active ? node.spec.displayedPanel.bounds.height * node.spec.panelScale : 0

        sourceComponent: FrontPanel {
            panel: node.spec.displayedPanel
            graph: node.canvas.graph
            dropListHost: node.canvas.dropListHost
            pixelsPerUnit: node.spec.panelScale
            canvasZoom: node.canvas.viewZoom
            editing: node.selected && node.spec.panelUnlocked
            Accessible.name: node.spec.propertiesOpen ? "Module properties panel" : "Front panel editor"
            onPanelItemMoved: (itemPath, x, y, itemProperties) =>
                node.canvas.graph.movePanelItem(node.nodeId, itemPath, x, y, node.spec.propertiesOpen, itemProperties)
            onPanelItemAnchorsChanged: (itemPath, itemProperties, anchors) =>
                node.canvas.graph.setPanelItemAnchors(node.nodeId, itemPath, node.spec.propertiesOpen, itemProperties, anchors)
        }
    }

    Loader {
        y: node.height - 1
        z: 6
        active: node.canvas.selectedNode === node.nodeId
        sourceComponent: NodeActionBar {
            spec: node.spec
            selectionCount: node.canvas.selectedNodes.length
            onTriggered: command => node.canvas.runNodeCommand(command, node.nodeId, node.spec)
        }
    }

    Loader {
        active: node.showPanel && node.selected && node.spec.panelUnlocked
        x: panel.x + panel.width - 19
        y: panel.y + panel.height - 19
        width: 18
        height: 18
        z: 3

        sourceComponent: MouseArea {
            id: grip
            hoverEnabled: true
            cursorShape: Qt.SizeFDiagCursor
            Accessible.name: node.spec.propertiesOpen ? "Resize properties panel" : "Resize front panel"
            property point grab
            property size original
            property size draft

            Rectangle {
                x: panel.x - grip.parent.x
                y: panel.y - grip.parent.y
                width: grip.draft.width * node.spec.panelScale
                height: grip.draft.height * node.spec.panelScale
                visible: grip.pressed
                color: "transparent"
                border.color: Theme.focus
                radius: Theme.controlRadius
            }

            Label {
                anchors.centerIn: parent
                text: "◢"
                color: grip.containsMouse || grip.pressed ? Theme.focus : Theme.mutedText
                font.pixelSize: 11
            }

            onPressed: mouse => {
                grab = mapToItem(node, mouse.x, mouse.y)
                const bounds = node.spec.displayedPanel.bounds
                original = Qt.size(bounds.width, bounds.height)
                draft = original
            }
            onPositionChanged: mouse => {
                if (pressed) {
                    const p = mapToItem(node, mouse.x, mouse.y)
                    const k = node.spec.panelScale
                    draft = Qt.size(Math.max(2, Math.round((original.width + (p.x - grab.x) / k) * 2) / 2),
                                    Math.max(2, Math.round((original.height + (p.y - grab.y) / k) * 2) / 2))
                }
            }
            onReleased: {
                if (draft.width !== original.width || draft.height !== original.height)
                    node.canvas.graph.resizePanel(node.nodeId, draft.width, draft.height, node.spec.propertiesOpen)
            }
        }
    }

    Loader {
        active: node.spec.valueField && !node.spec.multiline
        sourceComponent: TextField {
            id: valueField
            width: node.width - parent.x - 17
            height: 30
            text: node.valueText
            readOnly: !node.editable
            selectByMouse: true
            leftPadding: 8
            rightPadding: 6
            topPadding: 2
            bottomPadding: 2
            color: Theme.text
            font.pixelSize: 14
            Accessible.name: node.spec.tickNode ? "Ticks per second" : node.spec.title + " value"
            background: Rectangle {
                radius: Theme.controlRadius
                color: valueField.readOnly ? Theme.hover : Theme.field
                border.color: valueField.activeFocus ? Theme.focus : Theme.divider
            }
            onActiveFocusChanged: {
                if (activeFocus)
                    node.canvas.replaceSelection(node.nodeId)
            }
            onEditingFinished: {
                if (!readOnly && text !== node.valueText) {
                    node.canvas.graph.setText(node.nodeId, text)
                    text = Qt.binding(() => node.valueText)
                }
            }
        }
        x: node.spec.tickNode ? 49 : 17
        y: 32
    }

    Loader {
        active: node.spec.multiline
        x: 17
        y: 32
        sourceComponent: ScrollView {
            width: node.width - 34
            height: node.height - 43
            TextArea {
                id: multilineField
                text: node.valueText
                readOnly: !node.editable
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                color: Theme.text
                font.pixelSize: 14
                Accessible.name: "Multiline String value"
                background: Rectangle {
                    radius: Theme.controlRadius
                    color: multilineField.readOnly ? Theme.hover : Theme.field
                    border.color: multilineField.activeFocus ? Theme.focus : Theme.divider
                }
                onActiveFocusChanged: {
                    if (activeFocus)
                        node.canvas.replaceSelection(node.nodeId)
                    else if (!readOnly && text !== node.valueText) {
                        node.canvas.graph.setText(node.nodeId, text)
                        text = Qt.binding(() => node.valueText)
                    }
                }
            }
        }
    }

    Loader {
        active: node.spec.colorField
        x: 17
        y: 28
        sourceComponent: ToolButton {
            id: colorButton
            readonly property color currentColor: node.valueText.startsWith("#") ? node.valueText : "transparent"
            width: node.width - 34
            height: 22
            padding: 2
            enabled: node.editable
            hoverEnabled: true
            Accessible.name: "Edit color"
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: node.valueText
            background: Rectangle {
                radius: Theme.controlRadius
                color: Theme.field
                border.color: colorButton.hovered || colorButton.visualFocus ? Theme.focus : Theme.outline
            }
            contentItem: ColorSwatch { fill: colorButton.currentColor }
            onClicked: node.canvas.openColorPicker(node.nodeId, node, currentColor)
        }
    }

    Label {
        anchors.centerIn: parent
        visible: node.spec.operatorNode
        text: node.spec.symbol
        color: Theme.connectorPalettes[node.spec.outputs[0]?.palette]?.rim ?? Theme.green
        font.pixelSize: text.length > 1 ? 16 : 28
        font.bold: true
        Accessible.name: node.spec.title
    }

    FlatButton {
        x: 13
        y: 32
        width: node.width - 30
        height: 29
        visible: node.spec.triggerNode
        text: "Fire"
        filled: true
        warm: true
        Accessible.name: "Fire trigger"
        onClicked: {
            node.canvas.replaceSelection(node.nodeId)
            node.canvas.graph.fire(node.nodeId)
        }
    }

    Loader {
        x: 13
        y: node.height - height - 8
        width: node.width - 26
        height: 29
        active: node.spec.midiDevicePicker

        sourceComponent: FlatButton {
            id: deviceButton
            filled: true
            text: node.valueText
            Accessible.name: node.spec.title + " devices"
            ToolTip.visible: hovered && text.length > 0
            ToolTip.text: text
            ToolTip.delay: 600

            contentItem: Item {
                Label {
                    anchors.left: parent.left
                    anchors.right: indicator.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: deviceButton.text
                    font: deviceButton.font
                    color: deviceButton.palette.buttonText
                    elide: Text.ElideRight
                }

                Label {
                    id: indicator
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: Theme.secondaryText
                }
            }

            onClicked: {
                node.canvas.replaceSelection(node.nodeId)
                deviceMenu.devices = node.canvas.graph.midiDeviceChoices(node.nodeId)
                deviceMenu.popup(deviceButton, 0, deviceButton.height)
            }

            EditorMenu {
                id: deviceMenu
                property var devices: []

                EditorMenuItem {
                    text: qsTr("No MIDI devices")
                    visible: deviceMenu.devices.length === 0
                    enabled: false
                }

                Instantiator {
                    model: deviceMenu.devices
                    delegate: EditorMenuItem {
                        required property var modelData
                        text: modelData.label
                        checkable: true
                        checked: modelData.selected
                        enabled: modelData.available || modelData.selected
                        onTriggered: node.canvas.graph.setMidiDeviceSelected(node.nodeId, modelData.id, checked)
                    }
                    onObjectAdded: (index, object) => deviceMenu.insertItem(index + 1, object)
                    onObjectRemoved: (index, object) => deviceMenu.removeItem(object)
                }
            }
        }
    }

    FlatButton {
        id: settingsButton
        x: 12
        y: 25
        width: node.width - 28
        height: 25
        visible: node.spec.settingsChoices.length > 0
        text: visible ? node.spec.settingsChoices[node.spec.settingsChoice] + "  ▾" : ""
        filled: true
        Accessible.name: qsTr("System setting")
        onClicked: {
            node.canvas.replaceSelection(node.nodeId)
            settingsMenu.popup(settingsButton, 0, settingsButton.height)
        }

        EditorMenu {
            id: settingsMenu
            Instantiator {
                model: node.spec.settingsChoices
                delegate: EditorMenuItem {
                    required property string modelData
                    required property int index
                    text: modelData
                    checkable: true
                    checked: index === node.spec.settingsChoice
                    onTriggered: node.canvas.graph.setSystemSetting(node.nodeId, index)
                }
                onObjectAdded: (index, object) => settingsMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => settingsMenu.removeItem(object)
            }
        }
    }

    FlatButton {
        x: 13
        y: node.height - height - 8
        width: node.width - 26
        height: 29
        visible: node.spec.indexSelector
        text: (node.valueText || "Choose an option") + "  ▾"
        filled: true
        Accessible.name: node.spec.title + " options"
        onClicked: {
            node.canvas.replaceSelection(node.nodeId)
            node.canvas.graph.openIndexSelector(node.nodeId, mapToItem(node.canvas, 0, height))
        }
    }

    FlatButton {
        x: 13
        y: node.height - height - 8
        width: node.width - 26
        height: 25
        visible: node.spec.bitmapFilePicker
        text: "Open image…"
        filled: true
        onClicked: node.canvas.openBitmapFile(node.nodeId)
    }

    FlatButton {
        x: 13
        y: node.height - height - 8
        width: node.width - 26
        height: 25
        visible: node.spec.tickNode
        text: "Display Sync"
        filled: true
        checkable: true
        checked: node.spec.tickDisplaySync
        warm: checked
        onClicked: node.canvas.graph.setTickDisplaySync(node.nodeId, checked)
    }

    FlatButton {
        x: 13
        y: node.height - height - 8
        width: node.width - 26
        height: 25
        visible: node.spec.captureButton.length > 0
        text: node.spec.captureButton
        filled: true
        onClicked: {
            node.canvas.replaceSelection(node.nodeId)
            node.canvas.graph.fire(node.nodeId)
        }
    }

    Loader {
        x: 88
        y: 29
        width: node.width - 176
        height: node.height - 37
        active: node.spec.dspNode
        sourceComponent: DspCodeEditor {
            assembler: node.spec.assemblerNode
            source: node.spec.dspSource
            appliedSource: node.spec.dspAppliedSource
            onDraftChanged: text => node.canvas.graph.setDspDraft(node.nodeId, text)
            onCompileRequested: moveToError => showDiagnostic(node.canvas.graph.compileDsp(node.nodeId), moveToError)
            onCaptureRequested: (frames, blockSize) => showCapture(node.canvas.graph.captureDsp(node.nodeId, frames, blockSize))
        }
    }

    Loader {
        x: 88
        y: 29
        width: node.width - 176
        height: node.height - 37
        active: node.spec.rubyNode
        sourceComponent: RubyCodeEditor {
            source: node.spec.rubySource
            status: node.spec.rubyStatus
            running: node.spec.rubyRunning
            failed: node.spec.rubyFailed
            manualPorts: node.spec.rubyManualPorts
            onDraftChanged: text => node.canvas.graph.setRubyDraft(node.nodeId, text)
            onRunRequested: portsOnly => runError = node.canvas.graph.runRuby(node.nodeId, portsOnly)
            onStopRequested: node.canvas.graph.stopRuby(node.nodeId)
        }
    }

    Repeater {
        model: node.spec.labeledInputs && !node.spec.rubyNode ? node.spec.inputs : []
        Label {
            required property var modelData
            x: 17
            width: (node.spec.dspNode || node.spec.rubyNode) ? 67 : node.spec.module ? node.width / 2 - 21 : node.width - 34
            elide: Text.ElideRight
            y: modelData.centerY - height / 2
            text: modelData.title
            color: Theme.secondaryText
            font.pixelSize: 12
        }
    }

    Repeater {
        model: node.spec.labeledOutputs && !node.spec.rubyNode ? node.spec.outputs : []
        Label {
            required property var modelData
            anchors.right: node.right
            anchors.rightMargin: 17
            width: (node.spec.dspNode || node.spec.rubyNode) ? 67
                : (node.spec.busNode || node.spec.module) ? Math.min(implicitWidth, node.width / 2 - 21) : implicitWidth
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
            y: modelData.centerY - height / 2
            text: modelData.title
            color: Theme.secondaryText
            font.pixelSize: 12
        }
    }

    component RubyPortName: TextInput {
        required property var modelData
        required property int index
        required property bool output
        objectName: output ? "rubyOutputName-" + index : "rubyInputName-" + index
        x: output ? node.width - 84 : 17
        y: modelData.centerY - height / 2
        width: 67
        height: 21
        verticalAlignment: TextInput.AlignVCenter
        horizontalAlignment: output ? TextInput.AlignRight : TextInput.AlignLeft
        clip: true
        color: activeFocus ? Theme.text : Theme.secondaryText
        selectionColor: Theme.selection
        font.pixelSize: 12
        selectByMouse: true
        text: modelData.title
        Accessible.name: output ? "Ruby output name" : "Ruby input name"

        onEditingFinished: {
            if (text !== modelData.title &&
                !node.canvas.graph.editRubyPort(node.nodeId, output, index, "name", 0, text.trim()))
                text = modelData.title
        }
    }

    Repeater {
        model: node.spec.rubyNode ? node.spec.inputs : []
        delegate: RubyPortName { output: false }
    }

    Repeater {
        model: node.spec.rubyNode ? node.spec.outputs : []
        delegate: RubyPortName { output: true }
    }

    Repeater {
        model: node.spec.rubyNode ? 2 : 0
        delegate: Rectangle {
            id: countGrip
            required property int index
            readonly property bool output: index === 1
            readonly property int count: output ? node.spec.outputs.length : node.spec.inputs.length
            property int previewCount: count
            objectName: output ? "rubyOutputCountGrip" : "rubyInputCountGrip"
            x: output ? node.width - width / 2 : -width / 2
            y: 42 + 24 * (countMouse.pressed ? previewCount : count) - height / 2
            width: 16
            height: 15
            radius: 3
            color: countMouse.containsMouse || countMouse.pressed ? Theme.hover : Theme.surface
            border.color: countMouse.pressed ? Theme.focus : Theme.outline
            Accessible.name: output ? "Change Ruby output count" : "Change Ruby input count"

            Text {
                anchors.centerIn: parent
                text: "⠿"
                color: Theme.secondaryText
                font.pixelSize: 13
            }

            MouseArea {
                id: countMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SizeVerCursor
                property real startY
                property int startCount
                property bool moved
                onPressed: mouse => {
                    node.canvas.selectNode(node.nodeId)
                    startY = mapToItem(node.canvas, mouse.x, mouse.y).y
                    startCount = countGrip.count
                    countGrip.previewCount = startCount
                    moved = false
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const delta = mapToItem(node.canvas, mouse.x, mouse.y).y - startY
                    moved = moved || Math.abs(delta) > 4
                    countGrip.previewCount = Math.max(0, Math.min(node.spec.rubyPortLimit,
                        startCount + Math.round(delta / (24 * node.canvas.viewZoom))))
                }
                onReleased: {
                    const target = moved ? countGrip.previewCount : Math.min(node.spec.rubyPortLimit, startCount + 1)
                    if (target !== countGrip.count)
                        node.canvas.graph.editRubyPort(node.nodeId, countGrip.output, -1, "count", target, "")
                }
                ToolTip.visible: containsMouse || pressed
                ToolTip.text: pressed ? countGrip.previewCount + (countGrip.output ? " outputs" : " inputs")
                                      : "Drag to add or remove connectors"
            }
        }
    }

    Loader {
        active: node.spec.rubyNode
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 5
        width: 15
        height: 15
        sourceComponent: Rectangle {
            radius: 3
            color: sizeMouse.containsMouse ? Theme.hover : "transparent"
            Text {
                anchors.centerIn: parent
                text: "◢"
                color: Theme.secondaryText
                font.pixelSize: 10
            }
            MouseArea {
                id: sizeMouse
                objectName: "rubyBoxResizeGrip"
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SizeFDiagCursor
                property point start
                property size initialSize
                onPressed: mouse => {
                    node.canvas.selectNode(node.nodeId)
                    start = mapToItem(node.canvas, mouse.x, mouse.y)
                    initialSize = Qt.size(node.width, node.height)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const p = mapToItem(node.canvas, mouse.x, mouse.y)
                    const minimumHeight = Math.max(220, 76 + 24 * Math.max(node.spec.inputs.length, node.spec.outputs.length))
                    node.rubyResizePreview = Qt.size(
                        Math.max(420, Math.min(1800, initialSize.width + (p.x - start.x) / node.canvas.viewZoom)),
                        Math.max(minimumHeight, Math.min(1600, initialSize.height + (p.y - start.y) / node.canvas.viewZoom)))
                }
                onReleased: {
                    const size = node.rubyResizePreview
                    if (size.width > 0)
                        node.canvas.graph.resizeRubyBox(node.nodeId, Math.round(size.width), Math.round(size.height))
                    node.rubyResizePreview = Qt.size(0, 0)
                }
                onCanceled: node.rubyResizePreview = Qt.size(0, 0)
                ToolTip.visible: containsMouse
                ToolTip.text: "Resize Ruby box"
            }
        }
    }

    component Connector: Rectangle {
        id: portIcon
        required property var port
        property bool highlighted: false
        property string direction
        readonly property bool pulse: port.type === "Trigger"
        readonly property bool untyped: port.type === "Template"
        readonly property bool hasPalette: !!port.palette
        width: 21
        height: width
        radius: width / 2
        y: port.centerY - height / 2
        antialiasing: true
        color: hasPalette ? "transparent" : port.bus ? (highlighted ? Theme.selected : Theme.hover)
              : untyped ? (highlighted ? Theme.hover : Theme.surface)
              : pulse ? (highlighted ? Theme.amberHover : Theme.amberSurface)
              : port.yellow ? (highlighted ? Theme.yellowHover : Theme.yellowSurface)
                     : (highlighted ? Theme.greenHover : Theme.greenSurface)
        border.color: port.bus ? Theme.secondaryText : untyped ? Theme.mutedText
                    : pulse ? Theme.amber : port.yellow ? Theme.yellow : Theme.green
        border.width: hasPalette ? 0 : highlighted ? 2 : 1
        Accessible.name: node.spec.title + " " + port.title + " " + direction + " (" + port.type + ")"

        Text {
            visible: !portIcon.hasPalette
            anchors.centerIn: parent
            text: portIcon.port.glyph
            color: portIcon.port.bus ? Theme.text : portIcon.pulse ? Theme.amberText
                 : portIcon.port.yellow ? Theme.yellowText : Theme.greenText
            font.pixelSize: portIcon.port.glyph.length > 1 ? 9 : 11
            font.bold: true
        }

        Loader {
            anchors.fill: parent
            active: portIcon.hasPalette
            sourceComponent: SignalConnectorIcon {
                connector: portIcon.port
                highlighted: portIcon.highlighted
            }
        }
    }

    Repeater {
        model: node.spec.inputs
        delegate: Connector {
            id: inputPort
            required property var modelData
            required property int index
            port: modelData
            direction: "input"
            x: -width / 2
            highlighted: inputMouse.containsMouse

            MouseArea {
                id: inputMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.CrossCursor
                onClicked: mouse => {
                    if (mouse.button === Qt.RightButton) {
                        if (inputPort.port.canChooseType) {
                            node.openTypePicker(inputPort, inputPort.index, 0)
                        } else {
                            node.canvas.graph.disconnectInput(node.nodeId, inputPort.index)
                        }
                    } else {
                        node.canvas.finishWire(node.nodeId, inputPort.index)
                    }
                }
            }
        }
    }

    Repeater {
        model: node.spec.outputs
        delegate: Connector {
            id: outputPort
            required property var modelData
            required property int index
            x: node.width - width / 2
            port: modelData
            direction: "output"
            highlighted: outputMouse.containsMouse || (node.canvas.wireSource === node.nodeId &&
                                                    node.canvas.wireOutput === index)

            MouseArea {
                id: outputMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: outputPort.port.canChooseType ? Qt.LeftButton | Qt.RightButton : Qt.LeftButton
                cursorShape: Qt.CrossCursor
                property point pressPoint
                property bool moved: false
                onPressed: mouse => {
                    if (mouse.button === Qt.RightButton)
                        return
                    pressPoint = mapToItem(node.canvas, mouse.x, mouse.y)
                    moved = false
                    node.canvas.startWire(node.nodeId, outputPort.index,
                                          Qt.point(node.x + node.width, node.y + outputPort.port.centerY),
                                          outputPort.port)
                }
                onPositionChanged: mouse => {
                    if (!(pressedButtons & Qt.LeftButton))
                        return
                    const p = mapToItem(node.canvas, mouse.x, mouse.y)
                    if (Math.hypot(p.x - pressPoint.x, p.y - pressPoint.y) > 4)
                        moved = true
                    if (moved)
                        node.canvas.wireEnd = node.canvas.worldAt(p.x, p.y)
                }
                onReleased: mouse => {
                    if (mouse.button === Qt.RightButton) {
                        node.openTypePicker(outputPort, -1, outputPort.index)
                        return
                    }
                    if (moved) {
                        const p = mapToItem(node.canvas, mouse.x, mouse.y)
                        node.canvas.dropWire(p.x, p.y)
                    }
                }
                onCanceled: node.canvas.cancelWire()
            }
        }
    }
}
