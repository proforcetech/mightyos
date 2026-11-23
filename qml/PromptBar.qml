pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// Bottom hint bar, driven by the current screen's (or open modal's)
// `prompts`: a list of { action: InputRouter.<Action>, label } with an
// optional `actions: [...]` when one prompt stands for a pair (LB RB).
//
// The glyphs follow the device used last (InputRouter.lastSource):
// keyboard / mouse show key caps, a gamepad shows its own buttons
// (InputRouter.gamepadStyle: Xbox, PlayStation, Nintendo by position), the
// TV remote shows OK / Back / colour keys. Every prompt is also clickable,
// which gives mouse users Back, Options etc. without a dedicated button.
Row {
    id: bar

    property var prompts: []

    // "keyboard" | "xbox" | "playstation" | "nintendo" | "remote"
    readonly property string glyphStyle: {
        switch (InputRouter.lastSource) {
        case InputRouter.Gamepad:
            return InputRouter.gamepadStyle === InputRouter.PlayStation ? "playstation"
                 : InputRouter.gamepadStyle === InputRouter.Nintendo ? "nintendo" : "xbox"
        case InputRouter.Remote:
            return "remote"
        }
        return "keyboard"
    }

    spacing: Theme.spacingL

    Repeater {
        model: bar.prompts

        delegate: Item {
            id: prompt
            required property var modelData
            required property int index

            objectName: "prompt" + index
            width: content.implicitWidth
            height: content.implicitHeight

            Row {
                id: content
                spacing: Theme.spacingS

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Math.round(Theme.spacingS / 2)
                    Repeater {
                        model: prompt.modelData.actions ?? [prompt.modelData.action]
                        delegate: ButtonGlyph {
                            required property int modelData
                            objectName: "glyph"
                            action: modelData
                            glyphStyle: bar.glyphStyle
                            highlighted: area.containsMouse
                        }
                    }
                }
                Text {
                    textFormat: Text.PlainText
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
