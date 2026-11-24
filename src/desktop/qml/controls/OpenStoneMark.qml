import QtQuick
import QtQuick.Shapes

Shape {
    implicitWidth: 30
    implicitHeight: 30
    preferredRendererType: Shape.CurveRenderer
    Accessible.role: Accessible.Graphic
    Accessible.name: "OpenStone"

    ShapePath {
        fillColor: "transparent"
        strokeColor: "white"
        strokeWidth: 1.4
        capStyle: ShapePath.RoundCap
        PathSvg {
            path: "M 12.6 7.5 H 17.55 C 19.75 7.5 21.08 8.72 21.25 10.85 "
                + "M 12.3 22.6 H 17.95 C 20.05 22.6 21.15 21.38 21.3 19.35"
        }
    }

    ShapePath {
        fillColor: "white"
        strokeColor: "transparent"
        fillRule: ShapePath.OddEvenFill
        PathSvg {
            path: "M 4.6 2.65 H 10.2 Q 12.55 2.65 12.55 5 V 10.5 Q 12.55 12.85 10.2 12.85 "
                + "H 4.6 Q 2.25 12.85 2.25 10.5 V 5 Q 2.25 2.65 4.6 2.65 Z "
                + "M 3.47 8.6 C 4.17 6.6 5.02 5.65 5.98 5.65 C 7.1 5.65 7.65 6.74 8.13 7.9 "
                + "C 8.56 8.88 8.79 9.24 9.08 9.24 C 9.48 9.24 9.85 8.74 10.04 8.04 "
                + "Q 10.2 7.35 10.55 7.35 H 11.16 C 10.83 8.85 10.15 10.34 9.13 10.34 "
                + "C 8.05 10.34 7.55 9.18 7.05 8.03 C 6.64 7.09 6.34 6.79 5.98 6.79 "
                + "C 5.53 6.79 5.06 7.38 4.61 8.39 Q 4.5 8.6 4.2 8.6 Z"
        }
    }

    ShapePath {
        fillColor: "white"
        strokeColor: "transparent"
        fillRule: ShapePath.OddEvenFill
        PathSvg {
            path: "M 4.55 17.15 H 10.05 Q 12.35 17.15 12.35 19.45 V 24.95 Q 12.35 27.25 10.05 27.25 "
                + "H 4.55 Q 2.25 27.25 2.25 24.95 V 19.45 Q 2.25 17.15 4.55 17.15 Z "
                + "M 4.68 22.54 H 5.27 Q 5.75 22.54 5.75 23.02 V 24.36 Q 5.75 24.84 5.27 24.84 "
                + "H 4.68 Q 4.2 24.84 4.2 24.36 V 23.02 Q 4.2 22.54 4.68 22.54 Z "
                + "M 6.91 21.18 H 7.5 Q 7.98 21.18 7.98 21.66 V 24.36 Q 7.98 24.84 7.5 24.84 "
                + "H 6.91 Q 6.43 24.84 6.43 24.36 V 21.66 Q 6.43 21.18 6.91 21.18 Z "
                + "M 9.14 19.56 H 9.73 Q 10.21 19.56 10.21 20.04 V 24.36 Q 10.21 24.84 9.73 24.84 "
                + "H 9.14 Q 8.66 24.84 8.66 24.36 V 20.04 Q 8.66 19.56 9.14 19.56 Z"
        }
    }

    ShapePath {
        fillColor: "white"
        strokeColor: "transparent"
        fillRule: ShapePath.OddEvenFill
        PathSvg {
            path: "M 20.65 10.5 H 25.8 Q 27.75 10.5 27.75 12.45 V 17.65 Q 27.75 19.6 25.8 19.6 "
                + "H 20.65 Q 18.7 19.6 18.7 17.65 V 12.45 Q 18.7 10.5 20.65 10.5 Z "
                + "M 21.65 13.08 Q 21.65 12.65 22.04 12.88 L 25.49 14.84 Q 25.9 15.07 25.49 15.31 "
                + "L 22.04 17.29 Q 21.65 17.5 21.65 17.07 Z"
        }
    }

    ShapePath {
        fillColor: "white"
        strokeColor: "transparent"
        PathSvg {
            path: "M 14.12 7.5 A 1.26 1.26 0 1 0 11.6 7.5 A 1.26 1.26 0 1 0 14.12 7.5 Z "
                + "M 13.56 22.55 A 1.26 1.26 0 1 0 11.04 22.55 A 1.26 1.26 0 1 0 13.56 22.55 Z "
                + "M 20.26 15.05 A 1.26 1.26 0 1 0 17.74 15.05 A 1.26 1.26 0 1 0 20.26 15.05 Z"
        }
    }
}
