import QtQuick
import MightyOS.Core
import MightyOS.UI
import "../Catalog.js" as Catalog

// One TileRow for App Store / Settings: placeholders until M7 / M8, except
// Settings > Profiles (the profile picker), All apps, Media and Artwork.
// (Media has its own MediaScreen since M5; an old saved path to
// category/media is refused and the stack falls back to Home.)
//
// params: { category: "store" | "settings" }
NavScreen {
    id: categoryScreen

    readonly property var _category: Catalog.category(params.category ?? "")
    readonly property var _milestone: ({ store: "M7", settings: "M8" })

    title: _category ? _category.title : ""
    valid: _category !== null && _milestone[params.category] !== undefined
           && !Library.restricted   // kids: no store / settings

    prompts: {
        const list = [
            { action: InputRouter.Accept, label: focusedAcceptLabel },
            { action: InputRouter.Back, label: qsTr("Back") }
        ]
        if (focusedCanFavorite)
            list.push({ action: InputRouter.Favorite, label: qsTr("Favorite") })
        list.push({ action: InputRouter.Options, label: qsTr("Options") })
        return list
    }

    function _updateFocused() {
        focusedItem = row.currentEntry()
    }

    function handleAction(action: int, arg: string): bool {
        switch (action) {
        case InputRouter.Navigate:
            row.moveFocus(arg)
            return true
        case InputRouter.Accept:
            row.activateCurrent()
            return true
        }
        return false
    }

    function saveState(): var {
        return { row: 0, index: row.savedIndex(), key: row.savedKey(), extra: {} }
    }

    function restoreState(state: var) {
        row.restoreFocus(state.index, state.key)
    }

    function wheel(steps: int) {
        row.wheelStep(steps)
    }

    TileListModel { id: sectionModel }

    Component.onCompleted: {
        if (!valid)
            return
        sectionModel.setRows(Catalog.sectionRows(params.category))
        row.model = sectionModel
        row.currentIndex = 0
        _updateFocused()
    }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        InfoHeader {
            width: parent.width
            heading: categoryScreen.focusedItem ? categoryScreen.focusedItem.title : categoryScreen.title
            detail: qsTr("Placeholder. The full %1 screen arrives in %2.")
                    .arg(categoryScreen.title).arg(categoryScreen._milestone[categoryScreen.params.category] ?? "")
        }

        Item {
            width: parent.width
            height: row.height + 2 * Theme.focusOverflow

            TileRow {
                id: row
                anchors.verticalCenter: parent.verticalCenter
                active: categoryScreen.focusShown
                onCurrentDataChanged: categoryScreen._updateFocused()
                onCurrentIndexChanged: {
                    categoryScreen._updateFocused()
                    categoryScreen.navStateChanged()
                }
                onCurrentItemChanged: categoryScreen._updateFocused()
                onActivated: (index) => categoryScreen.activateItem(row.model.get(index))
            }
        }
    }
}
