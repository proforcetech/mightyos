pragma ComponentBehavior: Bound
import QtQuick

// Bottom hint bar: "(A/Enter) Open  (B/Esc) Back". `prompts` is a list of
// { button, key, label }. Button glyphs become device-specific in M3.
Row {
    id: bar

    property var prompts: []

    spacing: Theme.spacingL

    Repeater {
        model: bar.prompts

        delegate: Row {
            id: prompt
            required property var modelData
            spacing: Theme.spacingS

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.px(44)
                height: width
                radius: width / 2
                color: Theme.text
                Text {
                    anchors.centerIn: parent
                    text: prompt.modelData.button
                    color: Theme.background
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.Bold
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "/ " + prompt.modelData.key + "   " + prompt.modelData.label
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}
