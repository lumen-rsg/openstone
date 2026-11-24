pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs

ApplicationWindow {
    id: window

    visible: !graphModel.focusedPanel.key
    width: 1200
    height: 760
    minimumWidth: 800
    minimumHeight: 480
    title: graphModel.projectFile.toString() ? graphModel.projectFile.toString().split("/").pop() + " — OpenStone" : "OpenStone"
    color: Theme.chrome
    font.pixelSize: 14

    palette.window: Theme.chrome
    palette.windowText: Theme.text
    palette.base: Theme.field
    palette.text: Theme.text
    palette.button: Theme.surface
    palette.buttonText: Theme.text
    palette.highlight: Theme.selection
    palette.highlightedText: Theme.text
    palette.placeholderText: Theme.mutedText
    palette.toolTipBase: Theme.hover
    palette.toolTipText: Theme.text

    readonly property string graphicsApi: {
        switch (canvas.GraphicsInfo.api) {
        case GraphicsInfo.Metal: return "Metal"
        case GraphicsInfo.Direct3D11: return "Direct3D 11"
        case GraphicsInfo.Direct3D12: return "Direct3D 12"
        case GraphicsInfo.Vulkan: return "Vulkan"
        case GraphicsInfo.OpenGL: return "OpenGL"
        case GraphicsInfo.OpenGLNoProfile: return "OpenGL"
        case GraphicsInfo.OpenGLCoreProfile: return "OpenGL"
        case GraphicsInfo.OpenGLCompatibilityProfile: return "OpenGL"
        case GraphicsInfo.Software: return "Software"
        default: return "—"
        }
    }
    property string hint: ""
    property var draggedPrimitive: null
    property point dragPoint
    property string pendingAction: ""
    property bool allowClose: false

    function performAction(action: string) {
        if (action === "open") openFile.open()
        else if (action === "new") graphModel.newProject()
        else if (action === "close") graphModel.closeSchematic()
    }

    function requestAction(action: string) {
        if (graphModel.closePending || unsavedDialog.visible || saveAs.visible || openFile.visible) return
        canvas.commitActiveEdit()
        if (graphModel.hasUnsavedChanges()) {
            pendingAction = action
            unsavedDialog.open()
        } else {
            performAction(action)
        }
    }

    function saveFile() {
        canvas.commitActiveEdit()
        if (!graphModel.projectFile.toString()) {
            saveAs.open()
        } else {
            const saved = graphModel.saveProject(graphModel.projectFile)
            const action = pendingAction
            pendingAction = ""
            if (saved) performAction(action)
        }
    }

    onClosing: event => {
        if (allowClose) return
        event.accepted = false
        requestAction("close")
    }

    Dialogs.FileDialog {
        id: openFile
        title: "Open Schematic"
        nameFilters: ["Schematics (*.ostone *.fsm)", "OpenStone schematics (*.ostone)", "FlowStone schematics (*.fsm)", "All files (*)"]
        onAccepted: graphModel.openProject(selectedFile)
    }

    Dialogs.FileDialog {
        id: saveAs
        title: "Save Schematic"
        fileMode: Dialogs.FileDialog.SaveFile
        defaultSuffix: "ostone"
        nameFilters: ["OpenStone schematics (*.ostone)"]
        onAccepted: {
            const saved = graphModel.saveProject(selectedFile)
            const action = window.pendingAction
            window.pendingAction = ""
            if (saved) window.performAction(action)
        }
        onRejected: window.pendingAction = ""
    }

    Dialog {
        id: unsavedDialog
        title: "Save changes?"
        modal: true
        anchors.centerIn: parent
        onRejected: window.pendingAction = ""
        Label { text: "Save this schematic before continuing?" }
        footer: DialogButtonBox {
            Button {
                text: "Cancel"
                onClicked: unsavedDialog.reject()
            }
            Button {
                text: "Discard"
                onClicked: {
                    const action = window.pendingAction
                    window.pendingAction = ""
                    unsavedDialog.close()
                    window.performAction(action)
                }
            }
            Button {
                text: "Save"
                onClicked: {
                    unsavedDialog.close()
                    window.saveFile()
                }
            }
        }
    }

    Shortcut {
        id: newShortcut
        sequences: [StandardKey.New]
        onActivated: window.requestAction("new")
    }
    Shortcut {
        id: openShortcut
        sequences: [StandardKey.Open]
        onActivated: window.requestAction("open")
    }
    Shortcut {
        id: saveShortcut
        sequences: [StandardKey.Save]
        onActivated: window.saveFile()
    }
    Shortcut {
        id: saveAsShortcut
        sequences: [StandardKey.SaveAs]
        onActivated: { canvas.commitActiveEdit(); saveAs.open() }
    }
    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: library.focusSearch()
    }

    SchematicModel {
        id: graphModel
        Component.onCompleted: {
            registerTickWindow(window)
            watchApplicationQuit()
        }
        onApplicationQuitRequested: {
            if (graphModel.focusedPanel.key && !graphModel.closePending && graphModel.hasUnsavedChanges())
                graphModel.closeFocusMode()
            window.requestAction("close")
        }
        onSchematicCloseReady: {
            window.allowClose = true
            if (graphModel.focusedPanel.key) graphModel.closeFocusMode()
            window.close()
            Qt.quit()
        }
    }
    PrimitiveDialogs {
        graph: graphModel
        fallbackWindow: window
    }

    SchematicWindows {
        graph: graphModel
        hostWindow: window
    }

    header: ToolBar {
        implicitHeight: 49
        padding: 0
        background: Rectangle { color: Theme.chrome }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 16
            spacing: 4

            OpenStoneMark {
                Layout.rightMargin: 6
            }
            FlatButton {
                text: "File"
                onClicked: fileMenu.open()
                EditorMenu {
                    id: fileMenu
                    y: parent.height
                    EditorMenuItem {
                        text: "New"
                        shortcutText: newShortcut.nativeText
                        onTriggered: window.requestAction("new")
                    }
                    EditorMenuItem {
                        text: "Open…"
                        shortcutText: openShortcut.nativeText
                        onTriggered: window.requestAction("open")
                    }
                    EditorMenuItem {
                        text: "Save"
                        shortcutText: saveShortcut.nativeText
                        onTriggered: window.saveFile()
                    }
                    EditorMenuItem {
                        text: "Save As…"
                        shortcutText: saveAsShortcut.nativeText
                        onTriggered: { canvas.commitActiveEdit(); saveAs.open() }
                    }
                }
            }
            FlatButton {
                text: "Edit"
                onClicked: editMenu.open()
                EditorMenu {
                    id: editMenu
                    y: parent.height
                    EditorMenuItem {
                        text: graphModel.undoText || "Undo"
                        shortcutText: canvas.undoShortcutText
                        enabled: graphModel.undoText.length > 0
                        onTriggered: graphModel.undo()
                    }
                    EditorMenuItem {
                        text: graphModel.redoText || "Redo"
                        shortcutText: canvas.redoShortcutText
                        enabled: graphModel.redoText.length > 0
                        onTriggered: graphModel.redo()
                    }
                    MenuSeparator {}
                    EditorMenuItem {
                        text: "Cut"
                        shortcutText: canvas.cutShortcutText
                        enabled: canvas.canCopySelection
                        onTriggered: canvas.cutSelection()
                    }
                    EditorMenuItem {
                        text: "Copy"
                        shortcutText: canvas.copyShortcutText
                        enabled: canvas.canCopySelection
                        onTriggered: canvas.copySelection()
                    }
                    EditorMenuItem {
                        text: "Paste"
                        shortcutText: canvas.pasteShortcutText
                        enabled: graphModel.canPaste
                        onTriggered: canvas.pasteAtViewCenter()
                    }
                    EditorMenuItem {
                        text: "Duplicate"
                        shortcutText: canvas.duplicateShortcutText
                        enabled: canvas.canCopySelection
                        onTriggered: canvas.duplicateSelection()
                    }
                    MenuSeparator {}
                    EditorMenuItem {
                        text: "Rename…"
                        shortcutText: canvas.renameShortcutText
                        enabled: canvas.selectedNodes.length === 1
                        onTriggered: canvas.renameSelection()
                    }
                    EditorMenuItem {
                        text: "Delete"
                        shortcutText: "Del"
                        enabled: canvas.selectedNodes.length > 0
                        onTriggered: canvas.deleteNodes(canvas.selectedNodes)
                    }
                    EditorMenuItem {
                        text: "Make Module"
                        shortcutText: canvas.selectedNodes.length === 1 ? "" : canvas.makeModuleShortcutText
                        enabled: canvas.canCopySelection
                        onTriggered: canvas.makeModule()
                    }
                    MenuSeparator {}
                    EditorMenuItem {
                        text: "Select All"
                        shortcutText: canvas.selectAllShortcutText
                        enabled: canvas.hasNodes
                        onTriggered: canvas.selectAllNodes()
                    }
                }
            }
            FlatButton {
                text: "Wiring"
                onClicked: wiringMenu.open()
                EditorMenu {
                    id: wiringMenu
                    y: parent.height
                    EditorMenuItem {
                        text: "Straight wires"
                        checkable: true
                        checked: graphModel.straightWires
                        onTriggered: graphModel.straightWires = checked
                    }
                    EditorMenuItem {
                        text: "Auto-link nearby ports"
                        checkable: true
                        checked: graphModel.autoLink
                        onTriggered: graphModel.autoLink = checked
                    }
                }
            }
            FlatButton {
                text: graphModel.sequencerMode === 1 ? "Recording…" : graphModel.sequencerMode === 2 ? "Playing…" : "Sequencer"
                visible: graphModel.hasSequencerTracks
                onClicked: sequenceMenu.open()
                EditorMenu {
                    id: sequenceMenu
                    y: parent.height
                    EditorMenuItem {
                        text: "Record from start"
                        enabled: graphModel.sequencerMode !== 1
                        onTriggered: graphModel.setSequencerMode(1)
                    }
                    EditorMenuItem {
                        text: "Play from start"
                        enabled: graphModel.sequencerMode !== 2
                        onTriggered: graphModel.setSequencerMode(2)
                    }
                    EditorMenuItem {
                        text: "Stop"
                        enabled: graphModel.sequencerMode !== 0
                        onTriggered: graphModel.setSequencerMode(0)
                    }
                }
            }
            Item { Layout.fillWidth: true }
            FlatButton {
                text: graphModel.audioStarting ? "Starting Audio…" : graphModel.audioRunning ? "Stop Audio" : "Start Audio"
                enabled: !graphModel.audioStarting
                filled: true
                warm: graphModel.audioRunning
                Layout.preferredWidth: 122
                Layout.preferredHeight: 30
                onClicked: graphModel.toggleAudio()
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 8

        LibraryBrowser {
            id: library
            Layout.preferredWidth: Math.min(408, Math.max(338, Math.round(window.width * 0.36)))
            Layout.fillHeight: true
            graph: graphModel
            onInsertRequested: entry => canvas.insertAtCenter(entry)
            onContextRequested: (entry, position) => {
                savedModuleMenu.entry = entry
                savedModuleMenu.popup(library, position.x, position.y)
            }
            onDragMoved: (entry, position) => {
                window.draggedPrimitive = entry
                window.dragPoint = mapToItem(window.contentItem, position.x, position.y)
            }
            onDropRequested: (entry, position) => {
                const target = mapToItem(canvas, position.x, position.y)
                if (canvas.contains(target))
                    canvas.insertNode(entry, target.x, target.y)
            }
            onDragFinished: window.draggedPrimitive = null
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.canvas
            radius: Theme.panelRadius
            border.color: Theme.divider

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0
                Flow {
                    id: pathFlow
                    Layout.fillWidth: true
                    Layout.margins: 9
                    spacing: 5

                    Repeater {
                        model: graphModel.level + 1
                        delegate: Row {
                            id: levelRow

                            required property int index
                            readonly property var levelData: graphModel.breadcrumbs[index]
                            visible: levelData !== undefined
                            spacing: 5

                            Label {
                                visible: levelRow.index > 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: "›"
                                font.pixelSize: 16
                                color: Theme.mutedText
                            }
                            Breadcrumb {
                                name: levelRow.levelData?.name ?? ""
                                current: levelRow.index === graphModel.level
                                viewCenter: levelRow.levelData?.center ?? Qt.point(0, 0)
                                viewZoom: levelRow.levelData?.zoom ?? 1
                                canvasWidth: canvas.width
                                canvasHeight: canvas.height
                                nodeBounds: levelRow.levelData?.nodeBounds ?? []
                                onActivated: {
                                    const level = levelRow.index
                                    if (level < graphModel.level) {
                                        canvas.selectNode(-1)
                                        graphModel.navigate(level)
                                    }
                                }
                                onCenterRequested: (x, y) => graphModel.centerOn(levelRow.index, x, y)
                            }
                        }
                    }
                }
                Rectangle { implicitHeight: 1; Layout.fillWidth: true; color: Theme.divider }

                SchematicCanvas {
                    id: canvas
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 2
                    viewCenter: graphModel.center
                    viewZoom: graphModel.zoom
                    name: graphModel.breadcrumbs[graphModel.level]?.name ?? "Main"
                    graph: graphModel
                    onPanRequested: (dx, dy) => graphModel.pan(dx, dy)
                    onZoomRequested: (factor, x, y) => graphModel.zoomAt(factor, x, y)
                    onResetRequested: graphModel.resetView()
                    onHintChanged: window.hint = canvas.hint
                }
            }
        }
    }

    EditorMenu {
        id: savedModuleMenu
        property var entry
        EditorMenuItem {
            text: "Rename or Move…"
            onTriggered: {
                editSavedModule.entry = savedModuleMenu.entry
                savedName.text = savedModuleMenu.entry.title
                savedGroup.text = savedModuleMenu.entry.toolboxGroup
                editSavedModule.open()
            }
        }
        EditorMenuItem {
            text: "Remove from Toolbox…"
            onTriggered: {
                removeSavedModule.entry = savedModuleMenu.entry
                removeSavedModule.open()
            }
        }
    }

    Dialog {
        id: editSavedModule
        property var entry
        title: "Edit Saved Module"
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Save | Dialog.Cancel
        onAccepted: graphModel.editToolboxModule(entry.toolboxIndex, savedName.text, savedGroup.text)
        ColumnLayout {
            TextField { id: savedName; placeholderText: "Module name"; Layout.preferredWidth: 260 }
            TextField { id: savedGroup; placeholderText: "Group name"; Layout.fillWidth: true }
        }
    }

    Dialog {
        id: removeSavedModule
        property var entry
        title: "Remove Saved Module?"
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: graphModel.removeToolboxModule(entry.toolboxIndex)
        Label { text: removeSavedModule.entry?.title ?? "" }
    }

    Rectangle {
        visible: window.draggedPrimitive !== null
        z: 10
        x: window.dragPoint.x + 11
        y: window.dragPoint.y + 11
        width: dragLabel.implicitWidth + 22
        height: 30
        radius: Theme.controlRadius
        color: Theme.hover
        border.color: Theme.focus
        Label {
            id: dragLabel
            anchors.centerIn: parent
            text: window.draggedPrimitive?.title ?? ""
        }
    }

    footer: Rectangle {
        implicitHeight: 34
        color: Theme.chrome

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 15

            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: graphModel.closePending
                    ? (graphModel.feedback || "Waiting for About To Close · Done")
                    : (window.hint || "Drag from the library · Drag from output to input")
                color: Theme.secondaryText
                font.pixelSize: 12
            }
            Label { text: "Renderer  " + window.graphicsApi; color: Theme.secondaryText; font.pixelSize: 12 }
            Label { text: "CPU  —"; color: Theme.mutedText; font.pixelSize: 12 }
            Label { text: "GPU  —"; color: Theme.mutedText; font.pixelSize: 12 }
            Label { text: "Triggers  " + graphModel.triggerCount; color: Theme.secondaryText; font.pixelSize: 12 }
            Label {
                text: graphModel.audioSampleRate > 0 ? "SR  " + graphModel.audioSampleRate + " Hz" : "SR  —"
                color: Theme.mutedText
                font.pixelSize: 12
            }
        }
    }
}
