import QtQuick
import MightyOS.Core
import MightyOS.UI
import "../Catalog.js" as Catalog

// Home: category bar (row 0), the focused category's content (row 1) and,
// on Favorites only, a small "Recent" row (row 2).
//
//   [Favorites] [Emulators] [Games] [Media] [App Store] [Settings]   row 0
//   Emulators - Retro and modern consoles                           (info)
//   [NES] [SNES] [N64] [PS1] [PS2] ...                               row 1
//   Recent  [game] [app] ...          (Favorites only, if non-empty) row 2
//
// Up/Down moves between the rows (empty rows are skipped), Left/Right
// within one, LB/RB (PgUp/PgDn) switches category from any row, Back in
// row 1/2 returns to the bar. Accept on a category opens its screen; on a
// content tile it opens or launches that tile. Each category remembers its
// content position, and those positions are part of the saved path
// (extra.positions).
//
// Content models are the C++ ones: Favorites.model, Library.systems(),
// Media.home (media apps, web apps, Moonlight hosts), static TileListModels
// for Store/Settings.
NavScreen {
    id: home

    title: qsTr("MightyOS")

    property int focusRow: 0
    // categoryId -> { index, key } of the content focus, so switching
    // categories (and restarting) keeps your place in each one.
    property var _positions: ({})
    property string _shownCategory: ""
    // True while the content row swaps models (its index passes through
    // a transient 0 that must not be remembered or persisted).
    property bool _switching: false
    // The user is working in the content rows: LB/RB through a category
    // with no content drops to the bar, the next one with content brings
    // the focus back down.
    property bool _preferContent: false

    readonly property bool _recentShown: _shownCategory === "favorites" && recent.count > 0

    // Info area above the content row. Only re-evaluates when the focused
    // item changes, not per frame.
    readonly property var _category: focusRow === 0 && focusedItem ? Catalog.category(focusedItem.key) : null
    readonly property string _infoTitle: focusedItem ? focusedItem.title : ""
    readonly property string _infoText: {
        if (_category) {
            if (_category.key === "favorites" && Favorites.count === 0)
                return qsTr("Press X (Insert) on any game, app or system to pin it here")
            if (_category.key === "media" && Media.home.count === 0)
                return Library.restricted ? qsTr("Nothing is allowed for this profile yet. Ask a parent.")
                                          : qsTr("No media apps or streaming tiles yet")
            return _category.blurb
        }
        if (!focusedItem)
            return ""
        return focusedItem.available === false ? qsTr("Unavailable") : focusedItem.subtitle
    }

    // Rebuilt only when the row, the accept label or "can favorite" changes.
    prompts: {
        const list = [{ action: InputRouter.Accept, label: focusedAcceptLabel }]
        if (focusRow > 0)
            list.push({ action: InputRouter.Back, label: qsTr("Categories") })
        if (focusedCanFavorite)
            list.push({ action: InputRouter.Favorite, label: qsTr("Favorite") })
        list.push({ action: InputRouter.Options, label: qsTr("Options") },
                  { action: InputRouter.NextCategory, label: qsTr("Category"),
                    actions: [InputRouter.PrevCategory, InputRouter.NextCategory] })
        return list
    }

    onFocusRowChanged: {
        _updateFocused()
        navStateChanged()
    }

    function _view(rowIndex: int): var {
        return rowIndex === 0 ? bar : rowIndex === 1 ? content : recent
    }

    function _rowUsable(rowIndex: int): bool {
        if (rowIndex === 1)
            return content.count > 0
        if (rowIndex === 2)
            return _recentShown
        return rowIndex === 0
    }

    function _updateFocused() {
        const item = _view(focusRow).currentEntry()
        if (JSON.stringify(item) !== JSON.stringify(focusedItem))
            focusedItem = item
    }

    function focusedInFavoritesList(): bool {
        return focusRow === 1 && _shownCategory === "favorites"
    }

    function extraOptions(): var {
        return [{ id: "profiles", label: qsTr("Switch profile") }]
    }

    function extraOptionChosen(id: string): bool {
        if (id !== "profiles")
            return false
        profilePickerRequested()
        return true
    }

    function _currentPosition(): var {
        return { index: content.savedIndex(), key: content.savedKey() }
    }

    function _rememberPosition() {
        if (_shownCategory.length > 0 && content.currentIndex >= 0)
            _positions[_shownCategory] = _currentPosition()
    }

    function _showCategory() {
        const id = bar.keyAt(bar.currentIndex)
        if (id === _shownCategory)
            return
        _rememberPosition()
        _switching = true
        _shownCategory = id
        content.model = _contentModelFor(id)
        const position = _positions[id]
        content.restoreFocus(position ? position.index : 0, position ? position.key : "")
        _switching = false
        if (!_rowUsable(focusRow))
            focusRow = 0
        if (focusRow === 0 && _preferContent && content.count > 0)
            focusRow = 1
        _updateFocused()
    }

    function _moveVertical(direction: string): bool {
        const step = direction === "down" ? 1 : -1
        for (let r = focusRow + step; r >= 0 && r <= 2; r += step) {
            if (_rowUsable(r)) {
                focusRow = r
                _preferContent = r > 0
                SoundFx.play(SoundFx.Move)
                return true
            }
        }
        return false
    }

    function handleAction(action: int, arg: string): bool {
        switch (action) {
        case InputRouter.Navigate:
            if (arg === "up" || arg === "down") {
                _moveVertical(arg)   // nothing above the bar / below the last row
                return true
            }
            _view(focusRow).moveFocus(arg)
            return true
        case InputRouter.Accept:
            _view(focusRow).activateCurrent()
            return true
        case InputRouter.Back:
            if (focusRow > 0) {
                focusRow = 0
                _preferContent = false
                SoundFx.play(SoundFx.Back)
                return true
            }
            return false
        case InputRouter.PrevCategory:
            bar.moveFocus("left")
            return true
        case InputRouter.NextCategory:
            bar.moveFocus("right")
            return true
        }
        return false
    }

    function saveState(): var {
        const view = _view(focusRow)
        // No side effects: called on every persist.
        const positions = Object.assign({}, _positions)
        if (_shownCategory.length > 0 && content.currentIndex >= 0 && !_switching)
            positions[_shownCategory] = _currentPosition()
        return {
            row: focusRow,
            index: view.savedIndex(),
            key: view.savedKey(),
            extra: { category: _shownCategory, positions: positions }
        }
    }

    function restoreState(state: var) {
        // Saved per-category positions; anything malformed is ignored.
        const saved = state.extra && state.extra.positions ? state.extra.positions : {}
        const positions = {}
        for (const id in saved) {
            const p = saved[id]
            if (p && typeof p.index === "number" && p.index >= 0)
                positions[id] = { index: Math.floor(p.index), key: typeof p.key === "string" ? p.key : "" }
        }
        _positions = positions
        _shownCategory = ""   // force the content row to reload with them
        const category = state.extra && state.extra.category ? state.extra.category
                       : (state.row === 0 ? state.key : "")
        bar.restoreFocus(state.row === 0 ? state.index : 0, category)
        _showCategory()
        if (state.row === 1 && content.count > 0) {
            content.restoreFocus(state.index, state.key)
            focusRow = 1
        } else if (state.row === 2 && _recentShown) {
            recent.restoreFocus(state.index, state.key)
            focusRow = 2
        } else {
            focusRow = 0
        }
        _preferContent = focusRow > 0
        _updateFocused()
    }

    function wheel(steps: int) {
        _view(focusRow).wheelStep(steps)
    }

    // Guide / Ctrl+Home: the Favorites category on the bar. The other
    // categories keep their remembered positions.
    function goHome() {
        _preferContent = false
        focusRow = 0
        bar.restoreFocus(0, "favorites")   // onCurrentIndexChanged shows it
        _showCategory()
        _updateFocused()
        navStateChanged()
    }

    TileListModel { id: categoryModel }
    TileListModel { id: storeModel }
    TileListModel { id: settingsModel }

    function _contentModelFor(id: string): var {
        switch (id) {
        case "favorites": return Favorites.model
        case "emulators":
        case "games":     return Library.systems(id)
        case "media":     return Media.home
        case "store":     return storeModel
        case "settings":  return settingsModel
        }
        return null
    }

    Component.onCompleted: {
        categoryModel.setRows(Catalog.categoryRows(Library.restricted))
        storeModel.setRows(Catalog.sectionRows("store"))
        settingsModel.setRows(Catalog.sectionRows("settings"))
        bar.currentIndex = 0
        _showCategory()
    }

    Connections {
        target: Library
        function onRestrictedChanged() {
            categoryModel.setRows(Catalog.categoryRows(Library.restricted))
        }
    }

    // The Favorites row or Recent row can empty out under the focus
    // (removing the last favorite, a re-scan): fall back to a usable row.
    Connections {
        target: content
        function onCountChanged() {
            if (!home._switching && home.focusRow === 1 && content.count === 0)
                home.focusRow = home._recentShown ? 2 : 0
            home._updateFocused()
        }
    }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        Item {
            width: parent.width
            height: bar.height + 2 * Theme.overflowFor(Theme.pillHeight)

            CategoryBar {
                id: bar
                objectName: "categoryBar"
                anchors.verticalCenter: parent.verticalCenter
                model: categoryModel
                active: home.focusShown && home.focusRow === 0
                onCurrentIndexChanged: {
                    home._showCategory()
                    home._updateFocused()
                    home.navStateChanged()
                }
                onFocusRequested: {
                    home.focusRow = 0
                    home._preferContent = false
                }
                onActivated: (index) => home.activateItem(categoryModel.get(index))
            }
        }

        InfoHeader {
            width: parent.width
            heading: home._infoTitle
            detail: home._infoText
        }

        Item {
            width: parent.width
            height: content.height + 2 * Theme.focusOverflow

            TileRow {
                id: content
                objectName: "homeContent"
                keepPendingKey: home._shownCategory !== "store" && home._shownCategory !== "settings"
                sourceReady: home._shownCategory === "media" ? AppRegistry.ready && Media.ready
                           : home._shownCategory === "favorites" ? AppRegistry.ready && Library.scanned && Media.ready
                           : Library.scanned
                anchors.verticalCenter: parent.verticalCenter
                active: home.focusShown && home.focusRow === 1
                onCurrentIndexChanged: {
                    if (home._switching)
                        return
                    home._updateFocused()
                    home.navStateChanged()
                }
                onCurrentItemChanged: {
                    if (!home._switching)
                        home._updateFocused()
                }
                onCurrentDataChanged: home._updateFocused()
                onFocusRequested: {
                    home.focusRow = 1
                    home._preferContent = true
                }
                onActivated: (index) => home.activateItem(content.model.get(index))
            }
        }

        Text {
            textFormat: Text.PlainText
            visible: home._recentShown
            text: qsTr("Recent")
            color: Theme.textDim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
        }

        Item {
            visible: home._recentShown
            width: parent.width
            height: recent.height + 2 * Theme.overflowFor(recent.tileHeight)

            TileRow {
                id: recent
                objectName: "homeRecent"
                keepPendingKey: true
                sourceReady: AppRegistry.ready && Library.scanned && Media.ready
                anchors.verticalCenter: parent.verticalCenter
                model: History.recent
                tileWidth: Theme.recentTileSize
                tileHeight: Theme.recentTileSize
                spacing: Theme.spacingS
                active: home.focusShown && home.focusRow === 2
                onCurrentIndexChanged: {
                    home._updateFocused()
                    home.navStateChanged()
                }
                onCurrentItemChanged: home._updateFocused()
                onCurrentDataChanged: home._updateFocused()
                onFocusRequested: {
                    home.focusRow = 2
                    home._preferContent = true
                }
                onActivated: (index) => home.activateItem(History.recent.get(index))
                onCountChanged: {
                    if (count === 0 && home.focusRow === 2)
                        home.focusRow = content.count > 0 ? 1 : 0
                }
            }
        }
    }
}
