pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import QtQuick.Shapes

FocusScope {
    id: schematic

    required property string name
    required property SchematicModel graph
    required property point viewCenter
    required property real viewZoom
    readonly property alias dropListHost: dropList
    readonly property alias undoShortcutText: undoShortcut.nativeText
    readonly property alias redoShortcutText: redoShortcut.nativeText
    readonly property alias cutShortcutText: cutShortcut.nativeText
    readonly property alias copyShortcutText: copyShortcut.nativeText
    readonly property alias pasteShortcutText: pasteShortcut.nativeText
    readonly property alias duplicateShortcutText: duplicateShortcut.nativeText
    readonly property alias selectAllShortcutText: selectAllShortcut.nativeText
    readonly property alias renameShortcutText: renameShortcut.nativeText
    readonly property alias makeModuleShortcutText: makeModuleShortcut.nativeText
    readonly property string hint: graph.syncPainterActive
        ? (graph.feedback || "Click a matching module · Esc exits Synchronize Painter")
        : wireSource >= 0 ? "Choose an input · Esc cancels"
                                                   : resetButton.hovered ? "Reset view" : graph.feedback
    property var selectedNodes: []
    readonly property int selectedNode: selectedNodes.length ? selectedNodes[selectedNodes.length - 1] : -1
    readonly property bool hasNodes: nodes.count > 0
    readonly property bool canCopySelection: {
        if (!selectedNodes.length || graph.level === 0)
            return selectedNodes.length > 0
        const selection = new Set(selectedNodes)
        for (let i = 0; i < nodes.count; ++i) {
            const node = nodes.itemAt(i)
            if (node && selection.has(node.nodeId) && node.spec.boundary)
                return false
        }
        return true
    }
    property bool selectingBox: false
    property point boxStart
    property point boxEnd
    readonly property rect selectionBox: Qt.rect(Math.min(boxStart.x, boxEnd.x), Math.min(boxStart.y, boxEnd.y),
                                                Math.abs(boxStart.x - boxEnd.x), Math.abs(boxStart.y - boxEnd.y))
    property int wireSource: -1
    property int wireOutput: 0
    property point wireStart
    property point wireEnd
    property bool wirePulse: false
    property bool wireYellow: false
    property string wirePalette: ""
    property bool wireBus: false
    property int insertStep: 0
    property string hoverKey: ""
    property var hoverTarget: null
    property var hoverReadout: null
    property point hoverPosition
    property var autoWire: null
    property bool cuttingWires: false

    signal panRequested(real dx, real dy)
    signal zoomRequested(real factor, real x, real y)
    signal resetRequested()

    focus: true
    clip: true
    Accessible.name: "Schematic canvas"

    DropListPopup {
        id: dropList
        onFindAnchor: (key, location) => {
            if (key === "") {
                dropList.anchorPoint = schematic.mapToItem(dropList.parent, location.x, location.y)
                dropList.anchorFound = true
            }
        }
        onCompleted: (token, selection) => schematic.graph.finishDropList(token, selection)
    }

    Connections {
        target: schematic.graph
        function onValuesChanged() {
            Qt.callLater(schematic.presentDropList)
        }
    }

    function presentDropList() {
        const request = graph.dropList
        dropList.present(request.windowKey ? {} : request)
    }

    function worldAt(screenX: real, screenY: real): point {
        return Qt.point(viewCenter.x + (screenX - width / 2) / viewZoom,
                        viewCenter.y + (screenY - height / 2) / viewZoom)
    }

    function insertNode(spec: var, screenX: real, screenY: real) {
        const p = worldAt(screenX, screenY)
        const x = p.x - spec.width / 2
        const y = p.y - spec.height / 2
        selectNode(spec.toolboxIndex !== undefined ? graph.addToolboxModule(spec.toolboxIndex, x, y)
                                                  : graph.addNode(spec.kind, x, y))
    }

    function insertAtCenter(spec: var) {
        const offset = 25 * (insertStep++ % 6)
        insertNode(spec, width / 2 + offset, height / 2 + offset)
    }

    function commitActiveEdit() {
        keyboardTarget.forceActiveFocus()
    }

    function updateAutoWire() {
        if (!graph.autoLink || graph.syncPainterActive || wireSource >= 0) {
            clearAutoWire()
            return
        }
        const candidate = graph.nearbyWire(viewZoom)
        if (candidate.source === undefined) {
            clearAutoWire()
            return
        }
        const same = autoWire && autoWire.source === candidate.source && autoWire.output === candidate.output &&
            autoWire.target === candidate.target && autoWire.input === candidate.input
        autoWire = candidate
        if (!same) autoWireDelay.restart()
    }

    function clearAutoWire() {
        autoWireDelay.stop()
        autoWire = null
    }

    function acceptAutoWire() {
        const candidate = autoWire
        clearAutoWire()
        if (candidate) graph.connectNodes(candidate.source, candidate.output, candidate.target, candidate.input)
    }

    function nearestWire(position: point): var {
        const p = worldAt(position.x, position.y)
        let nearest = 7 / viewZoom
        let connection = null
        for (let i = wires.count - 1; i >= 0; --i) {
            const wire = wires.itemAt(i) as GreenWire
            if (!wire) continue
            const distance = wire.distanceTo(p, nearest)
            if (distance < nearest) {
                nearest = distance
                connection = wire.connection
            }
        }
        return connection
    }

    function cutWires(from: point, to: point) {
        const steps = Math.max(1, Math.ceil(Math.hypot(to.x - from.x, to.y - from.y) / 7))
        for (let i = 1; i <= steps; ++i) {
            const t = i / steps
            const wire = nearestWire(Qt.point(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t))
            if (wire) graph.disconnectWire(wire.source, wire.output, wire.target, wire.input)
        }
    }

    Timer {
        id: autoWireDelay
        interval: 350
        onTriggered: schematic.acceptAutoWire()
    }

    Dialog {
        id: saveToToolbox
        property var moduleIds: []
        title: "Add to Toolbox"
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Save | Dialog.Cancel
        onAccepted: {
            for (const id of moduleIds) schematic.graph.addModuleToToolbox(id, toolboxGroup.text)
        }
        TextField {
            id: toolboxGroup
            width: 270
            placeholderText: "Group name (optional)"
            Accessible.name: "Toolbox group"
        }
    }

    EditorMenu {
        id: wireMenu
        property var wire
        EditorMenuItem {
            text: "Delete Link"
            onTriggered: {
                const wire = wireMenu.wire
                schematic.graph.disconnectWire(wire.source, wire.output, wire.target, wire.input)
            }
        }
    }

    function replaceSelection(id: int) {
        selectedNodes = id >= 0 ? [id] : []
    }

    function selectNode(id: int) {
        focusNode(id, false)
    }

    function focusNode(id: int, preserveSelection: bool) {
        clearReadout()
        keyboardTarget.forceActiveFocus()
        if (!preserveSelection || !selectedNodes.includes(id))
            replaceSelection(id)
        cancelWire()
    }

    function toggleSelection(id: int) {
        clearReadout()
        keyboardTarget.forceActiveFocus()
        cancelWire()
        const next = selectedNodes.slice()
        const index = next.indexOf(id)
        if (index < 0)
            next.push(id)
        else
            next.splice(index, 1)
        selectedNodes = next
    }

    function selectInRectangle(rect: rect, all: bool) {
        const next = all ? [] : selectedNodes.slice()
        for (let i = 0; i < nodes.count; ++i) {
            const item = nodes.itemAt(i) as GreenNode
            if (!item || item.spec.boundary)
                continue
            const position = item.mapToItem(schematic, 0, 0)
            const inside = position.x <= rect.x + rect.width && position.y <= rect.y + rect.height &&
                           position.x + item.width * viewZoom >= rect.x &&
                           position.y + item.height * viewZoom >= rect.y
            if ((all || inside) && !next.includes(item.nodeId))
                next.push(item.nodeId)
        }
        selectedNodes = next
    }

    function selectAllNodes() {
        selectInRectangle(Qt.rect(0, 0, 0, 0), true)
    }

    function makeModule() {
        if (!selectedNodes.length)
            return
        const previous = selectedNodes.slice()
        const id = graph.makeModule(previous)
        if (id >= 0)
            selectNode(id)
        else
            selectedNodes = previous
    }

    function openNodeMenu(id: int, spec: var, location: point) {
        if (!selectedNodes.includes(id))
            selectNode(id)
        nodeMenu.nodeId = id
        nodeMenu.spec = spec
        nodeMenu.selectionCount = selectedNodes.length
        menuLocation = worldAt(location.x, location.y)
        nodeMenu.popup(schematic, location.x, location.y)
    }

    property point menuLocation

    function pasteAt(location: point, synchronized: bool) {
        const previous = selectedNodes.slice()
        const pasted = graph.pasteNodes(location.x, location.y, synchronized)
        selectedNodes = pasted.length ? pasted : previous
        commitActiveEdit()
    }

    function pasteAtViewCenter() {
        pasteAt(Qt.point(viewCenter.x + 24, viewCenter.y + 24), false)
    }

    function copySelection() {
        graph.copyNodes(selectedNodes)
    }

    function cutSelection() {
        if (graph.cutNodes(selectedNodes))
            selectNode(-1)
    }

    function duplicateSelection() {
        const previous = selectedNodes.slice()
        const duplicates = graph.duplicateNodes(previous)
        selectedNodes = duplicates.length ? duplicates : previous
        commitActiveEdit()
    }

    function deleteNodes(ids: var) {
        if (!ids.length)
            return
        graph.removeNodes(ids.slice())
        selectNode(-1)
    }

    function followSync(id: int, previous: bool): bool {
        const target = graph.followSync(id, previous)
        if (target < 0)
            return false
        selectNode(target)
        return true
    }

    function paintModule(id: int) {
        selectNode(id)
        graph.paintSync(id)
    }

    function runNodeCommand(name: string, id: int, spec: var) {
        switch (name) {
        case "cut": schematic.cutSelection(); break
        case "copy": schematic.copySelection(); break
        case "paste": schematic.pasteAt(schematic.menuLocation, false); break
        case "pasteSync": schematic.pasteAt(schematic.menuLocation, true); break
        case "duplicate": schematic.duplicateSelection(); break
        case "rename": schematic.openRename(id, spec); break
        case "delete":
            schematic.deleteNodes(schematic.selectedNodes.includes(id) ? schematic.selectedNodes : [id])
            break
        case "disconnect": graph.disconnectNode(id); break
        case "makeModule": schematic.makeModule(); break
        case "enter": graph.enterModule(id); break
        case "panel": graph.setPanelHidden(id, !spec.panelHidden); break
        case "properties": graph.toggleProperties(id); break
        case "pinProperties": graph.setPropertiesVisible(id, true, !spec.propertiesPinned); break
        case "toolbox":
            saveToToolbox.moduleIds = schematic.selectedNodes.filter(target => schematic.findNode(target)?.spec.module)
            toolboxGroup.clear()
            if (saveToToolbox.moduleIds.length) saveToToolbox.open()
            break
        case "enableFront": graph.enableFrontPanel(id); break
        case "showInParent": graph.setPanelShownInParent(id, !spec.showPanelInParent); break
        case "editPanel": graph.setPanelUnlocked(id, !spec.panelUnlocked); break
        case "focus": graph.focusModule(id); break
        case "syncAll": graph.synchronizeAll(id); break
        case "paint": graph.startSyncPainter(id); break
        case "stopPaint": graph.stopSyncPainter(); break
        case "next": schematic.followSync(id, false); break
        case "previous": schematic.followSync(id, true); break
        case "unsync": graph.unsynchronize(id, false); break
        case "unsyncAll": graph.unsynchronize(id, true); break
        }
    }

    function findNode(id: int): GreenNode {
        for (let i = 0; i < nodes.count; ++i) {
            const item = nodes.itemAt(i) as GreenNode
            if (item?.nodeId === id)
                return item
        }
        return null
    }

    NodeContextMenu {
        id: nodeMenu
        graph: schematic.graph
        shortcuts: ({
            cut: cutShortcut.nativeText,
            copy: copyShortcut.nativeText,
            paste: pasteShortcut.nativeText,
            pasteSync: pasteSyncShortcut.nativeText,
            duplicate: duplicateShortcut.nativeText,
            rename: renameShortcut.nativeText,
            makeModule: makeModuleShortcut.nativeText
        })
        onCommand: name => schematic.runNodeCommand(name, nodeId, spec)
        onArrangeRequested: arrangement => {
            schematic.graph.arrangeNodes(schematic.selectedNodes, arrangement)
            schematic.commitActiveEdit()
        }
    }

    function renameSelection() {
        const item = findNode(selectedNode)
        if (item)
            openRename(item.nodeId, item.spec)
    }

    function openRename(id: int, spec: var) {
        renameDialog.nodeId = id
        renameField.text = spec.wirelessLabel ? spec.wirelessName : spec.boundary ? spec.boundaryName : spec.title
        renameDialog.open()
    }

    Dialog {
        id: renameDialog
        property int nodeId: -1
        title: "Rename"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 320
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: {
            renameField.forceActiveFocus()
            renameField.selectAll()
        }
        onAccepted: schematic.graph.renameNode(nodeId, renameField.text)
        contentItem: TextField {
            id: renameField
            Accessible.name: "Node name"
            selectByMouse: true
            onAccepted: renameDialog.accept()
        }
    }

    Shortcut {
        id: undoShortcut
        sequences: [StandardKey.Undo]
        enabled: keyboardTarget.activeFocus && schematic.graph.undoText.length > 0
        autoRepeat: false
        onActivated: schematic.graph.undo()
    }
    Shortcut {
        id: redoShortcut
        sequences: [StandardKey.Redo]
        enabled: keyboardTarget.activeFocus && schematic.graph.redoText.length > 0
        autoRepeat: false
        onActivated: schematic.graph.redo()
    }
    Shortcut {
        id: copyShortcut
        sequences: [StandardKey.Copy]
        enabled: keyboardTarget.activeFocus && schematic.selectedNodes.length > 0
        onActivated: schematic.copySelection()
    }
    Shortcut {
        id: cutShortcut
        sequences: [StandardKey.Cut]
        enabled: keyboardTarget.activeFocus && schematic.selectedNodes.length > 0
        onActivated: schematic.cutSelection()
    }
    Shortcut {
        id: pasteShortcut
        sequences: [StandardKey.Paste]
        enabled: keyboardTarget.activeFocus && schematic.graph.canPaste
        onActivated: schematic.pasteAtViewCenter()
    }
    Shortcut {
        id: pasteSyncShortcut
        sequence: "Ctrl+Shift+V"
        enabled: keyboardTarget.activeFocus && schematic.graph.canPasteSynchronized
        onActivated: schematic.pasteAt(Qt.point(schematic.viewCenter.x + 24, schematic.viewCenter.y + 24), true)
    }
    Shortcut {
        id: duplicateShortcut
        sequence: "Ctrl+D"
        enabled: keyboardTarget.activeFocus && schematic.selectedNodes.length > 0
        autoRepeat: false
        onActivated: schematic.duplicateSelection()
    }
    Shortcut {
        id: renameShortcut
        sequence: "Ctrl+R"
        enabled: keyboardTarget.activeFocus && schematic.selectedNodes.length === 1
        onActivated: schematic.renameSelection()
    }

    Shortcut {
        id: makeModuleShortcut
        sequence: "Ctrl+M"
        enabled: keyboardTarget.activeFocus && schematic.selectedNodes.length >= 2
        onActivated: schematic.makeModule()
    }
    Shortcut {
        id: selectAllShortcut
        sequence: StandardKey.SelectAll
        enabled: keyboardTarget.activeFocus
        onActivated: schematic.selectAllNodes()
    }

    function openTypePicker(id: int, input: int, anchor: Item, groups: var) {
        selectNode(id)
        connectorPicker.nodeId = id
        connectorPicker.input = input
        connectorPicker.rubyPort = null
        connectorPicker.groups = groups
        connectorPicker.openAt(anchor, input >= 0)
    }

    function openRubyPortPicker(id: int, output: bool, index: int, anchor: Item, port: var, count: int, limit: int) {
        selectNode(id)
        connectorPicker.nodeId = id
        connectorPicker.input = output ? -1 : index
        connectorPicker.rubyPort = { output, index, title: port.title, count, limit }
        connectorPicker.groups = graph.rubyPortTypes(output, port.typeId)
        connectorPicker.openAt(anchor, !output)
    }

    ConnectorTypePicker {
        id: connectorPicker
        property int nodeId: -1
        property int input: -1
        groups: []
        onTypeChosen: type => {
            if (rubyPort)
                schematic.graph.editRubyPort(nodeId, rubyPort.output, rubyPort.index, "type", type, "")
            else
                schematic.graph.setTemplateType(nodeId, type)
        }
        onRubyEditRequested: (command, value, label) =>
            schematic.graph.editRubyPort(nodeId, rubyPort.output, rubyPort.index, command, value, label)
        onDisconnectRequested: schematic.graph.disconnectInput(nodeId, input)
    }

    function openColorPicker(id: int, anchor: Item, value: color) {
        selectNode(id)
        colorPicker.openAt(id, anchor, value)
    }

    ColorPicker {
        id: colorPicker
        onColorChosen: (id, chosen) => schematic.graph.setColor(id, chosen)
    }

    function openBitmapFile(id: int) {
        selectNode(id)
        bitmapDialog.nodeId = id
        bitmapDialog.open()
    }

    Dialogs.FileDialog {
        id: bitmapDialog
        property int nodeId: -1
        title: "Open bitmap"
        onAccepted: schematic.graph.loadBitmap(nodeId, selectedFile)
    }

    function startWire(id: int, output: int, p: point, port: var) {
        clearReadout()
        keyboardTarget.forceActiveFocus()
        replaceSelection(id)
        // Losing focus can compile DSP code and replace the connector under this press.
        const currentPort = findNode(id)?.spec.outputs[output]
        if (!currentPort || currentPort.title !== port.title || currentPort.type !== port.type) {
            cancelWire()
            return
        }
        wireOutput = output
        wireSource = id
        wireStart = p
        wireEnd = p
        wirePulse = port.type === "Trigger"
        wireYellow = port.yellow
        wirePalette = port.palette ?? ""
        wireBus = port.bus ?? false
    }

    function cancelWire() {
        wireSource = -1
    }

    function finishWire(id: int, input: int) {
        if (wireSource >= 0) {
            graph.connectNodes(wireSource, wireOutput, id, input)
            cancelWire()
        }
    }

    function dropWire(screenX: real, screenY: real) {
        const p = worldAt(screenX, screenY)
        for (let i = nodes.count - 1; i >= 0; --i) {
            const target = nodes.itemAt(i) as GreenNode
            if (!target)
                continue
            const input = target.inputAt(p.x, p.y)
            if (input >= 0) {
                finishWire(target.nodeId, input)
                return
            }
        }
        cancelWire()
    }

    function clearReadout() {
        hoverDelay.stop()
        hoverKey = ""
        hoverTarget = null
        hoverReadout = null
    }

    function refreshReadout() {
        if (hoverTarget === null)
            return
        const target = hoverTarget
        const result = target.source === undefined ? graph.inspectPort(target.node, target.index, target.output)
                                                   : graph.inspectWire(target.source, target.output, target.target, target.input)
        hoverReadout = result.title ? result : null
    }

    function updateReadout(position: point) {
        if (!pointer.hovered || pointer.point.pressedButtons !== Qt.NoButton ||
                wireSource >= 0 || connectorPicker.visible || colorPicker.visible || nodeMenu.visible || renameDialog.visible ||
                bitmapDialog.visible ||
                zoomControls.contains(zoomControls.mapFromItem(schematic, position))) {
            clearReadout()
            return
        }
        const p = worldAt(position.x, position.y)
        let key = ""
        let target = null
        // The selected node draws above its neighbors.
        let front = null
        let frontPort = null
        for (let i = nodes.count - 1; i >= 0; --i) {
            const item = nodes.itemAt(i) as GreenNode
            if (!item || (front !== null && front.z >= item.z))
                continue
            const port = item.portAt(p.x, p.y)
            if (port !== null || item.contains(Qt.point(p.x - item.x, p.y - item.y))) {
                front = item
                frontPort = port
            }
        }
        if (front !== null) {
            if (frontPort !== null) {
                target = {node: front.nodeId, index: frontPort.index, output: frontPort.output}
                key = "port:" + front.nodeId + ":" + frontPort.output + ":" + frontPort.index
            }
        } else {
            target = nearestWire(position)
            if (target)
                key = "wire:" + target.source + ":" + target.output + ":" + target.target + ":" + target.input
        }
        if (key === "") {
            clearReadout()
        } else if (key !== hoverKey) {
            hoverKey = key
            hoverTarget = target
            hoverPosition = position
            hoverReadout = null
            hoverDelay.restart()
        }
    }

    HoverHandler {
        id: pointer
        onPointChanged: schematic.updateReadout(point.position)
        onHoveredChanged: {
            if (hovered)
                schematic.updateReadout(point.position)
            else
                schematic.clearReadout()
        }
    }

    Timer {
        id: hoverDelay
        interval: 550
        onTriggered: schematic.refreshReadout()
    }

    Timer {
        interval: 50
        repeat: true
        running: schematic.hoverReadout?.dspLive === true
        onTriggered: schematic.refreshReadout()
    }

    onViewCenterChanged: clearReadout()
    onViewZoomChanged: clearReadout()
    onSelectedNodesChanged: {
        if (graph)
            graph.closeUnpinnedProperties(selectedNodes.length === 1 ? selectedNodes[0] : -1)
    }

    Connections {
        target: schematic.graph
        function onLevelChanged() {
            schematic.clearAutoWire()
            schematic.selectedNodes = []
            schematic.cancelWire()
            colorPicker.close()
            bitmapDialog.close()
            nodeMenu.close()
            renameDialog.close()
            schematic.insertStep = 0
        }
        function onGeometryChanged() { schematic.clearReadout() }
        function onValuesChanged() {
            if (schematic.hoverReadout !== null)
                schematic.refreshReadout()
        }
        function onDspCaptureChanged() {
            if (schematic.hoverReadout?.dspPalette)
                schematic.refreshReadout()
        }
        function onAudioChanged() {
            if (schematic.hoverReadout?.dspPalette)
                schematic.refreshReadout()
        }
        function onWiringPreferencesChanged() {
            schematic.clearAutoWire()
            schematic.clearReadout()
        }
    }

    Item {
        id: keyboardTarget
        focus: true
    }

    Keys.enabled: keyboardTarget.activeFocus
    Keys.onLeftPressed: schematic.panRequested(48, 0)
    Keys.onRightPressed: schematic.panRequested(-48, 0)
    Keys.onUpPressed: schematic.panRequested(0, 48)
    Keys.onDownPressed: schematic.panRequested(0, -48)
    Keys.onPressed: event => {
        switch (event.key) {
        case Qt.Key_Delete:
        case Qt.Key_Backspace:
            schematic.deleteNodes(schematic.selectedNodes)
            break
        case Qt.Key_PageUp:
            if (schematic.selectedNodes.length === 1)
                schematic.graph.enterModule(schematic.selectedNode)
            break
        case Qt.Key_PageDown:
            schematic.graph.navigate(schematic.graph.level - 1)
            break
        case Qt.Key_Tab:
        case Qt.Key_Backtab:
            if (schematic.selectedNodes.length !== 1 ||
                !schematic.followSync(schematic.selectedNode,
                    event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier) !== 0))
                return
            break
        case Qt.Key_Escape:
            schematic.clearAutoWire()
            schematic.graph.stopSyncPainter()
            schematic.cancelWire()
            schematic.selectingBox = false
            schematic.clearReadout()
            break
        case Qt.Key_Plus:
        case Qt.Key_Equal:
            schematic.zoomRequested(1.2, 0, 0)
            break
        case Qt.Key_Minus:
            schematic.zoomRequested(1 / 1.2, 0, 0)
            break
        case Qt.Key_0:
            schematic.resetRequested()
            break
        default:
            if (!keyboardTarget.activeFocus || event.isAutoRepeat || schematic.graph.syncPainterActive ||
                    (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.AltModifier))) return
            const spec = schematic.graph.primitiveShortcut(event.text)
            if (spec.kind === undefined) return
            if (pointer.hovered)
                schematic.insertNode(spec, pointer.point.position.x, pointer.point.position.y)
            else
                schematic.insertAtCenter(spec)
        }
        event.accepted = true
    }

    CanvasGrid {
        anchors.fill: parent
        center: schematic.viewCenter
        zoom: schematic.viewZoom
        dotColor: Theme.grid
    }

    Column {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 17
        spacing: 3

        Label { text: schematic.name; font.bold: true }
        Label {
            text: schematic.selectedNodes.length > 1 ? schematic.selectedNodes.length + " selected"
                                   : nodes.count === 0 ? "Add a node from the library"
                                   : nodes.count + (nodes.count === 1 ? " node" : " nodes")
            color: Theme.mutedText
            font.pixelSize: 12
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property point lastPosition

        hoverEnabled: true
        onPressed: mouse => {
            if (mouse.button === Qt.LeftButton && (mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier))) {
                schematic.clearReadout()
                schematic.cancelWire()
                keyboardTarget.forceActiveFocus()
                schematic.graph.beginWireDeletion()
                schematic.cuttingWires = true
                lastPosition = Qt.point(mouse.x, mouse.y)
                schematic.cutWires(lastPosition, lastPosition)
                return
            }
            if (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ShiftModifier) &&
                    !schematic.graph.syncPainterActive) {
                keyboardTarget.forceActiveFocus()
                schematic.cancelWire()
                schematic.boxStart = Qt.point(mouse.x, mouse.y)
                schematic.boxEnd = schematic.boxStart
                schematic.selectingBox = true
                return
            }
            if (mouse.button !== Qt.RightButton) {
                schematic.selectNode(-1)
                lastPosition = Qt.point(mouse.x, mouse.y)
            }
        }
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton) {
                const wire = schematic.nearestWire(Qt.point(mouse.x, mouse.y))
                if (wire) {
                    wireMenu.wire = wire
                    wireMenu.popup()
                } else {
                    schematic.openNodeMenu(-1, {}, Qt.point(mouse.x, mouse.y))
                }
            }
        }
        onReleased: {
            if (schematic.cuttingWires) schematic.graph.finishEdit()
            schematic.cuttingWires = false
            if (schematic.selectingBox) {
                const box = schematic.selectionBox
                if (box.width >= 4 || box.height >= 4)
                    schematic.selectInRectangle(box, false)
                schematic.selectingBox = false
            }
        }
        onCanceled: {
            schematic.selectingBox = false
            if (schematic.cuttingWires) schematic.graph.finishEdit()
            schematic.cuttingWires = false
        }
        onPositionChanged: mouse => {
            if (schematic.cuttingWires) {
                const position = Qt.point(mouse.x, mouse.y)
                if (mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier))
                    schematic.cutWires(lastPosition, position)
                lastPosition = position
                return
            }
            if (schematic.selectingBox) {
                schematic.boxEnd = Qt.point(mouse.x, mouse.y)
                return
            }
            if (!pressed) {
                if (schematic.wireSource >= 0)
                    schematic.wireEnd = schematic.worldAt(mouse.x, mouse.y)
                return
            }
            if (!(mouse.buttons & Qt.RightButton))
                schematic.panRequested(mouse.x - lastPosition.x, mouse.y - lastPosition.y)
            lastPosition = Qt.point(mouse.x, mouse.y)
        }
        onWheel: wheel => {
            const delta = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.angleDelta.y
            if (delta !== 0) {
                const step = Math.max(-240, Math.min(240, delta))
                schematic.zoomRequested(Math.exp(step / 650), wheel.x - width / 2, wheel.y - height / 2)
                wheel.accepted = true
            }
        }
    }

    Item {
        x: schematic.width / 2 - schematic.viewCenter.x * schematic.viewZoom
        y: schematic.height / 2 - schematic.viewCenter.y * schematic.viewZoom
        scale: schematic.viewZoom
        transformOrigin: Item.TopLeft

        Repeater {
            id: wires
            model: schematic.graph.wires
            GreenWire {
                required property var modelData
                connection: modelData
                start: modelData.start
                end: modelData.end
                pulse: modelData.pulse
                untyped: modelData.untyped
                yellow: modelData.yellow
                palette: modelData.palette
                bus: modelData.bus
                straight: schematic.graph.straightWires
            }
        }

        GreenWire {
            visible: schematic.wireSource >= 0
            start: schematic.wireStart
            end: schematic.wireEnd
            pulse: schematic.wirePulse
            yellow: schematic.wireYellow
            palette: schematic.wirePalette
            bus: schematic.wireBus
            preview: true
            straight: schematic.graph.straightWires
        }

        GreenWire {
            visible: schematic.autoWire !== null
            start: schematic.autoWire?.start ?? Qt.point(0, 0)
            end: schematic.autoWire?.end ?? Qt.point(0, 0)
            preview: true
            straight: schematic.graph.straightWires
        }

        Repeater {
            id: nodes
            model: schematic.graph
            delegate: GreenNode {
                required property var nodeSpec
                required property point nodePosition
                required property bool nodeEditable
                canvas: schematic
                spec: nodeSpec
                x: nodePosition.x
                y: nodePosition.y
                editable: nodeEditable
            }
        }
    }

    Rectangle {
        visible: schematic.selectingBox
        x: schematic.selectionBox.x
        y: schematic.selectionBox.y
        width: schematic.selectionBox.width
        height: schematic.selectionBox.height
        color: Qt.rgba(Theme.focus.r, Theme.focus.g, Theme.focus.b, 0.16)
        border.color: Theme.focus
    }

    Pane {
        id: zoomControls
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        padding: 4
        background: Rectangle {
            color: Theme.chrome
            border.color: Theme.outline
            radius: Theme.panelRadius
        }

        RowLayout {
            spacing: 2

            FlatButton {
                text: "−"
                Accessible.name: "Zoom out"
                onClicked: schematic.zoomRequested(1 / 1.2, 0, 0)
            }
            Label {
                Layout.minimumWidth: 44
                horizontalAlignment: Text.AlignHCenter
                color: Theme.secondaryText
                font.pixelSize: 12
                text: Math.round(schematic.viewZoom * 100) + "%"
            }
            FlatButton {
                text: "+"
                Accessible.name: "Zoom in"
                onClicked: schematic.zoomRequested(1.2, 0, 0)
            }
            FlatButton {
                id: resetButton
                text: "Reset view"
                onClicked: schematic.resetRequested()
            }
        }
    }

    Rectangle {
        id: readout
        visible: schematic.hoverReadout !== null
        width: Math.min(schematic.width - 16, Math.max(132, Math.min(288, readoutContent.implicitWidth + 22)))
        height: readoutContent.implicitHeight + 18
        x: Math.max(8, Math.min(schematic.width - width - 8, schematic.hoverPosition.x + 16))
        y: Math.max(8, schematic.hoverPosition.y + height + 18 <= schematic.height
                    ? schematic.hoverPosition.y + 12 : schematic.hoverPosition.y - height - 10)
        radius: Theme.nodeRadius
        color: Theme.surface
        border.color: Theme.outline

        ColumnLayout {
            id: readoutContent
            x: 11
            y: 9
            width: Math.max(0, readout.width - 22)
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: schematic.hoverReadout ? schematic.hoverReadout.title + " · " + schematic.hoverReadout.type : ""
                color: Theme.secondaryText
                font.pixelSize: 11
                font.bold: true
                elide: Text.ElideRight
            }
            Loader {
                Layout.fillWidth: true
                Layout.preferredWidth: 218
                active: schematic.hoverReadout?.dspSamples !== undefined
                visible: active
                sourceComponent: DspSwatch {
                    samples: schematic.hoverReadout?.dspSamples ?? []
                    paletteName: schematic.hoverReadout?.dspPalette ?? "stream"
                }
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: schematic.hoverReadout?.dspStatus ?? ""
                color: Theme.secondaryText
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            ColorSwatch {
                Layout.fillWidth: true
                Layout.preferredHeight: 18
                visible: schematic.hoverReadout?.swatch !== undefined
                fill: schematic.hoverReadout?.swatch ?? "transparent"
            }
            ColorSwatch {
                Layout.fillWidth: true
                Layout.preferredWidth: 204
                Layout.preferredHeight: 104
                visible: schematic.hoverReadout?.bitmapPreview !== undefined
                fill: "transparent"

                Image {
                    anchors.fill: parent
                    source: schematic.hoverReadout?.bitmapPreview ?? ""
                    fillMode: Image.PreserveAspectFit
                    cache: false
                }
            }
            Item {
                id: strokePreview
                readonly property var stroke: schematic.hoverReadout?.stroke ?? null
                Layout.fillWidth: true
                Layout.preferredHeight: 20
                visible: stroke !== null

                ColorSwatch { anchors.fill: parent; fill: "transparent" }
                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeColor: strokePreview.stroke?.color ?? "transparent"
                        strokeWidth: Math.max(1, Math.min(10, (strokePreview.stroke?.width ?? 0) * 24))
                        strokeStyle: strokePreview.stroke?.dashes.length > 0
                                     ? ShapePath.DashLine : ShapePath.SolidLine
                        dashPattern: strokePreview.stroke?.dashes.length > 0 ? strokePreview.stroke.dashes : [4, 2]
                        capStyle: ShapePath.FlatCap
                        fillColor: "transparent"
                        startX: 4
                        startY: strokePreview.height / 2
                        PathLine { x: strokePreview.width - 4; y: strokePreview.height / 2 }
                    }
                }
            }
            Label {
                id: fontSample
                readonly property var sample: schematic.hoverReadout?.fontSample ?? null
                Layout.fillWidth: true
                visible: sample !== null
                text: "Aa Bb Cc 123"
                color: Theme.text
                elide: Text.ElideRight
                font.family: sample?.family ?? ""
                font.pixelSize: sample?.pixels ?? 16
                font.bold: sample?.bold ?? false
                font.italic: sample?.italic ?? false
                font.underline: sample?.underline ?? false
                font.strikeout: sample?.strikeout ?? false
            }
            GridLayout {
                Layout.fillWidth: true
                visible: schematic.hoverReadout?.matrix !== undefined
                columns: 5
                columnSpacing: 4
                rowSpacing: 3
                Repeater {
                    model: schematic.hoverReadout?.matrix ?? []
                    Label {
                        required property string modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 40
                        Layout.minimumWidth: 0
                        horizontalAlignment: Text.AlignHCenter
                        text: modelData
                        elide: Text.ElideRight
                        font.pixelSize: 12
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: schematic.hoverReadout?.value ?? ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 5
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: 16
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: schematic.hoverReadout?.triggers === undefined ? ""
                      : "↯  " + schematic.hoverReadout.triggers + " triggers"
                color: Theme.amberText
                font.pixelSize: 11
            }
        }
    }
}
