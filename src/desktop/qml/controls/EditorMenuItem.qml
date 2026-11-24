import QtQuick
import QtQuick.Controls

MenuItem {
    id: item

    property string shortcutText: ""

    readonly property real shortcutSpace: shortcutText.length ? shortcutLabel.implicitWidth + 24 : 0
    horizontalPadding: 10
    leftPadding: horizontalPadding + (mirrored ? shortcutSpace : 0)
    rightPadding: horizontalPadding + (mirrored ? 0 : shortcutSpace)
    verticalPadding: 5
    font.pixelSize: 13
    hoverEnabled: true
    palette.windowText: item.enabled ? Theme.text : Theme.mutedText
    Accessible.description: shortcutText

    Label {
        id: shortcutLabel
        visible: item.shortcutText.length > 0
        x: item.mirrored ? item.horizontalPadding : item.width - item.horizontalPadding - width
        anchors.verticalCenter: parent.verticalCenter
        text: item.shortcutText
        font: item.font
        color: item.enabled ? Theme.secondaryText : Theme.mutedText
        Accessible.ignored: true
    }

    background: Rectangle {
        implicitHeight: 30
        radius: Theme.controlRadius
        color: item.down ? Theme.selected : item.highlighted ? Theme.hover : "transparent"
        border.width: Qt.styleHints.accessibility.contrastPreference === Qt.HighContrast && item.highlighted ? 1 : 0
        border.color: Theme.focus
    }
}
