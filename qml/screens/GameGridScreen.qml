import QtQuick
import MightyOS.Core
import MightyOS.UI

// The games of one system (Library.games()) or the Favorites collection
// (Favorites.model) as a poster grid. Accept launches the focused game (or
// opens a favorited system/app).
//
// A-Z jump (PLAN M4): LT/RT (keyboard Home/End, remote CH-/CH+) step to
// the previous/next letter group of the sorted list (first tile of "B",
// "C", ...; LT first goes to the start of the current group); typing a
// letter or digit on a keyboard jumps to the first title starting with it
// (or the next one after it). A large letter shows briefly where you
// landed. The Favorites collection is in your order, not sorted, so there
// LT/RT page instead.
// LB/RB (PgUp/PgDn) step to the previous/next system of the same category
// (SNES -> N64), replacing this grid in place: Back still returns to the
// system list, whose focus follows. At the first/last system they bump.
// In the Favorites collection LB/RB keep their global meaning, and the
// options menu can reorder (Move left/right) and remove favorites.
//
// params: { system: "<systemId>" } or { collection: "favorites" }
NavScreen {
    id: games

    readonly property bool _isFavorites: !!(params && params.collection === "favorites")
    readonly property bool _isSystem: !!(params && params.system)
    readonly property var _model: _isFavorites ? Favorites.model
                                 : _isSystem ? Library.games(params.system) : null

    title: _isFavorites ? qsTr("Favorites")
         : _isSystem ? (Library.system(params.system).title ?? "") : ""
    valid: _model !== null

    // Constant per screen except for the Favorite prompt (bool changes only).
    prompts: {
        const list = [
            { action: InputRouter.Accept, label: focusedAcceptLabel },
            { action: InputRouter.Back, label: qsTr("Back") }
        ]
        if (focusedCanFavorite)
            list.push({ action: InputRouter.Favorite, label: qsTr("Favorite") })
        list.push({ action: InputRouter.Options, label: qsTr("Options") },
                  { action: InputRouter.PageDown, label: _isSystem ? qsTr("A–Z") : qsTr("Page"),
                    actions: [InputRouter.PageUp, InputRouter.PageDown] })
        if (_isSystem)
            list.push({ action: InputRouter.NextCategory, label: qsTr("System"),
                        actions: [InputRouter.PrevCategory, InputRouter.NextCategory] })
        return list
    }

    function _updateFocused() {
        focusedItem = grid.currentEntry()
    }

    function focusedInFavoritesList(): bool {
        return _isFavorites
    }

    function _stepSystem(delta: int) {
        const next = Library.adjacentSystem(params.system, delta)
        if (next.length === 0) {
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
        case InputRouter.PageDown: {
            const delta = action === InputRouter.PageUp ? -1 : 1
            if (!_isSystem) {
                grid.page(delta)
                return true
            }
            const target = _model.letterStep(grid.currentIndex, delta)
            if (target < 0) {
                grid.bump(delta < 0 ? "up" : "down")
                return true
            }
            grid.jumpTo(target)
            _showLetter(_model.letterAt(target))
            return true
        }
        case InputRouter.Letter:
            if (!_isSystem || grid.count === 0)
                return false
            if (grid.jumpToLetter(arg) || _model.firstIndexForLetter(arg) === grid.currentIndex)
                _showLetter(_model.letterAt(grid.currentIndex))   // moved, or already there
            else
                grid.bump("down")   // nothing at or after that letter
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

    function _showLetter(letter: string) {
        letterBadge.text = letter
        letterBadge.opacity = 1
        letterFade.restart()
    }

    readonly property var _system: _isSystem ? Library.system(params.system) : ({})
    // What to tell someone looking at an empty system.
    readonly property string _emptyHint: {
        if (!_isSystem)
            return ""
        if (_system.kind === "steam")
            return qsTr("No installed Steam games found. Install games in Steam; they show up here.")
        const exts = (_system.extensions ?? []).slice(0, 4).map(e => "." + e).join(" ")
        const dir = (_system.romDirs ?? [])[0] ?? ""
        return Library.scanning ? qsTr("Looking for games…")
                                : qsTr("No games yet. Put %1 files in %2").arg(exts).arg(dir)
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
        heading: games.focusedItem ? games.focusedItem.title
               : (games._isFavorites ? qsTr("No favorites yet") : games.title)
        detail: {
            const row = games.focusedItem
            if (!row && games._isFavorites)
                return qsTr("Press X (Insert) on any game, app or system to pin it here")
            if (grid.count === 0 && games._isSystem)
                return games._emptyHint
            const position = grid.count > 0
                ? qsTr("%1 of %2").arg(grid.currentIndex + 1).arg(grid.count) : qsTr("No games")
            if (!row)
                return position
            const sub = row.available === false ? qsTr("Unavailable") : row.subtitle
            return sub.length > 0 ? sub + "  ·  " + position : position
        }
    }

    TileGrid {
        id: grid
        objectName: "gameGrid"
        anchors { top: header.bottom; bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
        anchors.topMargin: Theme.spacingS
        availableWidth: games.width
        // A saved game position survives until the library's first scan.
        keepPendingKey: true
        sourceReady: games._isFavorites ? AppRegistry.ready && Library.scanned : Library.scanned
        model: games._model
        active: games.focusShown
        onCurrentDataChanged: games._updateFocused()
        onCurrentIndexChanged: {
            games._updateFocused()
            games.navStateChanged()
        }
        onCurrentItemChanged: games._updateFocused()
        onCountChanged: games._updateFocused()
        onActivated: (index) => games.activateItem(games._model.get(index))
    }

    // Where an A-Z jump landed: a big letter that fades out.
    Rectangle {
        id: letterBadgeBox
        anchors.centerIn: grid
        width: Theme.posterWidth
        height: width
        radius: Theme.radius
        color: Theme.surfaceHigh
        opacity: letterBadge.opacity
        visible: opacity > 0
        Text {
            id: letterBadge
            objectName: "letterBadge"
            textFormat: Text.PlainText
            anchors.centerIn: parent
            opacity: 0
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: parent.height * 0.6
            font.weight: Font.Bold
        }
        NumberAnimation {
            id: letterFade
            target: letterBadge; property: "opacity"; from: 1; to: 0
            duration: 900; easing.type: Easing.InQuad
        }
    }
}
