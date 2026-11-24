.pragma library

// Button glyphs for the PromptBar, per input style and action.
//
// Styles: "keyboard" (also mouse), "xbox", "playstation", "nintendo"
// (from InputRouter.gamepadStyle) and "remote" (HDMI-CEC TV remote).
// Gamepad mapping is positional (Accept = south button), so on Nintendo
// pads Accept shows "B" and Back "A", exactly where they are on the pad.
//
// spec(style, actionName) -> {
//   shape:  "circle" | "bumper" | "trigger" | "key" | "pill" | "dot",
//   text:   label drawn on the glyph ("" if a symbol or colour is used),
//   name:   what the glyph is, in words ("Red", "Cross", "Menu"): the
//           accessible name, and the label of symbol / colour glyphs,
//   symbol: name in `symbols` below, or "",
//   fill, fg: colours (fg = text / symbol colour),
//   outline: true for dark glyphs that need a light ring on the dark bar
// }
// Translations: text and name are marked with QT_TR_NOOP and translated
// in spec(), on every call. A language switch at run time reaches the
// glyphs only when their bindings re-evaluate (e.g. the next device
// change); MightyOS has no live language switch yet.
//
// Symbols are 24x24 SVG markup, rasterised through the SVG image plugin
// once per (symbol, colour) like Icons.js, so they stay vector-sharp at
// every Theme scale.

const dark = "#0e1117"        // Theme.background
const light = "#f3f5f9"       // Theme.text
const surface = "#2a3245"     // Theme.surfaceHigh

function _face(text, fill, fg, outline) {
    return { shape: "circle", text: text, name: text, symbol: "", fill: fill, fg: fg ?? dark, outline: !!outline }
}
function _symbol(shape, symbol, name, fill, fg, outline) {
    return { shape: shape, text: "", name: name, symbol: symbol, fill: fill, fg: fg, outline: !!outline }
}
function _dot(name, fill) {
    return { shape: "dot", text: "", name: name, symbol: "", fill: fill, fg: dark, outline: false }
}
function _label(shape, text, fill, fg, outline) {
    return { shape: shape, text: text, name: text, symbol: "", fill: fill ?? light, fg: fg ?? dark, outline: !!outline }
}

// Xbox: the familiar colours, dark letters (>= 4.5:1 on each fill).
const xbox = {
    Accept:       _face("A", "#5cc45a"),
    Back:         _face("B", "#f2665e"),
    Favorite:     _face("X", "#5aa9ff"),
    Search:       _face("Y", "#ffcc4d"),
    Options:      _symbol("circle", "menu", QT_TR_NOOP("Menu"), light, dark),
    QuickPanel:   _symbol("circle", "view", QT_TR_NOOP("View"), light, dark),
    PrevCategory: _label("bumper", "LB"),
    NextCategory: _label("bumper", "RB"),
    PageUp:       _label("trigger", "LT"),
    PageDown:     _label("trigger", "RT"),
    Guide:        _symbol("circle", "home", QT_TR_NOOP("Guide"), light, dark)
}

// PlayStation: dark buttons with the coloured symbols.
const playstation = {
    Accept:       _symbol("circle", "cross", QT_TR_NOOP("Cross"), surface, "#8ab8ff", true),
    Back:         _symbol("circle", "ring", QT_TR_NOOP("Circle"), surface, "#ff7f77", true),
    Favorite:     _symbol("circle", "square", QT_TR_NOOP("Square"), surface, "#f59bd8", true),
    Search:       _symbol("circle", "triangle", QT_TR_NOOP("Triangle"), surface, "#4fd6a8", true),
    Options:      _symbol("pill", "menu", QT_TR_NOOP("Options"), light, dark),
    QuickPanel:   _label("pill", QT_TR_NOOP("Create")),
    PrevCategory: _label("bumper", "L1"),
    NextCategory: _label("bumper", "R1"),
    PageUp:       _label("trigger", "L2"),
    PageDown:     _label("trigger", "R2"),
    Guide:        _label("circle", "PS")
}

// Nintendo: letters by position (south = B, east = A, west = Y, north = X).
const nintendo = {
    Accept:       _face("B", surface, light, true),
    Back:         _face("A", surface, light, true),
    Favorite:     _face("Y", surface, light, true),
    Search:       _face("X", surface, light, true),
    Options:      _label("circle", "+"),
    QuickPanel:   _label("circle", "−"),
    PrevCategory: _label("bumper", "L"),
    NextCategory: _label("bumper", "R"),
    PageUp:       _label("trigger", "ZL"),
    PageDown:     _label("trigger", "ZR"),
    Guide:        _symbol("circle", "home", QT_TR_NOOP("Home"), light, dark)
}

