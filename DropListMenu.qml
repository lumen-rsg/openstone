pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Rectangle {
    id: menu

    required property var entries
    required property int rowsPerColumn
    required property bool scrolling
    required property int checkedIndex
    property int disabledIndex: -1
    required property int openRow
    property bool scrubbing: false
    property real preferredWidth: 0
    property string typeface: ""
    property color textColor: Theme.text
    property color backgroundColor: Theme.surface
    property color highlightColor: Theme.hover
    property string accessibleName: "DropList options"

    signal branchRequested(int row, real top, bool takeFocus)
    signal selectionRequested(int optionIndex)
    signal backRequested()
    signal entered()
    signal dismissChildren()

    readonly property int margin: 8
    readonly property int padding: 4
    readonly property int rowHeight: 32
    readonly property int maxRows: Math.max(1, Math.floor((parent.height - 2 * (margin + padding)) / rowHeight))
    readonly property int rows: Math.min(maxRows, rowsPerColumn > 0 ? rowsPerColumn : maxRows, Math.max(1, entries.length))
    readonly property real cellWidth: {
        let widest = 112
        for (const entry of entries)
            widest = Math.max(widest, metrics.advanceWidth(entry.text))
        const measured = Math.max(preferredWidth - 2 * padding, Math.ceil(widest) + 52)
        return Math.max(1, Math.min(measured, parent.width - 2 * (margin + padding)))
    }
    readonly property int maxColumns: Math.max(1, Math.floor((parent.width - 2 * (margin + padding)) / cellWidth))
    readonly property int columns: scrolling ? 1 : Math.min(maxColumns, Math.ceil(entries.length / rows))

    width: columns * cellWidth + 2 * padding
    height: rows * rowHeight + 2 * padding
    color: backgroundColor
    border.color: Theme.outline
    radius: Theme.panelRadius

    function isSeparator(entry: var): bool {
        return entry.optionIndex < 0 && entry.submenu < 0
    }

    function isSelectable(entry: var): bool {
        return !!entry && !isSeparator(entry) && (entry.optionIndex < 0 || entry.optionIndex !== disabledIndex)
    }

    function focusMenu() {
        if (!isSelectable(entries[options.currentIndex])) {
            const checkedRow = entries.findIndex(entry => entry.optionIndex >= 0 && entry.optionIndex === checkedIndex &&
                                                            isSelectable(entry))
            options.currentIndex = checkedRow >= 0 ? checkedRow : entries.findIndex(isSelectable)
        }
        if (options.currentIndex >= 0)
            options.positionViewAtIndex(options.currentIndex, GridView.Contain)
        options.forceActiveFocus()
    }

    function stopHover() {
        branchDelay.stop()
    }

    function centerCheckedRow(): real {
        const index = options.currentIndex
        if (index < 0)
            return 0
        options.positionViewAtIndex(index, GridView.Center)
        return padding + options.originY + (index + 0.5) * rowHeight - options.contentY
    }

    function highlightPointerRow(y: real) {
        const offset = options.contentY - options.originY
        const visibleY = Math.max(0, Math.min(y - padding, options.height - 1))
        options.currentIndex = Math.min(entries.length - 1, Math.floor((offset + visibleY) / rowHeight))
    }

    function pointerScrollDirection(y: real): int {
        if (y < padding + 8 && options.contentY > options.originY)
            return -1
        if (y > height - padding - 8 && options.contentY + options.height < options.originY + options.contentHeight)
            return 1
        return 0
    }

    function scrollSelection(direction: int) {
        move(direction)
        if (options.currentIndex >= 0)
            options.positionViewAtIndex(options.currentIndex, GridView.Contain)
    }

    function chooseHighlighted() {
        activate(options.currentIndex)
    }

    function showBranch(row: int, takeFocus: bool) {
        const item = options.itemAtIndex(row)
        if (item)
            branchRequested(row, item.mapToItem(parent, 0, -padding).y, takeFocus)
    }

    function activate(row: int) {
        const entry = entries[row]
        if (!isSelectable(entry))
            return
        stopHover()
        options.currentIndex = row
        if (entry.submenu >= 0)
            showBranch(row, true)
        else
            selectionRequested(entry.optionIndex)
    }

    function move(step: int) {
        stopHover()
        dismissChildren()
        for (let row = options.currentIndex + step; row >= 0 && row < entries.length; row += step) {
            if (isSelectable(entries[row])) {
                options.currentIndex = row
                return
            }
        }
    }

    FontMetrics {
        id: metrics
        font.family: menu.typeface
    }

    Timer {
        id: branchDelay
        property int row: -1
        interval: 140
        onTriggered: menu.showBranch(row, false)
    }

    HoverHandler {
        onHoveredChanged: {
            if (hovered)
                menu.entered()
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }

    GridView {
        id: options

        anchors.fill: parent
        anchors.margins: menu.padding
        model: menu.entries
        flow: menu.scrolling ? GridView.FlowLeftToRight : GridView.FlowTopToBottom
        cellWidth: menu.cellWidth
        cellHeight: menu.rowHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        currentIndex: -1
        Accessible.name: menu.accessibleName
        ScrollBar.vertical: ScrollBar { policy: menu.scrolling ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }
        ScrollBar.horizontal: ScrollBar { policy: menu.scrolling ? ScrollBar.AlwaysOff : ScrollBar.AsNeeded }
        onMovementStarted: {
            menu.stopHover()
            menu.dismissChildren()
        }

        Keys.onUpPressed: menu.move(-1)
        Keys.onDownPressed: menu.move(1)
        Keys.onLeftPressed: {
            if (!menu.scrolling && currentIndex >= menu.rows)
                menu.move(-menu.rows)
            else
                menu.backRequested()
        }
        Keys.onRightPressed: {
            if ((menu.entries[currentIndex]?.submenu ?? -1) >= 0)
                menu.activate(currentIndex)
            else if (!menu.scrolling)
                menu.move(menu.rows)
        }
        Keys.onReturnPressed: menu.activate(currentIndex)
        Keys.onEnterPressed: menu.activate(currentIndex)
        Keys.onSpacePressed: menu.activate(currentIndex)

        delegate: Item {
            id: row
            required property int index
            required property var modelData
            readonly property bool separator: menu.isSeparator(modelData)

            width: options.cellWidth
            height: options.cellHeight

            Rectangle {
                visible: row.separator
                anchors.verticalCenter: parent.verticalCenter
                x: 8
                width: parent.width - 16
                height: 1
                color: Theme.outline
            }

            ItemDelegate {
                id: option
                readonly property bool isChecked: row.modelData.optionIndex >= 0 && row.modelData.optionIndex === menu.checkedIndex

                visible: !row.separator
                enabled: menu.isSelectable(row.modelData)
                anchors.fill: parent
                leftPadding: 8
                rightPadding: 8
                hoverEnabled: !menu.scrubbing
                highlighted: row.index === menu.openRow ||
                             ((menu.scrubbing || options.activeFocus) && row.index === options.currentIndex)
                Accessible.name: row.modelData.text
                Accessible.checkable: isChecked
                Accessible.checked: isChecked
                onClicked: menu.activate(row.index)
                onHoveredChanged: {
                    if (hovered) {
                        branchDelay.row = row.index
                        branchDelay.restart()
                    } else if (branchDelay.row === row.index) {
                        menu.stopHover()
                    }
                }

                contentItem: Row {
                    spacing: 5
                    Label {
                        width: 16
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: option.isChecked ? "✓" : ""
                        color: menu.typeface.length ? menu.textColor : Theme.greenText
                    }
                    Label {
                        width: parent.width - 37
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: row.modelData.text
                        elide: Text.ElideRight
                        font.family: menu.typeface || option.font.family
                        color: option.enabled ? menu.textColor : Theme.secondaryText
                    }
                    Label {
                        width: 11
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: row.modelData.submenu >= 0 ? "›" : ""
                        color: Theme.secondaryText
                    }
                }

                background: Rectangle {
                    color: option.hovered || option.highlighted ? menu.highlightColor : "transparent"
                    radius: Theme.controlRadius
                }
            }
        }
    }
}
