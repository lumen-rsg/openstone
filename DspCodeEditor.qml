pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: editor

    required property string source
    required property string appliedSource
    property bool assembler: false
    property var diagnostic: ({})
    property int completionStart: 0
    property int completionEnd: 0
    property bool loaded: false
    property bool insertingCompletion: false
    readonly property bool dirty: code.text !== appliedSource

    signal draftChanged(string text)
    signal compileRequested(bool moveToError)
    signal captureRequested(int frames, int blockSize)

    function compile(moveToError: bool) {
        completionTimer.stop()
        suggestions.close()
        if (loaded && dirty && (moveToError || !diagnostic.message))
            compileRequested(moveToError)
    }

    function showDiagnostic(result: var, moveToError: bool) {
        diagnostic = result
        if (moveToError && result.line && result.column) {
            const lines = code.text.split("\n")
            const before = lines.slice(0, result.line - 1).join("\n")
            const lineStart = result.line > 1 ? before.length + 1 : 0
            const lineLength = lines[result.line - 1]?.length ?? 0
            code.forceActiveFocus()
            code.cursorPosition = lineStart + Math.min(lineLength, result.column - 1)
        }
    }

    function showCapture(result: var) {
        capture.errorMessage = result.message ?? ""
        capture.backend = result.backend ?? ""
        capture.resultText = result.samples ?? ""
    }

    function showCompletions(explicit: bool) {
        if (!code.activeFocus || code.selectedText.length || code.inputMethodComposing || insertingCompletion) {
            suggestions.close()
            return
        }
        const before = code.text.slice(0, code.cursorPosition)
        const line = before.slice(before.lastIndexOf("\n") + 1)
        const prefix = line.match(/[A-Za-z_]\w*$/)?.[0] ?? ""
        if (line.includes("//") || (!explicit && prefix.length < 2)) {
            suggestions.close()
            return
        }
        completionStart = code.cursorPosition - prefix.length
        completionEnd = code.cursorPosition + (code.text.slice(code.cursorPosition).match(/^\w*/)?.[0].length ?? 0)
        suggestionList.model = syntax.complete(code.text, prefix)
        if (!suggestionList.count) {
            suggestions.close()
            return
        }
        suggestionList.currentIndex = 0
        suggestions.open()
    }

    function acceptCompletion(index: int) {
        const name = suggestionList.model[index]
        if (name === undefined)
            return
        insertingCompletion = true
        suggestions.close()
        code.remove(completionStart, completionEnd)
        code.insert(completionStart, name)
        code.cursorPosition = completionStart + name.length
        code.forceActiveFocus()
        insertingCompletion = false
    }

    Component.onCompleted: {
        code.text = source
        loaded = true
    }
    onSourceChanged: {
        if (loaded && source !== code.text)
            code.text = source
    }

    Rectangle {
        anchors.fill: scroll
        radius: Theme.controlRadius
        color: Theme.field
        border.color: editor.diagnostic.message ? Theme.amber
                    : code.activeFocus ? Theme.focus : Theme.divider
    }

    ScrollView {
        id: scroll
        anchors.top: parent.top
        anchors.bottom: actions.top
        anchors.bottomMargin: 5
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded

        TextArea {
            id: code
            objectName: "dspSource"
            padding: 10
            wrapMode: TextEdit.NoWrap
            textFormat: TextEdit.PlainText
            font.family: syntax.fontFamily
            font.pixelSize: 13
            color: Theme.text
            selectByMouse: true
            background: null
            placeholderText: editor.assembler ? "Write assembly" : "Write DSP code"
            Accessible.name: editor.assembler ? "Assembler source code" : "DSP source code"

            onTextChanged: {
                if (editor.loaded) {
                    editor.diagnostic = ({})
                    editor.draftChanged(text)
                }
                if (!editor.insertingCompletion && activeFocus)
                    completionTimer.restart()
            }
            onCursorPositionChanged: suggestions.close()
            onActiveFocusChanged: {
                if (!activeFocus && !editor.insertingCompletion)
                    editor.compile(false)
            }
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Space && (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier))) {
                    completionTimer.stop()
                    editor.showCompletions(true)
                    event.accepted = true
                } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) &&
                           (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier))) {
                    editor.compile(true)
                    event.accepted = true
                } else if (suggestions.visible) {
                    switch (event.key) {
                    case Qt.Key_Down:
                        suggestionList.currentIndex = (suggestionList.currentIndex + 1) % suggestionList.count
                        break
                    case Qt.Key_Up:
                        suggestionList.currentIndex = (suggestionList.currentIndex + suggestionList.count - 1)
                                                    % suggestionList.count
                        break
                    case Qt.Key_Escape: suggestions.close(); break
                    case Qt.Key_Tab:
                    case Qt.Key_Return:
                    case Qt.Key_Enter:
                        editor.acceptCompletion(suggestionList.currentIndex)
                        break
                    default: return
                    }
                    event.accepted = true
                } else if (event.key === Qt.Key_Tab && event.modifiers === Qt.NoModifier) {
                    code.remove(code.selectionStart, code.selectionEnd)
                    code.insert(code.cursorPosition, "    ")
                    event.accepted = true
                }
            }
        }
    }

    DspSyntax {
        id: syntax
        assembler: editor.assembler
        textDocument: code.textDocument
    }

    Timer {
        id: completionTimer
        interval: 180
        onTriggered: editor.showCompletions(false)
    }

    Popup {
        id: suggestions
        parent: editor
        width: Math.min(195, editor.width)
        height: 8 + Math.min(7, suggestionList.count) * 26
        padding: 4
        margins: 8
        x: Math.max(0, Math.min(editor.width - width, code.mapToItem(editor, code.cursorRectangle.x, 0).x))
        y: code.mapToItem(editor, 0, code.cursorRectangle.y + code.cursorRectangle.height + 3).y
        focus: false
        closePolicy: Popup.CloseOnPressOutside
        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.chrome
            border.color: Theme.outline
        }
        contentItem: ListView {
            id: suggestionList
            clip: true
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
            delegate: ItemDelegate {
                id: suggestion
                required property string modelData
                required property int index
                width: suggestionList.width
                height: 26
                text: modelData
                highlighted: ListView.isCurrentItem
                font.family: syntax.fontFamily
                font.pixelSize: 12
                focusPolicy: Qt.NoFocus
                onClicked: editor.acceptCompletion(index)
            }
        }
    }

    RowLayout {
        id: actions
        anchors.bottom: parent.bottom
        width: parent.width
        spacing: 4

        Label {
            Layout.fillWidth: true
            text: editor.diagnostic.message || (editor.dirty ? "Changes to compile" : "Ready")
            color: editor.diagnostic.message ? Theme.amberText : Theme.secondaryText
            font.pixelSize: 11
            elide: Text.ElideRight
            ToolTip.visible: errorHover.hovered && !!editor.diagnostic.message
            ToolTip.text: editor.diagnostic.message ?? ""
            HoverHandler { id: errorHover }
        }
        FlatButton {
            text: "Compile"
            objectName: "dspCompile"
            focusPolicy: Qt.TabFocus
            onClicked: editor.compile(true)
        }
        FlatButton {
            text: "Capture…"
            objectName: "dspCapture"
            enabled: !editor.diagnostic.message
            onClicked: {
                editor.compile(false)
                if (editor.dirty)
                    return
                editor.showCapture({})
                capture.open()
                editor.captureRequested(frames.value, Math.min(frames.value, blockSize.value))
            }
        }
    }

    Dialog {
        id: capture
        objectName: "dspCaptureDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "DSP sample capture"
        standardButtons: Dialog.Close
        width: Math.min(660, parent ? parent.width - 32 : 660)
        height: Math.min(460, parent ? parent.height - 32 : 460)
        property string resultText: ""
        property string errorMessage: ""
        property string backend: ""

        background: Rectangle {
            color: Theme.chrome
            radius: Theme.panelRadius
            border.color: Theme.outline
        }

        contentItem: ColumnLayout {
            spacing: 8

            RowLayout {
                Label { text: "Samples" }
                SpinBox { id: frames; from: 1; to: 16384; value: 32; editable: true; Accessible.name: "Samples" }
                Label { text: "Block size" }
                SpinBox {
                    id: blockSize
                    from: 1
                    to: frames.value
                    value: 16
                    editable: true
                    Accessible.name: "Block size"
                }
                FlatButton {
                    text: "Run"
                    filled: true
                    onClicked: editor.captureRequested(frames.value, blockSize.value)
                }
            }
            Label {
                Layout.fillWidth: true
                text: capture.errorMessage || capture.backend
                color: capture.errorMessage ? Theme.amberText : Theme.secondaryText
                wrapMode: Text.Wrap
                visible: text.length > 0
            }
            ScrollView {
                Layout.fillHeight: true
                Layout.fillWidth: true
                clip: true
                TextArea {
                    id: results
                    objectName: "dspCaptureSamples"
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.NoWrap
                    text: capture.resultText
                    font.family: syntax.fontFamily
                    color: Theme.text
                    Accessible.name: "Captured DSP samples"
                }
            }
            FlatButton {
                text: "Copy samples"
                enabled: capture.resultText.length > 0
                onClicked: {
                    results.selectAll()
                    results.copy()
                    results.deselect()
                }
            }
        }
    }
}
