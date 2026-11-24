pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

FocusScope {
    id: control

    required property var drawing
    required property SchematicModel graph
    required property real pixelsPerUnit
    readonly property string rubySession: drawing?.rubySession ?? ""
    readonly property int textPixelSize: drawing?.fontSize > 0
        ? Math.max(1, Math.round(drawing.fontSize * pixelsPerUnit)) : 14
    readonly property string controlKey: drawing?.key ?? ""
    readonly property string currentText: drawing?.text ?? ""
    readonly property bool multiline: drawing?.multiline === true && drawing?.password !== true
    readonly property bool focused: multiline ? multilineField.activeFocus : singleLineField.activeFocus

    property bool applyingText: false
    property string session: ""
    property string sessionKey: ""
    property var sessionPath
    property int sessionNode: 0

    clip: true

    function requestFocus() {
        if (visible && enabled) {
            if (multiline)
                multilineField.forceActiveFocus()
            else
                singleLineField.forceActiveFocus()
        }
    }

    function updateText() {
        applyingText = true
        if (multiline) {
            if (multilineField.text !== currentText)
                multilineField.text = currentText
            singleLineField.clear()
        } else {
            if (singleLineField.text !== currentText)
                singleLineField.text = currentText
            multilineField.clear()
        }
        applyingText = false
    }

    function finishSession() {
        if (session === "")
            return
        const token = session
        session = ""
        graph.finishEditControl(sessionPath, sessionNode, token)
    }

    function syncFocus() {
        if (session !== "" && (!focused || !visible || sessionKey !== controlKey))
            finishSession()
        if (session === "" && visible && focused && controlKey !== "") {
            sessionKey = controlKey
            sessionPath = drawing.path
            sessionNode = drawing.node
            session = graph.beginEditControl(sessionPath, sessionNode, rubySession)
        }
    }

    function publishText(text: string) {
        if (!applyingText && focused) {
            if (session === "")
                syncFocus()
            if (session !== "" && sessionKey === controlKey)
                graph.changeEditControl(sessionPath, sessionNode, session, text)
        }
    }

    function leaveEditor() {
        if (multiline)
            multilineField.focus = false
        else
            singleLineField.focus = false
        focus = false
    }

    onControlKeyChanged: {
        if (session !== "" && sessionKey !== controlKey)
            leaveEditor()
        Qt.callLater(control.updateText)
    }
    onCurrentTextChanged: Qt.callLater(control.updateText)
    onMultilineChanged: Qt.callLater(control.updateText)
    onFocusedChanged: Qt.callLater(control.syncFocus)
    onVisibleChanged: Qt.callLater(control.syncFocus)
    Component.onCompleted: {
        updateText()
        if (rubySession !== "")
            Qt.callLater(control.requestFocus)
    }
    Component.onDestruction: {
        if (session !== "" && graph)
            Qt.callLater(graph.finishEditControl, sessionPath, sessionNode, session)
    }

    Connections {
        target: control.graph
        function onEditControlStartRequested(key) {
            if (key === control.controlKey)
                Qt.callLater(control.requestFocus)
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.controlRadius
        color: (control.focused || control.rubySession !== "") ? control.drawing.back : "transparent"
        border.color: control.focused ? Theme.focus : Theme.outline
    }

    TextField {
        id: singleLineField
        anchors.fill: parent
        visible: !control.multiline
        enabled: visible
        padding: 5
        selectByMouse: true
        color: control.drawing?.color ?? Theme.text
        font.family: control.drawing?.font.family ?? ""
        font.pixelSize: control.textPixelSize
        font.bold: control.drawing?.font.bold ?? false
        font.italic: control.drawing?.font.italic ?? false
        font.underline: control.drawing?.font.underline ?? false
        font.strikeout: control.drawing?.font.strikeout ?? false
        echoMode: control.drawing?.password ? TextInput.Password : TextInput.Normal
        background: null
        Accessible.name: "Edit Control"

        onAccepted: control.leaveEditor()
        onTextEdited: control.publishText(text)
        Keys.onTabPressed: control.leaveEditor()
        Keys.onBacktabPressed: control.leaveEditor()
        Keys.onEscapePressed: control.leaveEditor()
    }

    ScrollView {
        anchors.fill: parent
        visible: control.multiline
        enabled: visible
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        TextArea {
            id: multilineField
            padding: 5
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            textFormat: TextEdit.PlainText
            color: control.drawing?.color ?? Theme.text
            font.family: control.drawing?.font.family ?? ""
            font.pixelSize: control.textPixelSize
            font.bold: control.drawing?.font.bold ?? false
            font.italic: control.drawing?.font.italic ?? false
            font.underline: control.drawing?.font.underline ?? false
            font.strikeout: control.drawing?.font.strikeout ?? false
            background: null
            Accessible.name: "Edit Control"

            onTextChanged: {
                if (activeFocus)
                    control.publishText(text)
            }
            Keys.onTabPressed: control.leaveEditor()
            Keys.onBacktabPressed: control.leaveEditor()
            Keys.onEscapePressed: control.leaveEditor()
        }
    }
}
