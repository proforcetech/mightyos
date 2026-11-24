import QtQuick
import MightyOS.Core
import MightyOS.UI

// Settings > All apps: every application AppRegistry found (.desktop files
// from the XDG and Flatpak dirs, MightyOS manifests), sorted by name, as a
// grid. Accept launches; X / Insert toggles a favorite.
//
// params: {}
NavScreen {
    id: apps

    valid: !Library.restricted   // kids profiles: no app list / settings
    title: qsTr("All apps")

    prompts: [
        { action: InputRouter.Accept, label: qsTr("Launch") },
        { action: InputRouter.Back, label: qsTr("Back") },
        { action: InputRouter.Favorite, label: qsTr("Favorite") },
        { action: InputRouter.Options, label: qsTr("Options") },
        { action: InputRouter.PageDown, label: qsTr("Page"),
                    actions: [InputRouter.PageUp, InputRouter.PageDown] }
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
                grid.bump("down")   // nothing at or after that letter
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
        grid.currentIndex = 0
        _updateFocused()
    }

    InfoHeader {
        id: header
        anchors { left: parent.left; right: parent.right; top: parent.top }
        heading: apps.focusedItem ? apps.focusedItem.title : apps.title
        detail: {
            if (grid.count === 0)
                return AppRegistry.ready ? qsTr("No apps found") : qsTr("Looking for apps…")
            const position = qsTr("%1 of %2").arg(grid.currentIndex + 1).arg(grid.count)
            const sub = apps.focusedItem ? apps.focusedItem.subtitle : ""
            return sub.length > 0 ? sub + "  ·  " + position : position
        }
    }

    TileGrid {
        id: grid
        objectName: "appGrid"
        keepPendingKey: true
        anchors { top: header.bottom; bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
        anchors.topMargin: Theme.spacingS
        availableWidth: apps.width
        model: AppRegistry.apps
        active: apps.focusShown
        onCurrentDataChanged: apps._updateFocused()
        onCurrentIndexChanged: {
            apps._updateFocused()
            apps.navStateChanged()
        }
        onCurrentItemChanged: apps._updateFocused()
        onCountChanged: apps._updateFocused()
        onActivated: (index) => apps.activateItem(AppRegistry.apps.get(index))
    }
}
