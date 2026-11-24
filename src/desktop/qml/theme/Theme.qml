pragma Singleton

import QtQuick

QtObject {
    readonly property color chrome: "#262625"
    readonly property color sidebar: "#20201f"
    readonly property color canvas: "#191918"
    readonly property color field: "#1e1e1d"
    readonly property color surface: "#272726"
    readonly property color hover: "#30302f"
    readonly property color selected: "#373736"
    readonly property color divider: "#333332"
    readonly property color outline: "#434341"
    readonly property color grid: "#30302e"
    readonly property color text: "#eeeeeb"
    readonly property color secondaryText: "#b7b7b2"
    readonly property color mutedText: "#9a9a95"
    readonly property color focus: "#aaa9c5"
    readonly property color selection: "#56536a"

    readonly property color green: "#83c8a4"
    readonly property color greenText: "#b6e6cb"
    readonly property color greenSurface: "#25352d"
    readonly property color greenHover: "#344d40"
    readonly property color amber: "#dbaf83"
    readonly property color amberText: "#f2cca5"
    readonly property color amberSurface: "#3b3026"
    readonly property color amberHover: "#4d3d30"
    readonly property color yellow: "#e6ce65"
    readonly property color yellowText: "#f2e297"
    readonly property color yellowSurface: "#3c3620"
    readonly property color yellowHover: "#51482a"

    readonly property var connectorPalettes: ({
        memory: { rim: "#95958f", fill: "#292928", hover: "#383837", text: "#deded8" },
        stream: { rim: "#8f9299", fill: "#2e3034", hover: "#41444b", text: "#dddfe5" },
        poly: { rim: "#c5c4d2", fill: "#36343f", hover: "#484653", text: "#f0eef5" },
        mono: { rim: "#9999eb", fill: "#30304e", hover: "#41416b", text: "#d9d8ff" },
        midi: { rim: "#e39099", fill: "#452b31", hover: "#603741", text: "#f8c8cd" },
        ruby: { rim: "#df8977", fill: "#452d2a", hover: "#603b32", text: "#f8c1b5" },
        preset: { rim: green, fill: greenSurface, hover: greenHover, text: greenText }
    })

    readonly property int controlRadius: 7
    readonly property int nodeRadius: 9
    readonly property int panelRadius: 12
}
