pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: effect

    required property Item contentItem
    required property var options
    required property size textureSize

    readonly property bool hardware: GraphicsInfo.api !== GraphicsInfo.Software && GraphicsInfo.api !== GraphicsInfo.Unknown
    readonly property bool custom: options.shader === true

    ShaderEffectSource {
        id: original
        anchors.fill: parent
        sourceItem: processing.active ? effect.contentItem : null
        hideSource: processing.active && (!effect.custom ||
                     (processing.item as ShaderEffect)?.status === ShaderEffect.Compiled)
        visible: false
        textureSize: effect.textureSize
        smooth: true
    }

    Loader {
        id: processing
        anchors.fill: parent
        active: effect.hardware && (!effect.custom || String(effect.options.shaderUrl ?? "") !== "")
        sourceComponent: effect.custom ? userShader : builtIn
    }

    Component {
        id: userShader
        ShaderEffect {
            property Item source: original
            property point resolution: Qt.point(effect.textureSize.width, effect.textureSize.height)
            property real time: effect.options.time ?? 0
            property real amount: effect.options.amount ?? 1
            fragmentShader: effect.options.shaderUrl
            visible: status !== ShaderEffect.Error

            onStatusChanged: {
                if (status === ShaderEffect.Error)
                    console.warn("View Shader could not load", fragmentShader, log)
            }
        }
    }

    Component {
        id: builtIn
        Item {
            readonly property var settings: effect.options
            readonly property real threshold: settings.threshold ?? -1
            readonly property point taps: settings.taps ?? Qt.point(0, 0)
            readonly property point sigma: settings.sigma ?? Qt.point(1, 1)

            GaussianPass {
                id: horizontal
                anchors.fill: parent
                enabledPass: parent.taps.x > 0 || parent.threshold >= 0
                input: original
                stepUV: parent.settings.stepX ?? Qt.point(0, 0)
                radius: parent.taps.x
                sigma: parent.sigma.x
                threshold: parent.threshold
            }

            GaussianPass {
                id: vertical
                anchors.fill: parent
                enabledPass: parent.taps.y > 0
                input: horizontal.output
                stepUV: parent.settings.stepY ?? Qt.point(0, 0)
                radius: parent.taps.y
                sigma: parent.sigma.y
                threshold: -1
            }

            ShaderEffect {
                anchors.fill: parent
                property Item source: original
                property Item filtered: vertical.output
                property int mode: parent.settings.mode ?? 0
                property int colorMix: parent.settings.mix ?? 0
                property int onlyEffect: parent.settings.onlyEffect ? 1 : 0
                property point offsetUV: parent.settings.offset ?? Qt.point(0, 0)
                property vector4d channels: parent.settings.channels ?? Qt.vector4d(1, 1, 1, 1)
                property vector4d tint: parent.settings.tint ?? Qt.vector4d(0, 0, 0, 0)
                property real strength: parent.settings.strength ?? 1
                fragmentShader: "qrc:/openstone/shaders/view-composite.frag.qsb"
            }
        }
    }

    component GaussianPass: Item {
        id: pass
        required property bool enabledPass
        required property Item input
        required property point stepUV
        required property real radius
        required property real sigma
        required property real threshold
        readonly property Item output: enabledPass ? buffer : input

        ShaderEffect {
            id: filter
            anchors.fill: parent
            visible: pass.enabledPass
            property Item source: pass.input
            property point stepUV: pass.stepUV
            property real radius: pass.radius
            property real sigma: pass.sigma
            property real threshold: pass.threshold
            fragmentShader: "qrc:/openstone/shaders/view-gaussian.frag.qsb"
        }

        ShaderEffectSource {
            id: buffer
            anchors.fill: parent
            sourceItem: pass.enabledPass ? filter : null
            hideSource: true
            visible: false
            textureSize: effect.textureSize
            smooth: true
        }
    }
}
