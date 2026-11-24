pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs

Item {
    id: dialogs

    required property SchematicModel graph
    required property Window fallbackWindow
    property Window dialogParent: fallbackWindow
    property string activeToken: ""
    property var activeDialog: null

    function present() {
        const request = graph.appDialog
        const token = request.token || ""
        if (activeToken === token) return
        if (activeDialog) {
            const previousDialog = activeDialog
            activeDialog = null
            activeToken = ""
            previousDialog.close()
        }
        if (!token) return

        dialogParent = graph.appDialogWindow() || fallbackWindow
        activeToken = token
        if (request.kind === "file") {
            activeDialog = file
            const isOpen = request.type === 1
            file.title = request.title || (isOpen ? qsTr("Open File") : qsTr("Save File"))
            file.fileMode = isOpen ? Dialogs.FileDialog.OpenFile : Dialogs.FileDialog.SaveFile
            file.nameFilters = request.filters
            file.defaultSuffix = request.suffix
            file.currentFolder = request.folder
            file.selectedFile = request.file
            file.open()
        } else if (request.kind === "folder") {
            activeDialog = folder
            folder.title = request.title || qsTr("Choose Folder")
            folder.currentFolder = request.folder
            folder.open()
        } else if (request.kind === "color") {
            activeDialog = colorDialog
            colorDialog.selectedColor = request.color
            colorDialog.open()
        } else {
            activeDialog = message
            message.title = request.title
            message.message = request.message
            message.messageIcon = request.icon
            message.buttons = request.buttons
            message.rubyMessage = request.rubyMessage === true
            message.defaultButton = request.defaultButton ?? -1
            message.open()
        }
    }

    function finish(sender: QtObject, accepted: bool, path: url) {
        if (sender !== activeDialog) return
        const token = activeToken
        activeDialog = null
        activeToken = ""
        graph.finishAppDialog(token, accepted, path)
        Qt.callLater(present)
    }

    function finishMessage(result: string) {
        if (activeDialog !== message) return
        const token = activeToken
        activeDialog = null
        activeToken = ""
        message.close()
        graph.finishRubyMessageBox(token, result)
        Qt.callLater(present)
    }

    function finishColor(accepted: bool) {
        if (activeDialog !== colorDialog) return
        const token = activeToken
        activeDialog = null
        activeToken = ""
        graph.finishColorDialog(token, accepted, colorDialog.selectedColor)
        Qt.callLater(present)
    }

    Connections {
        target: dialogs.graph
        function onValuesChanged() { Qt.callLater(dialogs.present) }
    }

    Component.onCompleted: {
        // Keep the Qt Quick fallback within the editor's window.
        for (const picker of [file, folder]) {
            if ("popupType" in picker) picker.popupType = Popup.Item
        }
        present()
    }

    Dialogs.FileDialog {
        id: file
        objectName: "primitiveFileDialog"
        parentWindow: dialogs.dialogParent
        modality: Qt.ApplicationModal
        onAccepted: dialogs.finish(file, true, selectedFile)
        onRejected: dialogs.finish(file, false, "")
    }

    Dialogs.FolderDialog {
        id: folder
        objectName: "primitiveFolderDialog"
        parentWindow: dialogs.dialogParent
        modality: Qt.ApplicationModal
        onAccepted: dialogs.finish(folder, true, selectedFolder)
        onRejected: dialogs.finish(folder, false, "")
    }

    Dialogs.ColorDialog {
        id: colorDialog
        objectName: "primitiveColorDialog"
        parentWindow: dialogs.dialogParent
        modality: Qt.ApplicationModal
        title: qsTr("Choose Color")
        onAccepted: dialogs.finishColor(true)
        onRejected: dialogs.finishColor(false)
    }

    Dialog {
        id: message
        objectName: "primitiveMessageBox"
        property string message: ""
        property int messageIcon: 0
        property var buttons: []
        property bool rubyMessage: false
        property int defaultButton: -1
        readonly property string escapeResult: buttons.indexOf("cancel") >= 0 ? "cancel"
            : buttons.indexOf("ok") >= 0 ? "ok" : ""

        function choose(result: string) {
            if (rubyMessage)
                dialogs.finishMessage(result)
            else if (result === "ok" || result === "yes")
                accept()
            else
                reject()
        }
        parent: dialogs.dialogParent.contentItem
        modal: true
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 430)
        padding: 20
        closePolicy: rubyMessage && escapeResult === "" ? Popup.NoAutoClose : Popup.CloseOnEscape

        function focusDefaultButton() {
            if (opened && defaultButton >= 0)
                buttonRepeater.itemAt(defaultButton)?.forceActiveFocus()
        }

        onOpened: Qt.callLater(message.focusDefaultButton)
        onAccepted: {
            if (rubyMessage)
                choose(buttons[defaultButton])
            else
                dialogs.finish(message, true, "")
        }
        onRejected: {
            if (rubyMessage && escapeResult !== "")
                dialogs.finishMessage(escapeResult)
            else if (!rubyMessage)
                dialogs.finish(message, false, "")
        }

        contentItem: ScrollView {
            implicitHeight: Math.min(body.implicitHeight, 280)
            contentWidth: availableWidth
            clip: true
            RowLayout {
                id: body
                width: parent.width
                spacing: 14
                Label {
                    Layout.alignment: Qt.AlignTop
                    visible: message.messageIcon > 0 && message.messageIcon < 5
                    text: ["", "ⓘ", "?", "⚠", "⛔"][message.messageIcon] || ""
                    Accessible.name: ["", qsTr("Information"), qsTr("Question"), qsTr("Warning"), qsTr("Stop")][message.messageIcon] || ""
                    font.pixelSize: 24
                }
                Label {
                    Layout.fillWidth: true
                    text: message.message
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
            }
        }

        footer: Item {
            implicitHeight: buttonRow.implicitHeight + 24

            RowLayout {
                id: buttonRow
                anchors.fill: parent
                anchors.margins: 12

                Item {
                    Layout.fillWidth: true
                    visible: message.buttons.length === 1
                }

                Repeater {
                    id: buttonRepeater
                    model: message.buttons

                    Button {
                        required property string modelData
                        readonly property var labels: ({
                            ok: qsTr("OK"), cancel: qsTr("Cancel"), yes: qsTr("Yes"), no: qsTr("No"),
                            abort: qsTr("Abort"), retry: qsTr("Retry"), ignore: qsTr("Ignore")
                        })

                        Layout.fillWidth: message.buttons.length > 1
                        text: labels[modelData]
                        onClicked: message.choose(modelData)
                        Keys.onReturnPressed: message.choose(modelData)
                        Keys.onEnterPressed: message.choose(modelData)
                    }
                }
            }
        }
    }
}
