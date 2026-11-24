pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: editor
    required property string source
    required property string status
    required property bool running
    required property bool failed
    required property bool manualPorts
    property bool loaded: false
    property string runError

    signal draftChanged(string text)
    signal runRequested(bool portsOnly)
    signal stopRequested()

    function run() {
        runError = ""
        runRequested(false)
    }

    DspSyntax {
        id: syntax
        textDocument: code.textDocument
        ruby: true
    }

    Component.onCompleted: {
        code.text = source
        loaded = true
    }

    onSourceChanged: {
        if (loaded && code.text !== source)
            code.text = source
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 5

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            background: Rectangle {
                radius: Theme.controlRadius
                color: Theme.field
                border.color: code.activeFocus ? Theme.focus : Theme.divider
            }

            TextArea {
                id: code
                objectName: "rubySource"
                padding: 9
                wrapMode: TextEdit.NoWrap
                textFormat: TextEdit.PlainText
                font.family: syntax.fontFamily
                font.pixelSize: 13
                color: Theme.text
                selectByMouse: true
                background: null
                Accessible.name: "Ruby source code"

                onTextChanged: {
                    if (editor.loaded) {
                        editor.runError = ""
                        editor.draftChanged(text)
                    }
                }
                Keys.onPressed: event => {
                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) &&
                        (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier))) {
                        editor.run()
                        event.accepted = true
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            FlatButton {
                objectName: "runRuby"
                text: "Run Ruby"
                filled: true
                onClicked: editor.run()
            }

            FlatButton {
                objectName: "rubyApplyPorts"
                text: editor.manualPorts ? "Use Header Ports" : "Apply Ports"
                onClicked: {
                    editor.runError = ""
                    editor.runRequested(true)
                }
            }

            FlatButton {
                text: "Stop"
                enabled: editor.running
                onClicked: {
                    editor.runError = ""
                    editor.stopRequested()
                }
            }

            Label {
                Layout.fillWidth: true
                text: editor.running ? "Running" : "Stopped"
                color: Theme.secondaryText
            }
        }

        ScrollView {
            visible: result.text.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: 54
            clip: true

            TextArea {
                id: result
                objectName: "rubyResult"
                text: editor.runError || editor.status
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                textFormat: TextEdit.PlainText
                background: null
                color: editor.runError.length || editor.failed ? Theme.amber : Theme.secondaryText
                font.family: syntax.fontFamily
                font.pixelSize: 12
                Accessible.name: "Ruby result"
            }
        }
    }
}
