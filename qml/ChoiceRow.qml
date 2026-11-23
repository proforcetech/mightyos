pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// One horizontal row of choices for forms (the profile editor): text
// pills, icon chips or colour swatches. On a gamepad the focus *is* the
// selection: Left/Right change it (bump at the ends, no wrap). The owner
// forwards actions via moveFocus(); a click selects and asks for focus.
//
// choices: [{ id, label?, icon?, color? }]
Item {
    id: choiceRow

    property string label              // caption on the left
    property var choices: []
    property int currentIndex: 0
    property bool active: false        // this row has the form's focus
    property string style: "text"      // "text" | "icon" | "swatch"
    property color iconColor: Theme.text
    property real bumpX: 0

    signal focusRequested()

    readonly property string currentId: currentIndex >= 0 && currentIndex < choices.length
                                        ? choices[currentIndex].id : ""
    readonly property int _chip: style === "text" ? Theme.px(64) : Theme.px(80)

    implicitWidth: caption.width + Theme.spacingM + chips.implicitWidth
    implicitHeight: Math.max(caption.implicitHeight, _chip) + 2 * Theme.focusGap + 2 * Theme.focusBorder

    function moveFocus(direction: string): bool {
        const next = currentIndex + (direction === "left" ? -1 : direction === "right" ? 1 : 0)
        if (next === currentIndex)
            return false
        if (next < 0 || next >= choices.length) {
            if (!_bump.running) {
                _bump.dir = direction === "left" ? -1 : 1
                _bump.restart()
                SoundFx.play(SoundFx.Bump)
            }
            return false
        }
        currentIndex = next
        SoundFx.play(SoundFx.Move)
        return true
    }

    function selectId(id: string) {
        for (let i = 0; i < choices.length; ++i) {
            if (choices[i].id === id) {
                currentIndex = i
                return
            }
        }
        currentIndex = 0
    }

    Text {
        textFormat: Text.PlainText
        id: caption
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.px(170)
        text: choiceRow.label
        color: choiceRow.active ? Theme.text : Theme.textDim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        font.weight: choiceRow.active ? Font.DemiBold : Font.Normal
        elide: Text.ElideRight
    }

    Row {
        id: chips
        anchors.left: caption.right
        anchors.leftMargin: Theme.spacingM
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacingS

        Repeater {
            model: choiceRow.choices
            delegate: Rectangle {
                id: chip
                required property int index
                required property var modelData
                readonly property bool selected: index === choiceRow.currentIndex
                readonly property bool focused: selected && choiceRow.active

                width: choiceRow.style === "text"
                       ? Math.max(choiceRow._chip * 1.6, text.implicitWidth + 2 * Theme.spacingM)
                       : choiceRow._chip
                height: choiceRow._chip
                radius: choiceRow.style === "text" ? height / 2 : Theme.radius / 2
                color: choiceRow.style === "swatch" ? (chip.modelData.color ?? Theme.surface)
                     : chip.selected ? Theme.surfaceHigh : Theme.backgroundAlt
                border.width: chip.focused ? Theme.focusBorder
                            : chip.selected ? Math.max(2, Theme.px(3)) : Math.max(1, Theme.px(2))
                border.color: chip.focused ? Theme.accent : chip.selected ? Theme.text : Theme.surfaceHigh
                scale: chip.focused ? 1.06 : 1.0
                transform: Translate { x: chip.selected ? choiceRow.bumpX : 0 }
                Behavior on scale { NumberAnimation { duration: Theme.animFast } }

                Text {
                    textFormat: Text.PlainText
                    id: text
                    anchors.centerIn: parent
                    visible: choiceRow.style === "text"
                    text: chip.modelData.label ?? ""
                    color: chip.selected ? Theme.text : Theme.textDim
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSmall
                    font.weight: chip.selected ? Font.DemiBold : Font.Normal
                }
                Icon {
                    anchors.centerIn: parent
                    visible: choiceRow.style === "icon"
                    width: parent.width * 0.62
                    height: width
                    name: chip.modelData.icon ?? ""
                    color: chip.selected ? choiceRow.iconColor : Theme.textDim
                }
                // Tick on the selected swatch (colour alone isn't enough).
                Icon {
                    anchors.centerIn: parent
                    visible: choiceRow.style === "swatch" && chip.selected
                    width: parent.width * 0.45
                    height: width
                    name: "favorites"
                    color: Theme.background
                }

                PointerArea {
                    anchors.fill: parent
                    onTapped: (button) => {
                        if (button !== Qt.LeftButton || !InputRouter.pointerAction())
                            return
                        choiceRow.focusRequested()
                        if (choiceRow.currentIndex !== chip.index) {
                            choiceRow.currentIndex = chip.index
                            SoundFx.play(SoundFx.Move)
                        }
                    }
                }
            }
        }
    }

    SequentialAnimation {
        id: _bump
        property int dir: 1
        NumberAnimation {
            target: choiceRow; property: "bumpX"; to: _bump.dir * Theme.bumpDistance / 2
            duration: Theme.animFast / 2; easing.type: Easing.OutQuad
        }
        NumberAnimation {
            target: choiceRow; property: "bumpX"; to: 0
            duration: Theme.animFast; easing.type: Easing.OutBack
        }
    }
}
