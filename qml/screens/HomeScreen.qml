import QtQuick
import MightyOS.Core
import MightyOS.UI
import "../mock/MockData.js" as Mock

// Home: category bar (row 0) and the focused category's content (row 1).
//
//   [Favorites] [Emulators] [Games] [Media] [App Store] [Settings]   row 0
//   Emulators - Retro and modern consoles                           (info)
//   [NES] [SNES] [N64] [PS1] [PS2] ...                               row 1
//
// Up/Down moves between the rows, Left/Right within one, LB/RB (PgUp/PgDn)
// switches category from either row, Back in row 1 returns to the bar.
// Accept on a category opens its screen; on a content tile it opens or
// launches that tile. Each category remembers its content position, and
// those positions are part of the saved path (extra.positions).
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

    // Info area above the content row. Only re-evaluates when the focused
    // item changes, not per frame.
    readonly property var _category: focusRow === 0 && focusedItem ? Mock.category(focusedItem.itemId) : null
    readonly property string _infoTitle: focusedItem ? focusedItem.label : ""
    readonly property string _infoText: _category ? _category.blurb
                                        : (focusedItem ? focusedItem.subtitle : "")

    // Rebuilt only when the row or the accept label actually changes.
    prompts: {
        const list = [{ action: InputRouter.Accept, label: focusedAcceptLabel }]
        if (focusRow === 1)
            list.push({ action: InputRouter.Back, label: qsTr("Categories") })
        list.push({ action: InputRouter.Options, label: qsTr("Options") },
                  { action: InputRouter.NextCategory, label: qsTr("Category"),
                    pad: "LB RB", key: qsTr("PgUp PgDn") })
        return list
    }

    onFocusRowChanged: {
        _updateFocused()
        navStateChanged()
    }

    function _updateFocused() {
        let item = null
        if (focusRow === 0) {
            if (bar.currentIndex >= 0 && bar.currentIndex < bar.count)
                item = categoryModel.get(bar.currentIndex)
        } else if (content.currentIndex >= 0 && content.currentIndex < content.count) {
            item = content.model.get(content.currentIndex)
        }
        if (item !== focusedItem)
            focusedItem = item
    }

    function _currentPosition(): var {
        return { index: content.currentIndex, key: content.keyAt(content.currentIndex) }
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
        if (content.count === 0 && focusRow === 1)
            focusRow = 0
        _updateFocused()
    }

    function handleAction(action: int, arg: string): bool {
        switch (action) {
        case InputRouter.Navigate:
            if (arg === "down" && focusRow === 0) {
                if (content.count > 0) {
                    focusRow = 1
                    SoundFx.play(SoundFx.Move)
                }
                return true
            }
            if (arg === "up" && focusRow === 1) {
                focusRow = 0
                SoundFx.play(SoundFx.Move)
                return true
            }
            if (arg === "up" || arg === "down")
                return true   // nothing above the bar / below the content
            if (focusRow === 0)
                bar.moveFocus(arg)
            else
                content.moveFocus(arg)
            return true
        case InputRouter.Accept:
            if (focusRow === 0)
                bar.activateCurrent()
            else
                content.activateCurrent()
            return true
        case InputRouter.Back:
            if (focusRow === 1) {
                focusRow = 0
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
        const view = focusRow === 0 ? bar : content
        // No side effects: called on every persist.
        const positions = Object.assign({}, _positions)
        if (_shownCategory.length > 0 && content.currentIndex >= 0 && !_switching)
            positions[_shownCategory] = _currentPosition()
        return {
            row: focusRow,
            index: Math.max(0, view.currentIndex),
            key: view.keyAt(view.currentIndex),
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
        } else {
            focusRow = 0
        }
    }

    function wheel(steps: int) {
        if (focusRow === 0)
            bar.wheelStep(steps)
        else
            content.wheelStep(steps)
    }

    ListModel { id: categoryModel }
    // One model per category, built on first use. Swapping models (rather
    // than clear() + append() on a shared one) never pulls rows out from
    // under delegates the row is still incubating.
    property var _contentModels: ({})
    Component { id: contentModelComponent; ListModel {} }

    function _contentModelFor(id: string): var {
        let model = _contentModels[id]
        if (!model) {
            model = contentModelComponent.createObject(home) as ListModel
            model.append(Mock.contentRows(id))
            _contentModels[id] = model
        }
        return model
    }

    Component.onCompleted: {
        categoryModel.append(Mock.categoryRows())
        bar.currentIndex = 0
        _showCategory()
    }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        Item {
            width: parent.width
            height: bar.height + 2 * Theme.overflowFor(Theme.pillHeight)

            CategoryBar {
                id: bar
                anchors.verticalCenter: parent.verticalCenter
                model: categoryModel
                active: home.focusShown && home.focusRow === 0
                onCurrentIndexChanged: {
                    home._showCategory()
                    home._updateFocused()
                    home.navStateChanged()
                }
                onFocusRequested: home.focusRow = 0
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
                anchors.verticalCenter: parent.verticalCenter
                active: home.focusShown && home.focusRow === 1
                onCurrentIndexChanged: {
                    if (home._switching)
                        return
                    home._updateFocused()
                    home.navStateChanged()
                }
                onFocusRequested: home.focusRow = 1
                onActivated: (index) => home.activateItem(content.model.get(index))
            }
        }
    }
}
