import QtQuick

// A large square category / app tile: icon + label. Purely visual plus mouse
// handling; focus is owned by the containing TileRow.
Item {
    id: tile

    property string label
    property string iconName
    property color tint: Theme.accent
    property bool focused: false
    property real bumpOffset: 0

    signal hovered()
    signal clicked()

    width: Theme.tileSize
    height: Theme.tileSize
    scale: focused ? Theme.focusScale : 1.0
    z: focused ? 1 : 0
    transform: Translate { x: tile.bumpOffset }

    Behavior on scale {
        NumberAnimation { duration: Theme.animFast; easing.type: Easing.OutCubic }
    }

    Rectangle {
        id: card
        anchors.fill: parent
        radius: Theme.radius
        color: tile.focused ? Theme.surfaceHigh : Theme.surface
        antialiasing: true

        // Thin category colour band along the top edge.
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
            anchors.topMargin: Theme.px(44)
            width: Theme.tileIconSize
            height: Theme.tileIconSize
            name: tile.iconName
            color: tile.focused ? Theme.text : Theme.textDim
        }

        Text {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            anchors.bottomMargin: Theme.px(30)
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: tile.label
            color: tile.focused ? Theme.text : Theme.textDim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontLabel
            font.weight: tile.focused ? Font.DemiBold : Font.Normal
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        onPositionChanged: (mouse) => {
            if (InputMap.pointerMoved(mapToItem(null, mouse.x, mouse.y)))
                tile.hovered()
        }
        onClicked: tile.clicked()
    }
}
