import QtQuick

// A large tile: icon + label (square category/app/system tile) or, with
// `poster`, a 3:4 game cover. Purely visual plus raw mouse signals; focus
// and activation are owned by the containing TileRow / TileGrid.
//
// Artwork: `artwork` is an image URL (image://art/..., decoded off the GUI
// thread). Until it is ready, or if it fails, the built-in vector icon
// `iconName` is shown, so a tile never looks empty.
//
// The tile may be sized by its view (smaller "Recent" tiles); icon, margins
// and badges scale with it, text never drops below Theme.fontSmall.
Item {
    id: tile

    property string label
    property string subtitle
    property string iconName
    property url artwork
    property color tint: Theme.accent
    // Draw the vector icon in `tint` (profile avatars) instead of text colour.
    property bool tintIcon: false
    property bool focused: false
    property bool available: true     // false: greyed "Unavailable" tile
    property bool favorite: false     // star badge
    property string badge             // corner label, e.g. "Kids"
    property bool poster: false
    property real bumpX: 0
    property real bumpY: 0

    // Pointer really moved over the tile (InputRouter.pointerMoved gating).
    signal hovered()
    // Left or right button; the owning view decides what that means.
    signal clicked(int button)

    readonly property real _k: height / (poster ? Theme.posterHeight : Theme.tileSize)
    readonly property bool _compact: _k < 0.8

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
        radius: Theme.radius * Math.min(1, tile._k)
        color: tile.focused ? Theme.surfaceHigh : Theme.surface
        opacity: tile.available ? 1.0 : 0.45
        antialiasing: true

        // Colour band along the top edge (category / system colour).
        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            anchors.leftMargin: card.radius
            anchors.rightMargin: card.radius
            height: Math.max(2, Theme.px(6))
            radius: height / 2
            color: tile.tint
            opacity: tile.focused ? 1.0 : 0.6
        }

        Item {
            id: iconBox
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: (tile.poster ? Theme.px(40) : Theme.px(44)) * tile._k
            width: (tile.poster ? Theme.posterIconSize : Theme.tileIconSize) * tile._k
            height: width

            Image {
                id: art
                objectName: "artImage"
                anchors.fill: parent
                source: tile.artwork
                visible: status === Image.Ready
                fillMode: Image.PreserveAspectFit
                // Decode at the focused (scaled-up) size so focus stays crisp.
                sourceSize.width: Math.ceil(width * Theme.focusScale)
                sourceSize.height: Math.ceil(height * Theme.focusScale)
                asynchronous: true
                smooth: true
            }
            Icon {
                anchors.fill: parent
                visible: art.status !== Image.Ready
                name: tile.iconName
                color: tile.tintIcon ? tile.tint : (tile.focused ? Theme.text : Theme.textDim)
            }
        }

        Column {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            anchors.margins: Theme.spacingS * Math.min(1, tile._k)
            anchors.bottomMargin: (tile.poster ? Theme.spacingM : Theme.px(30)) * tile._k
            spacing: Theme.px(4)

            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: tile.poster ? Text.Wrap : Text.NoWrap
                maximumLineCount: tile.poster ? 3 : 1
                elide: Text.ElideRight
                text: tile.label
                color: tile.focused ? Theme.text : Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: tile.poster || tile._compact ? Theme.fontSmall : Theme.fontLabel
                font.weight: tile.focused ? Font.DemiBold : Font.Normal
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: text.length > 0 && !tile._compact
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: tile.available ? tile.subtitle : qsTr("Unavailable")
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSmall
            }
        }
    }

    // Favorite star (top right), outside the greyed card so it stays legible.
    Rectangle {
        objectName: "favoriteBadge"
        visible: tile.favorite
        anchors { right: parent.right; top: parent.top; margins: Theme.spacingS * Math.min(1, tile._k) }
        width: Theme.px(40) * Math.max(0.75, Math.min(1, tile._k))
        height: width
        radius: width / 2
        color: Theme.background
        opacity: 0.9
        Icon {
            anchors.centerIn: parent
            width: parent.width * 0.7
            height: width
            name: "favorites"
            color: "#ffc94d"
        }
    }

    // Corner label (top left), e.g. "Kids" on a profile tile.
    Rectangle {
        visible: tile.badge.length > 0
        anchors { left: parent.left; top: parent.top; margins: Theme.spacingS }
        width: badgeText.implicitWidth + 2 * Theme.spacingS
        height: badgeText.implicitHeight + Theme.px(4)
        radius: height / 2
        color: tile.tint
        Text {
            textFormat: Text.PlainText
            id: badgeText
            anchors.centerIn: parent
            text: tile.badge
            color: Theme.accentText
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSmall
            font.weight: Font.Bold
        }
    }

    PointerArea {
        anchors.fill: parent
        onHovered: tile.hovered()
        onTapped: (button) => tile.clicked(button)
    }
}
