import QtQuick
import QtQuick.Controls

Menu {
    id: menu

    popupType: Popup.Item
    margins: 6
    padding: 5
    overlap: 3
    implicitWidth: {
        let width = implicitBackgroundWidth
        for (let i = 0; i < count; ++i) {
            const item = itemAt(i)
            if (item)
                width = Math.max(width, item.implicitWidth + leftPadding + rightPadding)
        }
        return width
    }

    palette.mid: Theme.divider

    delegate: EditorMenuItem {}

    background: Rectangle {
        implicitWidth: 166
        radius: Theme.panelRadius
        color: Theme.surface
        border.color: Theme.outline
        antialiasing: true
    }
}
