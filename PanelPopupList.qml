pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: control

    required property var drawing
    required property SchematicModel graph
    required property DropListPopup menuHost
    property string menuToken: ""

    activeFocusOnTab: enabled
    enabled: drawing?.hasOptions ?? false
    Accessible.role: Accessible.ComboBox
    Accessible.name: "Popup List"
    Accessible.description: selectedLabel.text
    Accessible.onPressAction: control.openMenu(false, height)
    Keys.onSpacePressed: openMenu(false, height)
    Keys.onReturnPressed: openMenu(false, height)
    Keys.onEnterPressed: openMenu(false, height)

    function openMenu(held: bool, localY: real) {
        if (!enabled || !menuHost?.parent)
            return
        forceActiveFocus()
        const next = graph.openPopupList(drawing.path, drawing.node)
        if (next.token) {
            const origin = mapToItem(menuHost.parent, 0, localY)
            const right = mapToItem(menuHost.parent, width, localY)
            menuToken = next.token
            menuHost.presentPanelList(next, drawing, origin, right.x - origin.x, held)
        }
    }

    function closeMenu() {
        if (menuToken.length && menuHost)
            menuHost.finishPointerSelection(menuToken, false)
        menuToken = ""
    }

    onVisibleChanged: {
        if (!visible)
            Qt.callLater(control.closeMenu)
    }
    Component.onDestruction: {
        if (menuToken.length && menuHost)
            Qt.callLater(menuHost.finishPointerSelection, menuToken, false)
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.controlRadius
        color: pointer.pressed || pointer.containsMouse ? Theme.hover : "transparent"
        border.color: control.activeFocus ? Theme.focus : Theme.outline
    }

    Label {
        id: selectedLabel
        anchors.fill: parent
        leftPadding: 6
        rightPadding: 22
        verticalAlignment: Text.AlignVCenter
        textFormat: Text.PlainText
        text: control.drawing?.text ?? ""
        elide: Text.ElideRight
        color: control.drawing?.color ?? Theme.text
        font.family: control.drawing?.font.family ?? ""
        font.pixelSize: 14
    }

    Label {
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        text: "▾"
        color: selectedLabel.color
    }

    MouseArea {
        id: pointer
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        onPressed: mouse => control.openMenu(true, mouse.y)
        onPositionChanged: mouse => {
            if (pressed && control.menuToken.length) {
                const point = mapToItem(control.menuHost.parent, mouse.x, mouse.y)
                control.menuHost.trackPointer(control.menuToken, point)
            }
        }
        onReleased: {
            if (control.menuToken.length)
                control.menuHost.finishPointerSelection(control.menuToken, true)
            control.menuToken = ""
        }
        onCanceled: control.closeMenu()
    }
}
