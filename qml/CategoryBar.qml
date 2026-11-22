pragma ComponentBehavior: Bound
import QtQuick

// Top-level category tabs ("pills"): Favorites, Emulators, Games, ...
// A TileRow with a compact delegate, so focus, bump, hover, click and wheel
// behave exactly like every other row. The current category stays marked
// (tinted icon, filled pill) while focus is in the content below.
//
// Model contract: TileRow's (itemId, label, icon, tint).
TileRow {
    id: bar

    tileWidth: Theme.px(250)
    tileHeight: Theme.pillHeight
    width: parent ? parent.width : 0
    spacing: Theme.spacingS

    delegate: Item {
        id: pill
        required property int index
        required property var model

        readonly property bool current: ListView.isCurrentItem
        readonly property bool focused: current && bar.active

        width: content.implicitWidth + 2 * Theme.pillPadding
        height: bar.tileHeight
        scale: focused ? 1.04 : 1.0
        transform: Translate { x: pill.current ? bar.bumpOffset : 0 }
        Behavior on scale { NumberAnimation { duration: Theme.animFast; easing.type: Easing.OutCubic } }

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: pill.focused ? Theme.surfaceHigh : pill.current ? Theme.surface : "transparent"
            border.width: pill.current && !pill.focused ? Math.max(1, Theme.px(2)) : 0
            border.color: Theme.surfaceHigh
        }

        Row {
            id: content
            anchors.centerIn: parent
            spacing: Theme.spacingS
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.pillIconSize
                height: Theme.pillIconSize
                name: pill.model.icon ?? ""
                color: pill.current ? (pill.model.tint ?? Theme.accent) : Theme.textDim
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: pill.model.label ?? ""
                color: pill.current ? Theme.text : Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontLabel
                font.weight: pill.current ? Font.DemiBold : Font.Normal
            }
        }

        PointerArea {
            anchors.fill: parent
            onHovered: bar.hoverAt(pill.index)
            onTapped: (button) => bar.clickAt(pill.index, button)
        }
    }
}
