pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: browser

    required property SchematicModel graph
    property string category: ""
    property string connectorType: ""
    property bool compact: false
    property bool connectorsExpanded: true
    signal insertRequested(var entry)
    signal contextRequested(var entry, point position)
    signal dragMoved(var entry, point position)
    signal dropRequested(var entry, point position)
    signal dragFinished()

    padding: 10
    background: Rectangle {
        color: Theme.sidebar
        radius: Theme.panelRadius
        border.color: Theme.divider
    }

    function focusSearch() {
        search.forceActiveFocus()
        search.selectAll()
    }

    function emptyResultText() {
        if (!search.text.trim() && !connectorType) {
            if (category === graph.favoritesCategory) return "Star components to add them here"
            if (category === graph.recentlyUsedCategory) return "Place components to see them here"
        }
        return "No matching components"
    }

    function filterEntries() {
        graph.filterLibrary(search.text, category, connectorType)
        results.positionViewAtBeginning()
    }

    onCategoryChanged: filterEntries()
    onConnectorTypeChanged: filterEntries()

    Connections {
        target: browser.graph
        function onToolboxChanged() {
            if (browser.category && browser.category !== browser.graph.favoritesCategory &&
                    browser.category !== browser.graph.recentlyUsedCategory &&
                    !browser.graph.librarySections.some(section =>
                        section.groups.some(group => group.filter === browser.category))) {
                browser.category = ""
            }
            if (browser.connectorType && !browser.graph.libraryConnectors.some(port => port.type === browser.connectorType))
                browser.connectorType = ""
            browser.filterEntries()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 9

        TextField {
            id: search
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            leftPadding: 32
            rightPadding: clearSearch.visible ? 33 : 11
            placeholderText: "Search library"
            Accessible.name: placeholderText
            selectByMouse: true
            onTextChanged: browser.filterEntries()

            background: Rectangle {
                radius: Theme.controlRadius
                color: Theme.field
                border.color: search.activeFocus ? Theme.focus : Theme.outline
            }
            Item {
                width: 14
                height: 14
                anchors.left: parent.left
                anchors.leftMargin: 11
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    width: 9
                    height: width
                    x: 1
                    y: 1
                    radius: width / 2
                    color: "transparent"
                    border.color: Theme.mutedText
                    antialiasing: true
                }
                Rectangle {
                    x: 8
                    y: 8
                    width: 5
                    height: 1
                    transformOrigin: Item.Left
                    rotation: 45
                    color: Theme.mutedText
                    antialiasing: true
                }
            }
            FlatButton {
                id: clearSearch
                visible: search.text.length > 0
                width: 26
                height: 26
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                padding: 0
                text: "×"
                Accessible.name: "Clear search"
                onClicked: {
                    search.clear()
                    search.forceActiveFocus()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 9

            Rectangle {
                Layout.preferredWidth: 108
                Layout.fillHeight: true
                radius: Theme.controlRadius
                color: Theme.field
                border.color: Theme.divider

                ScrollView {
                    id: rail
                    anchors.fill: parent
                    anchors.margins: 4
                    clip: true
                    contentWidth: availableWidth
                    contentHeight: categoryList.implicitHeight
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    Column {
                        id: categoryList
                        width: rail.availableWidth
                        spacing: 2

                        CategoryButton { text: "All"; filter: "" }
                        CategoryButton { text: "Favorites"; filter: browser.graph.favoritesCategory }
                        CategoryButton { text: "Recently Used"; filter: browser.graph.recentlyUsedCategory }

                        Repeater {
                            model: browser.graph.librarySections
                            delegate: Column {
                                id: section
                                required property var modelData
                                width: categoryList.width
                                spacing: 2

                                Label {
                                    width: parent.width
                                    leftPadding: 6
                                    topPadding: 8
                                    bottomPadding: 3
                                    text: section.modelData.title.toUpperCase()
                                    color: Theme.mutedText
                                    elide: Text.ElideRight
                                    font.pixelSize: 10
                                    font.bold: true
                                }

                                Repeater {
                                    model: section.modelData.groups
                                    delegate: CategoryButton {
                                        required property var modelData
                                        text: modelData.label
                                        filter: modelData.filter
                                    }
                                }
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 6

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: connectorControls.implicitHeight + 10
                    radius: Theme.controlRadius
                    color: Theme.field
                    border.color: Theme.divider

                    ColumnLayout {
                        id: connectorControls
                        anchors.fill: parent
                        anchors.margins: 5
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            FlatButton {
                                Layout.preferredHeight: 24
                                leftPadding: 5
                                text: browser.connectorsExpanded ? "▾  Connectors" : "▸  Connectors"
                                font.pixelSize: 12
                                Accessible.name: browser.connectorsExpanded ? "Hide connector filters" : "Show connector filters"
                                onClicked: browser.connectorsExpanded = !browser.connectorsExpanded
                            }
                            Item { Layout.fillWidth: true }
                            FlatButton {
                                Layout.preferredHeight: 24
                                leftPadding: 7
                                rightPadding: 7
                                filled: browser.connectorType.length > 0
                                text: browser.connectorType || "Any"
                                font.pixelSize: 11
                                enabled: browser.connectorType.length > 0
                                Accessible.name: enabled ? "Clear " + browser.connectorType + " connector filter" : "Any connector"
                                ToolTip.visible: enabled && hovered
                                ToolTip.text: "Clear connector filter"
                                ToolTip.delay: 500
                                onClicked: browser.connectorType = ""
                            }
                        }

                        Item {
                            visible: browser.connectorsExpanded
                            Layout.fillWidth: true
                            implicitHeight: connectorGrid.implicitHeight

                            Flow {
                                id: connectorGrid
                                width: parent.width
                                spacing: 2
                                Repeater {
                                    model: browser.graph.libraryConnectors
                                    delegate: ToolButton {
                                        id: connectorButton
                                        required property var modelData
                                        readonly property bool selected: browser.connectorType === modelData.type
                                        width: 25
                                        height: 25
                                        padding: 0
                                        hoverEnabled: true
                                        checkable: true
                                        checked: selected
                                        Accessible.name: modelData.type
                                        Accessible.description: "Filter library by connector"
                                        ToolTip.visible: hovered || visualFocus
                                        ToolTip.text: modelData.type
                                        ToolTip.delay: 500
                                        onClicked: browser.connectorType = selected ? "" : modelData.type
                                        background: Rectangle {
                                            radius: 5
                                            color: connectorButton.selected ? Theme.selection
                                                : connectorButton.hovered ? Theme.hover : "transparent"
                                            border.width: connectorButton.selected || connectorButton.visualFocus ? 1 : 0
                                            border.color: Theme.focus
                                        }
                                        contentItem: Item {
                                            LibraryConnector {
                                                anchors.centerIn: parent
                                                width: 19
                                                height: width
                                                port: connectorButton.modelData
                                                highlighted: connectorButton.selected
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        Layout.leftMargin: 2
                        Layout.fillWidth: true
                        text: results.count + (results.count === 1 ? " component" : " components")
                        color: Theme.secondaryText
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    FlatButton {
                        Layout.preferredHeight: 25
                        leftPadding: 7
                        rightPadding: 7
                        text: browser.compact ? "Previews" : "Compact"
                        font.pixelSize: 11
                        Accessible.name: browser.compact ? "Show component previews" : "Use a compact list"
                        onClicked: {
                            browser.compact = !browser.compact
                            results.positionViewAtBeginning()
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ListView {
                        id: results
                        anchors.fill: parent
                        model: browser.graph.library
                        clip: true
                        spacing: 5
                        reuseItems: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        delegate: Item {
                            id: card
                            required property var entry
                            required property string category
                            required property bool favorite
                            width: Math.max(0, results.width - 7)
                            height: browser.compact ? 34 : 88

                            Accessible.role: Accessible.Button
                            Accessible.name: entry.title
                            Accessible.description: category
                            Accessible.onPressAction: browser.insertRequested(card.entry)
                            activeFocusOnTab: true
                            Keys.onReturnPressed: browser.insertRequested(card.entry)
                            Keys.onEnterPressed: browser.insertRequested(card.entry)
                            Keys.onSpacePressed: browser.insertRequested(card.entry)

                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.controlRadius
                                color: rowMouse.pressed ? Theme.selected : rowMouse.containsMouse || card.activeFocus ? Theme.hover : Theme.surface
                                border.color: card.activeFocus ? Theme.focus : Theme.divider
                            }

                            Loader {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 5
                                height: 50
                                active: !browser.compact
                                visible: active
                                sourceComponent: Rectangle {
                                    radius: 4
                                    color: Theme.field
                                    LibraryPreview {
                                        anchors.fill: parent
                                        entry: card.entry
                                    }
                                }
                            }

                            RowLayout {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.leftMargin: 32
                                anchors.rightMargin: 7
                                height: 33
                                spacing: 5

                                Label {
                                    id: title
                                    Layout.fillWidth: true
                                    text: card.entry.title
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Label {
                                    visible: text.length > 0
                                    text: card.entry.hotkey ?? ""
                                    color: Theme.mutedText
                                    font.pixelSize: 10
                                }
                                Label {
                                    visible: card.width >= 206
                                    Layout.maximumWidth: 77
                                    text: card.entry.toolboxIndex === undefined ? card.category : "Toolbox"
                                    horizontalAlignment: Text.AlignHCenter
                                    color: Theme.secondaryText
                                    elide: Text.ElideRight
                                    font.pixelSize: 10
                                    leftPadding: 5
                                    rightPadding: 5
                                    topPadding: 2
                                    bottomPadding: 2
                                    background: Rectangle {
                                        radius: 4
                                        color: Theme.field
                                        border.color: Theme.divider
                                    }
                                }
                            }

                            FlatButton {
                                id: star
                                z: 1
                                anchors.left: parent.left
                                anchors.leftMargin: 5
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 4
                                width: 23
                                height: 25
                                padding: 0
                                text: card.favorite ? "★" : "☆"
                                Accessible.name: card.favorite ? "Remove " + card.entry.title + " from Favorites"
                                    : "Add " + card.entry.title + " to Favorites"
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                ToolTip.text: card.favorite ? "Remove from Favorites" : "Add to Favorites"
                                onClicked: browser.graph.setLibraryFavorite(card.entry.kind, card.entry.toolboxIndex ?? -1, !card.favorite)
                                contentItem: Label {
                                    text: star.text
                                    color: card.favorite ? Theme.amber : star.hovered ? Theme.text : Theme.mutedText
                                    font.pixelSize: 15
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    Accessible.ignored: true
                                }
                            }

                            ToolTip.visible: title.truncated && rowMouse.containsMouse && !rowMouse.pressed
                            ToolTip.text: card.entry.title
                            ToolTip.delay: 500

                            MouseArea {
                                id: rowMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                preventStealing: true
                                acceptedButtons: card.entry.toolboxIndex === undefined
                                    ? Qt.LeftButton : Qt.LeftButton | Qt.RightButton
                                cursorShape: dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                property point origin
                                property bool dragging: false

                                onPressed: mouse => {
                                    if (mouse.button !== Qt.LeftButton)
                                        return
                                    origin = Qt.point(mouse.x, mouse.y)
                                    dragging = false
                                    card.forceActiveFocus()
                                }
                                onPositionChanged: mouse => {
                                    if ((mouse.buttons & Qt.LeftButton) &&
                                        (dragging || Math.hypot(mouse.x - origin.x, mouse.y - origin.y) > 6)) {
                                        dragging = true
                                        browser.dragMoved(card.entry, mapToItem(browser, mouse.x, mouse.y))
                                    }
                                }
                                onReleased: mouse => {
                                    if (mouse.button === Qt.RightButton) {
                                        browser.contextRequested(card.entry, mapToItem(browser, mouse.x, mouse.y))
                                    } else if (dragging) {
                                        browser.dropRequested(card.entry, mapToItem(browser, mouse.x, mouse.y))
                                        browser.dragFinished()
                                        dragging = false
                                    } else if (containsMouse) {
                                        browser.insertRequested(card.entry)
                                    }
                                }
                                onCanceled: {
                                    if (dragging) browser.dragFinished()
                                    dragging = false
                                }
                            }
                        }
                    }

                    Label {
                        visible: results.count === 0
                        anchors.top: parent.top
                        anchors.topMargin: 14
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: browser.emptyResultText()
                        color: Theme.mutedText
                        font.pixelSize: 12
                    }
                }
            }
        }
    }

    component CategoryButton: FlatButton {
        id: categoryButton
        required property string filter
        readonly property bool selected: filter === browser.category
        width: parent.width
        height: 26
        leftPadding: 6
        rightPadding: 5
        font.pixelSize: 11
        filled: selected
        onClicked: browser.category = filter
        contentItem: Label {
            text: categoryButton.text
            font: categoryButton.font
            color: categoryButton.selected ? Theme.text : Theme.secondaryText
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        ToolTip.visible: hovered && contentItem.truncated
        ToolTip.delay: 500
        ToolTip.text: text
    }
}
