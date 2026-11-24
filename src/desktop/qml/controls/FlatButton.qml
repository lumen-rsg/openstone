import QtQuick
import QtQuick.Controls

ToolButton {
    id: button

    property bool filled: false
    property bool warm: false
    hoverEnabled: true
    topPadding: 4
    bottomPadding: 4
    leftPadding: 10
    rightPadding: 10
    font.pixelSize: 13
    font.weight: Font.DemiBold
    palette.buttonText: !enabled ? Theme.mutedText : warm ? Theme.amberText : Theme.text

    background: Rectangle {
        radius: Theme.controlRadius
        color: button.down ? Theme.selected
                          : button.warm && button.hovered ? Theme.amberHover
                          : button.hovered ? Theme.hover
                          : button.warm ? Theme.amberSurface
                          : button.filled ? Theme.surface : "transparent"
        border.width: button.visualFocus || (button.filled && button.enabled) ? 1 : 0
        border.color: button.visualFocus ? Theme.focus : Theme.outline
    }
}
