import QtQuick
import MightyOS.Core
import MightyOS.UI

// Media > [Moonlight host]: the apps the host offers (as Moonlight last
// cached them), sorted by name, as a grid. Accept starts the stream
// (`moonlight stream -- <host> <app>`); X / Insert toggles a favorite.
//
//   GAMING-PC
//   Steam Big Picture · 2 of 14
//   [Desktop] [Steam Big Picture] [Cyberpunk 2077] ...
//
// params: { host: "moonlight:<host id>" }
NavScreen {
    id: hostScreen

    // Before Moonlight's settings were read (a saved path restored at
    // startup) the host isn't known yet: the screen stays (empty, "looking")
    // and binds its model once Media.ready turns true.
    readonly property TileListModel _model: Media.ready ? Media.streams(params.host ?? "") : null

    valid: !Media.ready || _model !== null
    title: Media.ready ? Media.hostName(params.host ?? "") : qsTr("Game streaming")

    prompts: [
        { action: InputRouter.Accept, label: qsTr("Play") },
        { action: InputRouter.Back, label: qsTr("Back") },
        { action: InputRouter.Favorite, label: qsTr("Favorite") },
        { action: InputRouter.Options, label: qsTr("Options") }
    ]

    function _updateFocused() {
        focusedItem = grid.currentEntry()
    }

    function handleAction(action: int, arg: string): bool {
        switch (action) {
        case InputRouter.Navigate:
            if (!grid.moveFocus(arg) && arg === "up" && grid.currentRow === 0)
                grid.bump("up")
            return true
        case InputRouter.Accept:
            grid.activateCurrent()
            return true
        case InputRouter.Letter:
            if (!grid.jumpToLetter(arg) && grid.model && grid.model.firstIndexForLetter(arg) !== grid.currentIndex)
                grid.bump("down")
            return true
        case InputRouter.PageUp:
            grid.page(-1)
            return true
        case InputRouter.PageDown:
            grid.page(1)
            return true
        }
        return false
    }

    function saveState(): var {
        return { row: 0, index: grid.savedIndex(), key: grid.savedKey(), extra: {} }
    }

    function restoreState(state: var) {
        grid.restoreFocus(state.index, state.key)
    }

    function wheel(steps: int) {
        const direction = steps > 0 ? "down" : "up"
        for (let i = 0; i < Math.abs(steps); ++i) {
            if (!grid.moveFocus(direction))
                break
        }
    }

    Component.onCompleted: {
        if (!valid)
            return
        grid.currentIndex = 0
        _updateFocused()
    }

    InfoHeader {
        id: header
        anchors { left: parent.left; right: parent.right; top: parent.top }
        heading: hostScreen.focusedItem ? hostScreen.focusedItem.title : hostScreen.title
        detail: {
            if (!Media.ready)
                return qsTr("Looking for Moonlight hosts…")
            if (!hostScreen._model)
                return qsTr("This host is no longer paired with Moonlight.")
            if (grid.count === 0)
                return qsTr("No apps known for this host yet. Open Moonlight once while the host is on to load them.")
            return qsTr("%1 of %2").arg(grid.currentIndex + 1).arg(grid.count) + "  ·  "
                   + qsTr("Hold the Guide button to end the stream")
        }
    }

    TileGrid {
        id: grid
        objectName: "streamGrid"
        keepPendingKey: true
        sourceReady: Media.ready
        anchors { top: header.bottom; bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
        anchors.topMargin: Theme.spacingS
        availableWidth: hostScreen.width
        model: hostScreen._model
        active: hostScreen.focusShown
        onCurrentDataChanged: hostScreen._updateFocused()
        onCurrentIndexChanged: {
            hostScreen._updateFocused()
            hostScreen.navStateChanged()
        }
        onCurrentItemChanged: hostScreen._updateFocused()
        onCountChanged: hostScreen._updateFocused()
        onActivated: (index) => hostScreen.activateItem(hostScreen._model.get(index))
    }
}
