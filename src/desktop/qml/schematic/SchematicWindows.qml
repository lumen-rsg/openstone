pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: manager

    required property SchematicModel graph
    required property Window hostWindow
    readonly property var live: new Map()

    function synchronizeWindows() {
        const current = new Set()
        const focus = graph.focusedPanel
        const frames = focus.key ? graph.windowFrames.concat([focus]) : graph.windowFrames
        for (const frame of frames) {
            current.add(frame.key)
            const window = live.get(frame.key)
            if (window) {
                window.frame = frame
                if (window.visible && !frame.focused)
                    graph.reportWindowVisible(frame.path, frame.node, true)
                else if (!window.visible)
                    window.visible = true
            } else {
                const created = nativeWindow.createObject(manager, { frame: frame })
                if (created)
                    live.set(frame.key, created)
            }
        }

        const request = graph.dropList
        for (const [key, window] of live) {
            if (current.has(key)) {
                window.presentDropList(request.windowKey === key ? request : {})
            } else {
                live.delete(key)
                window.visible = false
                window.destroy()
            }
        }
        if (request.windowKey && !live.has(request.windowKey))
            graph.finishDropList(request.token, -1)
    }

    Connections {
        target: manager.graph
        function onValuesChanged() { Qt.callLater(manager.synchronizeWindows) }
    }

    Component.onCompleted: synchronizeWindows()

    Component {
        id: nativeWindow

        Window {
            id: surface
            required property var frame
            readonly property bool focused: frame.focused === true
            readonly property bool fullScreen: visibility === Window.FullScreen
            readonly property bool fixed: (focused && !fullScreen) || frame.type === 0
            readonly property bool appWindow: focused || frame.type === 2
            readonly property real panelWidth: Math.max(1, (frame.panel.bounds?.width ?? 0) * frame.pixelsPerUnit)
            readonly property real panelHeight: Math.max(1, (frame.panel.bounds?.height ?? 0) * frame.pixelsPerUnit)
            readonly property real requestedScale: frame.zoomScale === 0 ? Infinity : (frame.zoomScale ?? 1)
            readonly property real windowedScale: Math.min(requestedScale,
                Math.max(1, Math.min(Screen.desktopAvailableWidth, frame.maximumExtent)) / panelWidth,
                Math.max(1, Math.min(Screen.desktopAvailableHeight, frame.maximumExtent)) / panelHeight)
            readonly property int targetWidth: focused ? Math.ceil(panelWidth * windowedScale) : frame.width
            readonly property int targetHeight: focused ? Math.ceil(panelHeight * windowedScale) : frame.height
            readonly property real actualScale: focused
                ? Math.min(requestedScale, width / panelWidth, height / panelHeight) : 1
            property double appliedFullScreenToggles: 0
            property size appliedSize
            property bool ready: false

            title: frame.title
            color: frame.backgroundColor ?? Theme.canvas
            transientParent: appWindow ? null : manager.hostWindow
            flags: appWindow ? Qt.Window | Qt.WindowMinMaxButtonsHint | Qt.WindowCloseButtonHint
                             : fixed ? Qt.Dialog | Qt.MSWindowsFixedSizeDialogHint | Qt.WindowCloseButtonHint
                                     : Qt.Dialog | Qt.WindowMaximizeButtonHint | Qt.WindowCloseButtonHint
            minimumWidth: fixed ? targetWidth : 1
            minimumHeight: fixed ? targetHeight : 1
            maximumWidth: fixed ? targetWidth : frame.maximumExtent
            maximumHeight: fixed ? targetHeight : frame.maximumExtent

            function applySize() {
                if ((focused && fullScreen) ||
                    (targetWidth === appliedSize.width && targetHeight === appliedSize.height))
                    return
                appliedSize = Qt.size(targetWidth, targetHeight)
                width = targetWidth
                height = targetHeight
            }

            function applyFullScreenToggles() {
                const next = frame.fullScreenToggles ?? 0
                const toggles = next - appliedFullScreenToggles
                appliedFullScreenToggles = next
                if (toggles > 0 && toggles % 2 === 1) {
                    if (fullScreen)
                        showNormal()
                    else
                        showFullScreen()
                }
            }

            function presentDropList(request: var) { dropList.present(request) }

            onFrameChanged: if (ready && focused) applyFullScreenToggles()
            onFullScreenChanged: {
                if (ready && focused) {
                    applySize()
                    sizeUpdate.restart()
                }
            }
            onActualScaleChanged: if (ready && focused) sizeUpdate.restart()
            onTargetWidthChanged: if (ready) applySize()
            onTargetHeightChanged: if (ready) applySize()
            onVisibleChanged: {
                if (visible && !focused)
                    manager.graph.reportWindowVisible(frame.path, frame.node, true)
            }
            onClosing: close => {
                if (focused) {
                    close.accepted = false
                    manager.graph.closeFocusMode()
                } else {
                    manager.graph.reportWindowVisible(frame.path, frame.node, false)
                }
            }
            onWidthChanged: sizeUpdate.restart()
            onHeightChanged: sizeUpdate.restart()

            Shortcut {
                sequence: "Escape"
                enabled: surface.focused && surface.visible
                onActivated: manager.graph.closeFocusMode()
            }

            Component.onCompleted: {
                applySize()
                ready = true
                manager.graph.registerTickWindow(surface)
                visible = true
                if (focused) {
                    applyFullScreenToggles()
                    sizeUpdate.restart()
                }
            }

            Timer {
                id: sizeUpdate
                interval: 0
                onTriggered: {
                    if (!surface.visible)
                        return
                    if (surface.focused)
                        manager.graph.reportFocusWindow(surface.frame.path, surface.fullScreen, surface.actualScale)
                    else
                        manager.graph.reportWindowSize(surface.frame.path, surface.frame.node, surface.width, surface.height)
                }
            }

            FrontPanel {
                anchors.centerIn: parent
                width: surface.focused ? surface.panelWidth * surface.actualScale : surface.width
                height: surface.focused ? surface.panelHeight * surface.actualScale : surface.height
                panel: surface.frame.panel
                graph: manager.graph
                color: surface.color
                pixelsPerUnit: surface.frame.pixelsPerUnit * surface.actualScale
                schematicChrome: false
                dropListHost: dropList
            }

            DropListPopup {
                id: dropList
                onCompleted: (token, selection) => manager.graph.finishDropList(token, selection)
            }
        }
    }
}
