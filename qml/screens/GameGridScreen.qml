import QtQuick
import MightyOS.Core
import MightyOS.UI
import "../mock/MockData.js" as Mock

// The games of one system (or the Favorites collection) as a poster grid.
// LT/RT (Home/End) page through it; Accept launches the focused game.
// LB/RB (PgUp/PgDn) step to the previous/next system of the same category
// (SNES -> N64), replacing this grid in place: Back still returns to the
// system list, whose focus follows. At the first/last system they bump.
// In the Favorites collection LB/RB keep their global meaning.
//
// params: { system: "<systemId>" } or { collection: "favorites" }
NavScreen {
    id: games

    readonly property var _rows: Mock.gridRows(params)

    title: Mock.gridTitle(params)
    valid: _rows !== null

    readonly property bool _isSystem: !!(params && params.system)

    // Constant for the lifetime of the screen (no per-move rebuild).
    prompts: {
        const list = [
            { action: InputRouter.Accept, label: qsTr("Play") },
            { action: InputRouter.Back, label: qsTr("Back") },
            { action: InputRouter.Options, label: qsTr("Options") },
            { action: InputRouter.PageDown, label: qsTr("Page"), pad: "LT RT", key: qsTr("Home End") }
        ]
        if (_isSystem)
            list.push({ action: InputRouter.NextCategory, label: qsTr("System"),
                        pad: "LB RB", key: qsTr("PgUp PgDn") })
        return list
    }

    function _updateFocused() {
        focusedItem = grid.currentIndex >= 0 && grid.currentIndex < grid.count ? gameModel.get(grid.currentIndex) : null
    }

    function _stepSystem(delta: int) {
        const next = Mock.adjacentSystem(params.system, delta)
        if (!next) {
            grid.bump(delta < 0 ? "left" : "right")
            return
        }
        SoundFx.play(SoundFx.Move)
        replaceRequested("games", { system: next }, delta)
    }

    function handleAction(action: int, arg: string): bool {
        switch (action) {
        case InputRouter.Navigate:
            if (!grid.moveFocus(arg) && arg === "up" && grid.currentRow === 0)
                grid.bump("up")   // nothing focusable above the grid
            return true
        case InputRouter.Accept:
            grid.activateCurrent()
            return true
        case InputRouter.PageUp:
            grid.page(-1)
            return true
        case InputRouter.PageDown:
            grid.page(1)
            return true
        case InputRouter.PrevCategory:
        case InputRouter.NextCategory:
            if (!_isSystem)
                return false   // Favorites: global "back to Home + switch"
            _stepSystem(action === InputRouter.PrevCategory ? -1 : 1)
            return true
        }
        return false
    }

    function saveState(): var {
        return { row: 0, index: Math.max(0, grid.currentIndex), key: grid.keyAt(grid.currentIndex), extra: {} }
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

    ListModel { id: gameModel }

    Component.onCompleted: {
        if (!valid)
            return
        gameModel.append(_rows)
        grid.currentIndex = 0
        _updateFocused()
    }

    InfoHeader {
        id: header
        anchors { left: parent.left; right: parent.right; top: parent.top }
        heading: games.focusedItem ? games.focusedItem.label : games.title
        detail: {
            const row = games.focusedItem
            const position = grid.count > 0
                ? qsTr("%1 of %2").arg(grid.currentIndex + 1).arg(grid.count) : qsTr("No games")
            return row ? row.subtitle + "  ·  " + position : position
        }
    }

    TileGrid {
        id: grid
        anchors { top: header.bottom; bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
        anchors.topMargin: Theme.spacingS
        availableWidth: games.width
        model: gameModel
        active: games.focusShown
        onCurrentIndexChanged: {
            games._updateFocused()
            games.navStateChanged()
        }
        onActivated: (index) => games.activateItem(gameModel.get(index))
    }
}
