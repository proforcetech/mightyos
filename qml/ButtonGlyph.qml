import QtQuick
import MightyOS.Core
import "Glyphs.js" as Glyphs

// One button glyph (PromptBar): the button that triggers `action` in the
// given input style, drawn from shapes and SVG symbols (Glyphs.js) so it
// stays sharp at every Theme scale. `highlighted` (mouse hover over the
// prompt) adds the accent ring.
Item {
    id: glyph

    property int action: InputRouter.None
    // "keyboard" | "xbox" | "playstation" | "nintendo" | "remote"
    property string glyphStyle: "keyboard"
    property bool highlighted: false

    readonly property var spec: Glyphs.spec(glyphStyle, InputRouter.actionName(action))
    // What the glyph says or is ("A", "Cross", "Red"): tests, accessibility.
    readonly property string label: spec.text.length > 0 ? spec.text : spec.name

    readonly property real _h: Theme.px(44)

    implicitHeight: _h
    implicitWidth: {
        switch (spec.shape) {
        case "circle":
        case "dot":
            return _h
        case "bumper":
        case "trigger":
            return Math.max(_h * 1.45, text.implicitWidth + 2 * Theme.spacingS)
        }
        return Math.max(_h, (spec.symbol.length > 0 ? _h * 0.9 : text.implicitWidth) + 2 * Theme.spacingS)
    }

    Accessible.role: Accessible.StaticText
    Accessible.name: spec.name.length > 0 ? spec.name : spec.text

    Rectangle {
        id: body
        readonly property real full: height / 2
        anchors.centerIn: parent
        width: glyph.spec.shape === "dot" ? glyph._h * 0.8 : parent.width
        height: glyph.spec.shape === "dot" ? glyph._h * 0.55 : parent.height
        color: glyph.highlighted && glyph.spec.shape === "key" ? Theme.accent : glyph.spec.fill
        // Shapes: circle; pill / bumper fully rounded; trigger rounded on
        // top like the real thing; key caps and colour keys slightly.
        radius: {
            switch (glyph.spec.shape) {
            case "circle":
            case "pill":
            case "bumper":
                return full
            case "dot":
                return Theme.px(6)
            }
            return Theme.px(8)
        }
        topLeftRadius: glyph.spec.shape === "trigger" ? full : radius
        topRightRadius: glyph.spec.shape === "trigger" ? full : radius
        bottomLeftRadius: glyph.spec.shape === "trigger" ? Theme.px(8) : radius
        bottomRightRadius: glyph.spec.shape === "trigger" ? Theme.px(8) : radius
        border.width: glyph.highlighted ? Math.max(2, Theme.px(4))
                    : glyph.spec.outline ? Math.max(1, Theme.px(2)) : 0
        border.color: glyph.highlighted ? Theme.accent : Theme.textDim
    }

    Image {
        visible: glyph.spec.symbol.length > 0
        anchors.centerIn: body
        width: Math.round(glyph._h * 0.95)
        height: width
        sourceSize.width: width
        sourceSize.height: height
        smooth: true
        source: visible ? Glyphs.symbolUrl(glyph.spec.symbol, glyph.spec.fg) : ""
    }

    Text {
        id: text
        visible: glyph.spec.text.length > 0
        anchors.centerIn: body
        text: glyph.spec.text
        textFormat: Text.PlainText
        color: glyph.highlighted && glyph.spec.shape === "key" ? Theme.background : glyph.spec.fg
        font.family: Theme.fontFamily
        font.pixelSize: glyph.spec.text.length > 2 ? Theme.fontSmall : Theme.px(26)
        font.weight: Font.Bold
    }
}
