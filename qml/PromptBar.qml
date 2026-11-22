pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// Bottom hint bar, driven by the current screen's (or open modal's)
// `prompts`: a list of { action: InputRouter.<Action>, label } with
// optional `pad` / `key` glyph overrides. The glyph follows
// InputRouter.lastSource: gamepad/remote show controller buttons, keyboard
// and mouse show key caps. Device-specific button art (Xbox/PS/Nintendo)
// comes in M3. Every prompt is also clickable, which gives mouse users Back,
// Options etc. without a dedicated button.
Row {
    id: bar

    property var prompts: []

    readonly property bool _padGlyphs: InputRouter.lastSource === InputRouter.Gamepad
                                       || InputRouter.lastSource === InputRouter.Remote

    // Default glyphs per action (PLAN.md section 3).
    function _glyph(prompt: var): string {
        if (_padGlyphs && prompt.pad) return prompt.pad
        if (!_padGlyphs && prompt.key) return prompt.key
        switch (prompt.action) {
        case InputRouter.Accept:       return _padGlyphs ? "A" : qsTr("Enter")
        case InputRouter.Back:         return _padGlyphs ? "B" : qsTr("Esc")
        case InputRouter.Favorite:     return _padGlyphs ? "X" : "F"
        case InputRouter.Search:       return _padGlyphs ? "Y" : "/"
        case InputRouter.Options:      return _padGlyphs ? qsTr("Start") : "M"
        case InputRouter.QuickPanel:   return _padGlyphs ? qsTr("Select") : "Q"
        case InputRouter.PrevCategory: return _padGlyphs ? "LB" : qsTr("PgUp")
        case InputRouter.NextCategory: return _padGlyphs ? "RB" : qsTr("PgDn")
        case InputRouter.PageUp:       return _padGlyphs ? "LT" : qsTr("Home")
        case InputRouter.PageDown:     return _padGlyphs ? "RT" : qsTr("End")
        }
        return "?"
    }

    spacing: Theme.spacingL

    Repeater {
        model: bar.prompts

        delegate: Item {
            id: prompt
            required property var modelData

            width: content.implicitWidth
            height: content.implicitHeight

            Row {
                id: content
                spacing: Theme.spacingS

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(height, glyph.implicitWidth + 2 * Theme.spacingS)
                    height: Theme.px(44)
                    radius: bar._padGlyphs ? height / 2 : Theme.px(8)
                    color: area.containsMouse ? Theme.accent : Theme.text
                    Text {
                        id: glyph
                        anchors.centerIn: parent
                        text: bar._glyph(prompt.modelData)
                        color: Theme.background
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.Bold
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: prompt.modelData.label
                    color: area.containsMouse ? Theme.text : Theme.textDim
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSmall
                }
            }

            MouseArea {
                id: area
                anchors.fill: parent
                anchors.margins: -Theme.spacingS
                hoverEnabled: true
                onClicked: {
                    if (InputRouter.pointerAction())
                        InputRouter.dispatch(prompt.modelData.action, "", InputRouter.Mouse)
                }
            }
        }
    }
}