// TV remote: OK / Back, the four colour keys, transport and channel keys.
const remote = {
    Accept:       _label("pill", QT_TR_NOOP("OK")),
    Back:         _label("pill", QT_TR_NOOP("Back")),
    Favorite:     _dot(QT_TR_NOOP("Blue"), "#4d8dff"),
    Search:       _dot(QT_TR_NOOP("Yellow"), "#ffd23f"),
    Options:      _dot(QT_TR_NOOP("Red"), "#f2554c"),
    QuickPanel:   _dot(QT_TR_NOOP("Green"), "#4cc46a"),
    PrevCategory: _symbol("pill", "rewind", QT_TR_NOOP("Rewind"), light, dark),
    NextCategory: _symbol("pill", "forward", QT_TR_NOOP("Fast forward"), light, dark),
    PageUp:       _label("pill", QT_TR_NOOP("CH+")),
    PageDown:     _label("pill", QT_TR_NOOP("CH−")),
    Guide:        _label("pill", QT_TR_NOOP("Home"))
}

const keyboard = {
    Accept:       _label("key", QT_TR_NOOP("Enter")),
    Back:         _label("key", QT_TR_NOOP("Esc")),
    Favorite:     _label("key", QT_TR_NOOP("Ins")),
    Search:       _label("key", "/"),
    Options:      _label("key", QT_TR_NOOP("Tab")),
    QuickPanel:   _label("key", "F12"),
    PrevCategory: _label("key", QT_TR_NOOP("PgUp")),
    NextCategory: _label("key", QT_TR_NOOP("PgDn")),
    PageUp:       _label("key", QT_TR_NOOP("Home")),
    PageDown:     _label("key", QT_TR_NOOP("End")),
    Guide:        _label("key", QT_TR_NOOP("Ctrl+Home"))
}

const styles = { keyboard: keyboard, xbox: xbox, playstation: playstation,
                 nintendo: nintendo, remote: remote }

function spec(style, actionName) {
    const table = styles[style] ?? keyboard
    const entry = table[actionName] ?? _label("key", "?")
    return Object.assign({}, entry, { text: entry.text.length > 0 ? qsTr(entry.text) : "",
                                      name: qsTr(entry.name) })
}

// 24x24 symbols. `C` is replaced by the colour.
const symbols = {
    menu:     "<path d='M6 7.5h12M6 12h12M6 16.5h12' stroke='C' stroke-width='2.4' stroke-linecap='round' fill='none'/>",
    view:     "<rect x='4.5' y='5' width='10' height='9' rx='1.5' stroke='C' stroke-width='2' fill='none'/>"
            + "<rect x='9.5' y='10' width='10' height='9' rx='1.5' fill='C'/>",
    home:     "<path d='M4 11.5L12 5l8 6.5V19a1 1 0 0 1-1 1h-4.5v-5h-5v5H5a1 1 0 0 1-1-1z' fill='C'/>",
    cross:    "<path d='M6.5 6.5l11 11M17.5 6.5l-11 11' stroke='C' stroke-width='2.6' stroke-linecap='round'/>",
    ring:     "<circle cx='12' cy='12' r='6.2' stroke='C' stroke-width='2.4' fill='none'/>",
    square:   "<rect x='6.3' y='6.3' width='11.4' height='11.4' rx='0.8' stroke='C' stroke-width='2.4' fill='none'/>",
    triangle: "<path d='M12 5.2L19 17.6H5z' stroke='C' stroke-width='2.3' stroke-linejoin='round' fill='none'/>",
    rewind:   "<path d='M11.5 6.5v11L4 12zM20 6.5v11L12.5 12z' fill='C'/>",
    forward:  "<path d='M12.5 6.5v11L20 12zM4 6.5v11L11.5 12z' fill='C'/>"
}

const _urls = {}

function _hex(color) {
    const c = (v) => ("0" + Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16)).slice(-2)
    return "#" + c(color.r) + c(color.g) + c(color.b)
}

function symbolUrl(name, color) {
    const body = symbols[name]
    if (!body)
        return ""
    const hex = typeof color === "string" ? color : _hex(color)
    const key = name + "|" + hex
    let url = _urls[key]
    if (url === undefined) {
        const svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
                  + body.split("'C'").join("'" + hex + "'") + "</svg>"
        url = "data:image/svg+xml;utf8," + encodeURIComponent(svg)
        _urls[key] = url
    }
    return url
}
