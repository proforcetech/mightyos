.pragma library

// Shared vector icon table (24x24 SVG path data). One copy per engine
// (.pragma library), so every Icon instance reads the same object instead
// of building its own table.
//
// Each entry: { d: <svg path>, oddEven: <bool fill rule> }.
// Unknown names fall back to "placeholder". dataUrl() turns an entry into
// an SVG data URL for Image (see Icon.qml).

function gearPath(teeth, rOuter, rInner, rHole) {
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

const table = {
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
    "settings": { d: gearPath(8, 10.5, 7.8, 3.4), oddEven: true },
    "cartridge": {   // cartridge with label recess
        d: "M5 2 H19 V18 L17 20 V22 H7 V20 L5 18 Z M8 5 H16 V12 H8 Z",
        oddEven: true
    },
    "disc": {
        d: "M2 12 A10 10 0 1 0 22 12 A10 10 0 1 0 2 12 Z "
         + "M9 12 A3 3 0 1 0 15 12 A3 3 0 1 0 9 12 Z",
        oddEven: true
    },
    "handheld": {    // wide console with screen cut out
        d: "M4 6 H20 A3 3 0 0 1 23 9 V15 A3 3 0 0 1 20 18 H4 A3 3 0 0 1 1 15 V9 A3 3 0 0 1 4 6 Z "
         + "M7 8 H17 V16 H7 Z",
        oddEven: true
    },
    "arcade": {      // cabinet with screen cut out
        d: "M6 2 H18 V9 L20 11 V22 H4 V11 L6 9 Z M8 4 V9 H16 V4 Z",
        oddEven: true
    },
    "steam": {       // generic "PC launcher" mark: disc with two joints
        d: "M2 12 A10 10 0 1 0 22 12 A10 10 0 1 0 2 12 Z "
         + "M13 9 A3 3 0 1 0 19 9 A3 3 0 1 0 13 9 Z "
         + "M5 15 A2 2 0 1 0 9 15 A2 2 0 1 0 5 15 Z",
        oddEven: true
    },
    "display": {     // monitor with stand
        d: "M2 4 H22 V17 H2 Z M4 6 V15 H20 V6 Z M9 19 H15 V21 H9 Z",
        oddEven: true
    },
    "audio": {       // speaker
        d: "M3 9 H7 L13 4 V20 L7 15 H3 Z M16 7 H18 V17 H16 Z M20 4 H22 V20 H20 Z",
        oddEven: false
    },
    "network": {     // signal bars
        d: "M3 17 H6 V21 H3 Z M8.5 13 H11.5 V21 H8.5 Z M14 9 H17 V21 H14 Z M19.5 4 H22.5 V21 H19.5 Z",
        oddEven: false
    },
    "power": {       // power symbol: ring with a gap and a bar
        d: "M11 2 H13 V12 H11 Z "
         + "M7.05 5.05 L8.46 6.46 A6.5 6.5 0 1 0 15.54 6.46 L16.95 5.05 A8.5 8.5 0 1 1 7.05 5.05 Z",
        oddEven: false
    },
    "folder": {
        d: "M2 5 H9 L11 7 H22 V20 H2 Z",
        oddEven: false
    },
    "moon": {        // crescent (game streaming)
        d: "M14 2 A10 10 0 1 0 22 14 A8 8 0 1 1 14 2 Z",
        oddEven: false
    },
    "placeholder": {
        d: "M4 4 H20 V20 H4 Z M6 6 V18 H18 V6 Z",
        oddEven: true
    }
}

function lookup(name) {
    return table[name] || table["placeholder"]
}

// Memoised: bindings re-evaluate often, the set of (name, colour) pairs is
// small.
const _urls = {}

function _channel(v) {
    return Math.round(Math.max(0, Math.min(1, v)) * 255)
}

// `color` is a QML color value. Same (name, colour) -> same string, so the
// image cache shares one rasterised texture between all instances.
function dataUrl(name, color) {
    const key = name + "|" + color
    let url = _urls[key]
    if (url === undefined) {
        const icon = lookup(name)
        const svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
                  + "<path d='" + icon.d + "'"
                  + " fill='rgb(" + _channel(color.r) + "," + _channel(color.g) + "," + _channel(color.b) + ")'"
                  + " fill-opacity='" + Number(color.a).toFixed(3) + "'"
                  + " fill-rule='" + (icon.oddEven ? "evenodd" : "nonzero") + "'/></svg>"
        url = "data:image/svg+xml;utf8," + encodeURIComponent(svg)
        _urls[key] = url
    }
    return url
}
