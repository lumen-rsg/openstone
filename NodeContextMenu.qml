pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

EditorMenu {
    id: menu

    required property SchematicModel graph
    required property var shortcuts
    property int nodeId: -1
    property var spec: ({})
    property int selectionCount: 0
    readonly property bool onNode: nodeId >= 0 && selectionCount === 1
    readonly property bool editableNode: onNode && spec.boundary !== true
    readonly property bool copyableSelection: editableNode || selectionCount > 1
    readonly property bool onModule: onNode && spec.module === true
    readonly property bool hasPeers: onModule && (spec.syncCount ?? 0) > 0
    signal command(string name)
    signal arrangeRequested(int arrangement)

    EditorMenuItem {
        text: "Cut"
        shortcutText: menu.shortcuts.cut
        enabled: menu.copyableSelection
        onTriggered: menu.command("cut")
    }
    EditorMenuItem {
        text: "Copy"
        shortcutText: menu.shortcuts.copy
        enabled: menu.copyableSelection
        onTriggered: menu.command("copy")
    }
    EditorMenuItem {
        text: "Paste"
        shortcutText: menu.shortcuts.paste
        enabled: menu.graph.canPaste
        onTriggered: menu.command("paste")
    }
    EditorMenuItem {
        text: "Paste Synchronize"
        shortcutText: menu.shortcuts.pasteSync
        enabled: menu.graph.canPasteSynchronized
        onTriggered: menu.command("pasteSync")
    }
    EditorMenuItem {
        text: "Duplicate"
        shortcutText: menu.shortcuts.duplicate
        enabled: menu.copyableSelection
        onTriggered: menu.command("duplicate")
    }

    MenuSeparator { visible: menu.onNode; height: visible ? implicitHeight : 0 }
    EditorMenuItem {
        text: "Rename…"
        shortcutText: menu.shortcuts.rename
        visible: menu.onNode
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("rename")
    }
    EditorMenuItem {
        text: "Delete"
        shortcutText: "Del"
        visible: menu.onNode
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("delete")
    }
    EditorMenuItem {
        text: "Disconnect all links"
        visible: menu.onNode && menu.spec.linked === true
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("disconnect")
    }

    MenuSeparator { visible: menu.selectionCount > 1; height: visible ? implicitHeight : 0 }
    EditorMenu {
        id: arrangeMenu
        title: "Align and Distribute"

        EditorMenuItem {
            text: "Align Horizontally"
            onTriggered: menu.arrangeRequested(SchematicModel.AlignHorizontally)
        }
        EditorMenuItem {
            text: "Align Vertically"
            onTriggered: menu.arrangeRequested(SchematicModel.AlignVertically)
        }
        MenuSeparator {}
        EditorMenuItem {
            text: "Distribute Horizontally"
            enabled: menu.selectionCount >= 3
            onTriggered: menu.arrangeRequested(SchematicModel.DistributeHorizontally)
        }
        EditorMenuItem {
            text: "Distribute Vertically"
            enabled: menu.selectionCount >= 3
            onTriggered: menu.arrangeRequested(SchematicModel.DistributeVertically)
        }
    }
    EditorMenuItem {
        text: "Make Module"
        shortcutText: menu.selectionCount > 1 ? menu.shortcuts.makeModule : ""
        visible: menu.selectionCount > 1 || menu.editableNode
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("makeModule")
    }

    MenuSeparator { visible: menu.onModule; height: visible ? implicitHeight : 0 }
    EditorMenuItem {
        text: "Enter module"
        shortcutText: "PgUp"
        visible: menu.onModule
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("enter")
    }
    EditorMenuItem {
        text: "Add to toolbox"
        visible: menu.onModule
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("toolbox")
    }
    EditorMenuItem {
        text: "Enable front panel"
        visible: menu.onModule && !menu.spec.hasFrontPanel
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("enableFront")
    }
    EditorMenuItem {
        text: menu.spec.panelHidden ? "Maximize front panel" : "Minimize front panel"
        visible: menu.onModule && menu.spec.hasFrontPanel && !menu.spec.propertiesOpen
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("panel")
    }
    EditorMenuItem {
        text: menu.spec.propertiesOpen ? "Close properties"
            : menu.spec.hasProperties ? "Show properties" : "Enable properties"
        visible: menu.onModule
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("properties")
    }
    EditorMenuItem {
        text: menu.spec.propertiesPinned ? "Unpin properties" : "Pin properties"
        visible: menu.onModule && menu.spec.hasProperties
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("pinProperties")
    }
    EditorMenuItem {
        text: menu.spec.showPanelInParent ? "Hide in parent panel" : "Show in parent panel"
        visible: menu.onModule && (menu.spec.hasFrontPanel || menu.spec.hasProperties)
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("showInParent")
    }
    EditorMenuItem {
        text: menu.spec.panelUnlocked ? "Lock panel" : "Edit panel"
        visible: menu.onModule && menu.spec.displayedPanel.bounds !== undefined
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("editPanel")
    }
    EditorMenuItem {
        text: "Focus mode"
        visible: menu.onModule && menu.spec.hasFrontPanel
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("focus")
    }
    EditorMenu {
        id: syncMenu
        title: "Synchronize"
        EditorMenuItem { text: "Synchronize All"; onTriggered: menu.command("syncAll") }
        EditorMenuItem { text: "Synchronize Painter"; onTriggered: menu.command("paint") }
        MenuSeparator {}
        EditorMenuItem {
            text: "Next synchronized module"
            shortcutText: "Tab"
            enabled: menu.hasPeers
            onTriggered: menu.command("next")
        }
        EditorMenuItem {
            text: "Previous synchronized module"
            shortcutText: "Shift+Tab"
            enabled: menu.hasPeers
            onTriggered: menu.command("previous")
        }
        MenuSeparator {}
        EditorMenuItem {
            text: "Unsynchronize"
            enabled: menu.spec.canUnsynchronize === true
            onTriggered: menu.command("unsync")
        }
        EditorMenuItem {
            text: "Unsynchronize All"
            enabled: menu.spec.canUnsynchronize === true
            onTriggered: menu.command("unsyncAll")
        }
    }

    MenuSeparator { visible: menu.graph.syncPainterActive; height: visible ? implicitHeight : 0 }
    EditorMenuItem {
        text: "Exit Synchronize Painter"
        shortcutText: "Esc"
        visible: menu.graph.syncPainterActive
        height: visible ? implicitHeight : 0
        onTriggered: menu.command("stopPaint")
    }

    Component.onCompleted: {
        for (let i = 0; i < count; ++i) {
            const item = itemAt(i) as MenuItem
            if (item && (item.subMenu === arrangeMenu || item.subMenu === syncMenu)) {
                item.visible = Qt.binding(() => item.subMenu === arrangeMenu ? menu.selectionCount > 1 : menu.onModule)
                item.height = Qt.binding(() => item.visible ? item.implicitHeight : 0)
            }
        }
    }
}
