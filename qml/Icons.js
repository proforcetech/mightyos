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
    "apps": {        // 2x2 grid of tiles
        d: "M4 4 H10 V10 H4 Z M14 4 H20 V10 H14 Z M4 14 H10 V20 H4 Z M14 14 H20 V20 H14 Z",
        oddEven: false
    },
    "add": {
        d: "M11 4 H13 V11 H20 V13 H13 V20 H11 V13 H4 V11 H11 Z",
        oddEven: false
    },
    "profile": {     // head and shoulders
        d: "M12 3 A4.5 4.5 0 1 0 12 12 A4.5 4.5 0 1 0 12 3 Z M3 21 C3 16.5 7 14 12 14 C17 14 21 16.5 21 21 Z",
        oddEven: false
    },
    // Profile avatars (ProfileService::avatars(): "avatar-" + name).
    "avatar-robot": {
        d: "M11 2 H13 V5 H17 A3 3 0 0 1 20 8 V15 A3 3 0 0 1 17 18 H7 A3 3 0 0 1 4 15 V8 A3 3 0 0 1 7 5 H11 Z "
         + "M8.5 8.5 A1.6 1.6 0 1 0 8.5 11.7 A1.6 1.6 0 1 0 8.5 8.5 Z "
         + "M15.5 8.5 A1.6 1.6 0 1 0 15.5 11.7 A1.6 1.6 0 1 0 15.5 8.5 Z "
         + "M9 14 H15 V15.5 H9 Z M1.5 9.5 H3.2 V13.5 H1.5 Z M20.8 9.5 H22.5 V13.5 H20.8 Z M8 19.5 H16 V22 H8 Z",
        oddEven: true
    },
    "avatar-cat": {
        d: "M4 3 L9 8 H15 L20 3 V13 A8 8 0 0 1 4 13 Z "
         + "M8 11 A1.4 1.4 0 1 0 8 13.8 A1.4 1.4 0 1 0 8 11 Z "
         + "M16 11 A1.4 1.4 0 1 0 16 13.8 A1.4 1.4 0 1 0 16 11 Z M10.5 16 H13.5 L12 17.6 Z",
        oddEven: true
    },
    "avatar-ghost": {
        d: "M12 2 A8 8 0 0 1 20 10 V21 L17.33 19 L14.67 21 L12 19 L9.33 21 L6.67 19 L4 21 V10 A8 8 0 0 1 12 2 Z "
         + "M9 8 A1.6 2 0 1 0 9 12 A1.6 2 0 1 0 9 8 Z M15 8 A1.6 2 0 1 0 15 12 A1.6 2 0 1 0 15 8 Z",
        oddEven: true
    },
    "avatar-rocket": {
        d: "M12 2 C15.5 4.5 17 8.5 16 14 H8 C7 8.5 8.5 4.5 12 2 Z "
         + "M12 6.8 A1.8 1.8 0 1 0 12 10.4 A1.8 1.8 0 1 0 12 6.8 Z "
         + "M7.4 11.5 L4.5 15 V18.5 L7.8 15.2 Z M16.6 11.5 L19.5 15 V18.5 L16.2 15.2 Z M10 15.2 H14 L12 21.5 Z",
        oddEven: true
    },
    "avatar-alien": {
        d: "M12 2 C17 2 20 5.5 20 10 C20 15 15 21 12 22 C9 21 4 15 4 10 C4 5.5 7 2 12 2 Z "
         + "M6.5 10 C8 9.5 10 10.5 10.5 13 C9 13.5 7 12.5 6.5 10 Z "
         + "M17.5 10 C16 9.5 14 10.5 13.5 13 C15 13.5 17 12.5 17.5 10 Z",
        oddEven: true
    },
    "avatar-bolt": {
        d: "M13 2 L4 14 H11 L10 22 L20 9 H13 Z",
        oddEven: false
    },
    "avatar-heart": {
        d: "M12 21 L10.6 19.7 C5.4 15 2 11.9 2 8.1 C2 5 4.4 2.6 7.5 2.6 C9.2 2.6 10.9 3.4 12 4.7 "
         + "C13.1 3.4 14.8 2.6 16.5 2.6 C19.6 2.6 22 5 22 8.1 C22 11.9 18.6 15 13.4 19.7 Z",
        oddEven: false
    },
    "avatar-crown": {
        d: "M3 7 L8 12 L12 5 L16 12 L21 7 L19 18 H5 Z M5 19.5 H19 V21.5 H5 Z",
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
