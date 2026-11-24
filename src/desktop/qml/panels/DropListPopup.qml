pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Popup {
    id: popup

    parent: Overlay.overlay
    popupType: Popup.Item
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    modal: !scrubbing
    dim: false
    focus: true
    padding: 0
    background: null
    enter: null
    exit: null
    closePolicy: Popup.CloseOnEscape

    signal findAnchor(string key, point location)
    signal completed(string token, int selection)
    property point anchorPoint
    property bool anchorFound: false
    property var request: null
    property int selection: -1
    property bool scrubbing: false
    property bool pointerInColumn: true
    property real scrubOffset: 0
    property real controlWidth: 0
    property string typeface: ""
    property color panelTextColor: Theme.text
    property color panelBackground: Theme.surface

    function rootPane(): DropListMenu {
        return panes.itemAt(0) as DropListMenu
    }

    function present(next: var) {
        if (request && request.token === next.token)
            return
        if (request) {
            request = null
            close()
        }
        if (!next.token || next.panelList)
            return

        anchorFound = false
        findAnchor(next.key, next.position)
        if (!anchorFound) {
            completed(next.token, -1)
            return
        }

        showRequest(next)
    }

    function presentPanelList(next: var, appearance: var, origin: point, span: real, held: bool) {
        if (!next.token)
            return
        if (request) {
            request = null
            close()
        }
        scrubbing = held
        pointerInColumn = true
        anchorPoint = origin
        controlWidth = span
        typeface = appearance.font.family
        panelTextColor = appearance.color
        panelBackground = appearance.back
        showRequest(next)
    }

    function showRequest(next: var) {
        selection = -1
        scrubOffset = 0
        request = next
        paneModel.append({ menuId: 0, sourceRow: -1, leftEdge: anchorPoint.x,
                           rightEdge: anchorPoint.x, anchorY: anchorPoint.y })
        open()
    }

    function trackPointer(token: string, position: point) {
        const pane = rootPane()
        if (request?.token !== token || !scrubbing || !pane)
            return
        const x = position.x - pane.x
        const y = position.y - pane.y
        // Vertical excursions keep scrolling. Moving sideways leaves the list.
        pointerInColumn = x >= -20 && x <= pane.width + 20
        if (pointerInColumn)
            pane.highlightPointerRow(y)
        autoScroll.direction = pointerInColumn ? pane.pointerScrollDirection(y) : 0
    }

    function finishPointerSelection(token: string, accept: bool) {
        if (request?.token !== token)
            return
        const pane = rootPane()
        if (accept && scrubbing && pointerInColumn && pane) {
            pane.chooseHighlighted()
            return
        }
        close()
    }

    function closeAfter(depth: int) {
        const remaining = paneModel.count - depth - 1
        if (remaining > 0)
            paneModel.remove(depth + 1, remaining)
    }

    function openBranch(depth: int, row: int, top: real, takeFocus: bool) {
        const pane = panes.itemAt(depth) as DropListMenu
        if (paneModel.count > depth + 1 && paneModel.get(depth + 1).sourceRow === row) {
            if (takeFocus)
                (panes.itemAt(depth + 1) as DropListMenu)?.focusMenu()
            return
        }

        closeAfter(depth)
        const menuId = pane.entries[row].submenu
        if (menuId < 0 || !request.menus[menuId].length)
            return

        paneModel.append({ menuId: menuId, sourceRow: row, leftEdge: pane.x + 4,
                           rightEdge: pane.x + pane.width - 4, anchorY: top })
        if (takeFocus)
            (panes.itemAt(depth + 1) as DropListMenu)?.focusMenu()
    }

    onOpened: {
        const pane = rootPane()
        pane?.focusMenu()
        if (scrubbing && pane)
            scrubOffset = pane.centerCheckedRow()
    }

    onClosed: {
        autoScroll.direction = 0
        scrubbing = false
        paneModel.clear()
        if (request) {
            const token = request.token
            const result = selection
            request = null
            completed(token, result)
        }
    }

    Timer {
        id: autoScroll
        property int direction: 0
        running: direction !== 0
        repeat: true
        interval: 120
        onTriggered: popup.rootPane()?.scrollSelection(direction)
    }

    ListModel { id: paneModel }

    contentItem: Item {
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: popup.close()
        }

        Repeater {
            id: panes
            model: paneModel

            delegate: DropListMenu {
                id: pane

                required property int index
                required property int menuId
                required property real leftEdge
                required property real rightEdge
                required property real anchorY

                entries: popup.request?.menus[menuId] ?? []
                rowsPerColumn: popup.request?.rowsPerColumn ?? 0
                scrolling: popup.request?.scroll ?? false
                checkedIndex: popup.request?.checkedIndex ?? -1
                disabledIndex: popup.request?.disabledIndex ?? -1
                openRow: paneModel.count > index + 1 ? paneModel.get(index + 1).sourceRow : -1
                scrubbing: popup.scrubbing
                preferredWidth: popup.request?.panelList ? popup.controlWidth : 0
                typeface: popup.request?.panelList ? popup.typeface : ""
                textColor: popup.request?.panelList ? popup.panelTextColor : Theme.text
                backgroundColor: popup.request?.panelList ? popup.panelBackground : Theme.surface
                highlightColor: popup.request?.panelList
                    ? Qt.tint(backgroundColor, Qt.rgba(textColor.r, textColor.g, textColor.b, 0.2)) : Theme.hover
                accessibleName: popup.request?.panelList ? "Popup List options" : "DropList options"

                z: index
                x: {
                    const fitsRight = rightEdge + width <= popup.width - margin
                    const position = index === 0 || fitsRight ? rightEdge : leftEdge - width
                    return Math.max(margin, Math.min(position, popup.width - width - margin))
                }
                y: Math.max(margin, Math.min(anchorY - (index === 0 ? popup.scrubOffset : 0),
                                            popup.height - height - margin))

                onBranchRequested: (row, top, takeFocus) => popup.openBranch(index, row, top, takeFocus)
                onSelectionRequested: selected => {
                    popup.selection = selected
                    popup.close()
                }
                onBackRequested: {
                    if (index > 0) {
                        const parentPane = panes.itemAt(index - 1) as DropListMenu
                        popup.closeAfter(index - 1)
                        parentPane?.focusMenu()
                    }
                }
                onEntered: {
                    for (let depth = 0; depth < index; ++depth)
                        (panes.itemAt(depth) as DropListMenu)?.stopHover()
                }
                onDismissChildren: popup.closeAfter(index)
            }
        }
    }
}
