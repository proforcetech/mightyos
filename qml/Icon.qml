import QtQuick
import QtQuick.Shapes

// Vector icon drawn from an SVG path on a 24x24 grid. No image assets, crisp
// at any scale (CurveRenderer antialiases on the GPU without MSAA).
Item {
    id: icon

    property string name
    property color color: Theme.text

    implicitWidth: Theme.tileIconSize
    implicitHeight: Theme.tileIconSize

    readonly property var _data: _icons[name] ?? _icons["placeholder"]

    readonly property var _icons: ({
        "favorites": {
            d: "M12 2 L14.94 8.26 L21.8 9.09 L16.7 13.79 L18.05 20.56 L12 17.2 "
             + "L5.95 20.56 L7.3 13.79 L2.2 9.09 L9.06 8.26 Z",
            oddEven: false
        },
        "emulators": {   // gamepad with D-pad and two buttons cut out
            d: "M7 6 H17 A5 5 0 0 1 22 11 V14 A4 4 0 0 1 15 16.5 L14 15 H10 L9 16.5 "
             + "A4 4 0 0 1 2 14 V11 A5 5 0 0 1 7 6 Z "
             + "M6 8.5 H8 V10 H9.5 V12 H8 V13.5 H6 V12 H4.5 V10 H6 Z "
             + "M14.8 10 A1.2 1.2 0 1 0 17.2 10 A1.2 1.2 0 1 0 14.8 10 Z "
             + "M16.8 12.6 A1.2 1.2 0 1 0 19.2 12.6 A1.2 1.2 0 1 0 16.8 12.6 Z",
            oddEven: true
        },
        "games": {       // arcade joystick
            d: "M8.5 6 A3.5 3.5 0 1 0 15.5 6 A3.5 3.5 0 1 0 8.5 6 Z "
             + "M11 9.5 H13 V16 H11 Z "
             + "M5 16 H19 A1.5 1.5 0 0 1 20.5 17.5 V20.5 A1.5 1.5 0 0 1 19 22 "
             + "H5 A1.5 1.5 0 0 1 3.5 20.5 V17.5 A1.5 1.5 0 0 1 5 16 Z",
            oddEven: false
        },
        "media": {       // play symbol cut out of a disc
            d: "M2 12 A10 10 0 1 0 22 12 A10 10 0 1 0 2 12 Z "
             + "M9.5 7.5 L16.5 12 L9.5 16.5 Z",
            oddEven: true
        },
        "store": {       // shopping bag
            d: "M9 7.5 V6.5 A3 3 0 0 1 15 6.5 V7.5 H13.5 V6.5 A1.5 1.5 0 0 0 10.5 6.5 V7.5 Z "
             + "M4.5 7.5 H19.5 L18.4 20.6 A1.5 1.5 0 0 1 16.9 22 H7.1 A1.5 1.5 0 0 1 5.6 20.6 Z",
            oddEven: false
        },
        "settings": { d: _gearPath(8, 10.5, 7.8, 3.4), oddEven: true },
        "placeholder": {
            d: "M4 4 H20 V20 H4 Z M6 6 V18 H18 V6 Z",
            oddEven: true
        }
    })

    // Gear: `teeth` trapezoid teeth between rInner and rOuter, round hole.
    function _gearPath(teeth, rOuter, rInner, rHole) {
        const c = 12
        const step = 2 * Math.PI / teeth
        const pt = (r, a) => Number(c + r * Math.cos(a)).toFixed(2) + " " + Number(c + r * Math.sin(a)).toFixed(2)
        let d = ""
        for (let i = 0; i < teeth; ++i) {
            const a = i * step
            d += (i === 0 ? "M" : "L") + pt(rInner, a - step * 0.30)
               + " L" + pt(rOuter, a - step * 0.17)
               + " L" + pt(rOuter, a + step * 0.17)
               + " L" + pt(rInner, a + step * 0.30) + " "
        }
        d += "Z "
        d += "M" + (c - rHole) + " " + c + " A" + rHole + " " + rHole + " 0 1 0 " + (c + rHole) + " " + c
           + " A" + rHole + " " + rHole + " 0 1 0 " + (c - rHole) + " " + c + " Z"
        return d
    }

    Shape {
        width: 24
        height: 24
        scale: Math.min(icon.width, icon.height) / 24
        transformOrigin: Item.TopLeft
        x: (icon.width - width * scale) / 2
        y: (icon.height - height * scale) / 2
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeWidth: -1
            strokeColor: "transparent"
            fillColor: icon.color
            fillRule: icon._data.oddEven ? ShapePath.OddEvenFill : ShapePath.WindingFill
            PathSvg { path: icon._data.d }
        }
    }
}
