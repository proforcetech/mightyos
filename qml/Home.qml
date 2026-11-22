pragma ComponentBehavior: Bound
import QtQuick

// Home screen: category row, focused-category title and a placeholder
// content row. Receives named actions from Main via handleAction().
Item {
    id: home

    // index, model entry
    signal categoryActivated(int index, var entry)

    property alias currentIndex: categoryRow.currentIndex
    readonly property var currentEntry: categoryRow.currentIndex >= 0
        ? categories.get(categoryRow.currentIndex) : null

    // Returns true if the action was handled.
    function handleAction(action, arg) {
        switch (action) {
        case "navigate":
            categoryRow.moveFocus(arg)   // up/down: nothing focusable yet (M1)
            return true
        case "accept":
            categoryRow.activateCurrent()
            return true
        case "back":
            return false                  // top level: nothing to go back to
        }
        return false
    }

    // M0: hard-coded. Replaced by AppRegistry / category models in M1-M2.
    // `launchable` marks the two tiles that start the M0 test app.
    ListModel {
        id: categories
        ListElement { categoryId: "favorites"; label: qsTr("Favorites"); icon: "favorites"; tint: "#ffc94d"; launchable: false; blurb: qsTr("Your pinned games and apps") }
        ListElement { categoryId: "emulators"; label: qsTr("Emulators"); icon: "emulators"; tint: "#ff6b8b"; launchable: false; blurb: qsTr("Retro and modern consoles") }
        ListElement { categoryId: "games";     label: qsTr("Games");     icon: "games";     tint: "#7bd88f"; launchable: false; blurb: qsTr("Native and Steam games") }
        ListElement { categoryId: "media";     label: qsTr("Media");     icon: "media";     tint: "#4da3ff"; launchable: true;  blurb: qsTr("Movies, music and streaming") }
        ListElement { categoryId: "store";     label: qsTr("App Store"); icon: "store";     tint: "#b48cff"; launchable: false; blurb: qsTr("Install apps from Flathub") }
        ListElement { categoryId: "settings";  label: qsTr("Settings");  icon: "settings";  tint: "#a0aec0"; launchable: true;  blurb: qsTr("Display, audio, network and controllers") }
    }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        Item {
            width: parent.width
            height: categoryRow.height + 2 * Theme.focusOverflow

            TileRow {
                id: categoryRow
                anchors.centerIn: parent
                model: categories
                currentIndex: 0
                onActivated: (index) => home.categoryActivated(index, categories.get(index))
            }
        }

        Column {
            width: parent.width
            spacing: Theme.spacingS

            Text {
                id: title
                width: parent.width
                text: home.currentEntry ? home.currentEntry.label : ""
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontTitle
                font.weight: Font.Bold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: home.currentEntry ? home.currentEntry.blurb : ""
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
        }

        // Placeholder content row (not focusable in M0).
        Row {
            spacing: Theme.spacingM
            Repeater {
                model: 4
                delegate: Rectangle {
                    id: placeholder
                    required property int index
                    width: Theme.placeholderWidth
                    height: Theme.placeholderHeight
                    radius: Theme.radius
                    color: Theme.backgroundAlt
                    border.width: Math.max(1, Theme.px(2))
                    border.color: Theme.surface
                    Text {
                        anchors.centerIn: parent
                        text: placeholder.index === 0 ? qsTr("Content arrives in M1") : ""
                        color: Theme.textDim
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSmall
                    }
                }
            }
        }
    }
}
