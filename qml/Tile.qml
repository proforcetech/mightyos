import QtQuick

// A large tile: icon + label (square category/app/system tile) or, with
// `poster`, a 3:4 game cover. Purely visual plus raw mouse signals; focus
// and activation are owned by the containing TileRow / TileGrid.
Item {
    id: tile

    property string label
    property string subtitle
    property string iconName
    property color tint: Theme.accent
    property bool focused: false
    property bool available: true     // false: greyed "Unavailable" tile
    property bool poster: false
    property real bumpX: 0
    property real bumpY: 0

    // Pointer really moved over the tile (InputRouter.pointerMoved gating).
    signal hovered()
    // Left or right button; the owning view decides what that means.
    signal clicked(int button)

    width: poster ? Theme.posterWidth : Theme.tileSize
    height: poster ? Theme.posterHeight : Theme.tileSize
    scale: focused ? Theme.focusScale : 1.0
    z: focused ? 1 : 0
    transform: Translate { x: tile.bumpX; y: tile.bumpY }

    Behavior on scale {
        NumberAnimation { duration: Theme.animFast; easing.type: Easing.OutCubic }
    }

    Rectangle {
        id: card
        anchors.fill: parent
        radius: Theme.radius
        color: tile.focused ? Theme.surfaceHigh : Theme.surface
        opacity: tile.available ? 1.0 : 0.45
        antialiasing: true

        // Colour band along the top edge (category / system colour).
        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            anchors.leftMargin: Theme.radius
            anchors.rightMargin: Theme.radius
            height: Math.max(2, Theme.px(6))
            radius: height / 2
            color: tile.tint
            opacity: tile.focused ? 1.0 : 0.6
        }

        Icon {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: tile.poster ? Theme.px(40) : Theme.px(44)
            width: tile.poster ? Theme.posterIconSize : Theme.tileIconSize
            height: width
            name: tile.iconName
            color: tile.focused ? Theme.text : Theme.textDim
        }

        Column {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            anchors.margins: Theme.spacingS
            anchors.bottomMargin: tile.poster ? Theme.spacingM : Theme.px(30)
            spacing: Theme.px(4)

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: tile.poster ? Text.Wrap : Text.NoWrap
                maximumLineCount: tile.poster ? 3 : 1
                elide: Text.ElideRight
                text: tile.label
                color: tile.focused ? Theme.text : Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: tile.poster ? Theme.fontSmall : Theme.fontLabel
                font.weight: tile.focused ? Font.DemiBold : Font.Normal
            }
            Text {
                width: parent.width
                visible: text.length > 0
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: tile.available ? tile.subtitle : qsTr("Unavailable")
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSmall
            }
        }
    }

    PointerArea {
        anchors.fill: parent
        onHovered: tile.hovered()
        onTapped: (button) => tile.clicked(button)
    }
}
