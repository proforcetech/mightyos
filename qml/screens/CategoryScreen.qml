import QtQuick
import MightyOS.Core
import MightyOS.UI
import "../Catalog.js" as Catalog

// One TileRow for Media / App Store / Settings. Media lists the media apps
// AppRegistry found (.desktop files and manifests); App Store and Settings
// are placeholders until M7 / M8, except Settings > Profiles (the profile
// picker) and Settings > All apps.
//
// params: { category: "media" | "store" | "settings" }
NavScreen {
    id: categoryScreen

    readonly property var _category: Catalog.category(params.category ?? "")
    readonly property var _milestone: ({ media: "M5", store: "M7", settings: "M8" })
    readonly property bool _isMedia: params.category === "media"

    title: _category ? _category.title : ""
    valid: _category !== null && _milestone[params.category] !== undefined
           && !(Library.restricted && params.category !== "media")   // kids: no store / settings

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
        if (!_isMedia)
            sectionModel.setRows(Catalog.sectionRows(params.category))
        row.model = _isMedia ? AppRegistry.media : sectionModel
        row.currentIndex = 0
        _updateFocused()
    }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        InfoHeader {
            width: parent.width
            heading: categoryScreen.focusedItem ? categoryScreen.focusedItem.title : categoryScreen.title
            detail: {
                if (categoryScreen._isMedia) {
                    if (row.count === 0)
                        return qsTr("No media apps found. Kodi, Jellyfin, Plex and friends show up here once installed.")
                    return categoryScreen.focusedItem && categoryScreen.focusedItem.subtitle.length > 0
                        ? categoryScreen.focusedItem.subtitle
                        : qsTr("Installed media apps. Streaming and web-app tiles arrive in M5.")
                }
                return qsTr("Placeholder. The full %1 screen arrives in %2.")
                    .arg(categoryScreen.title).arg(categoryScreen._milestone[categoryScreen.params.category] ?? "")
            }
        }

        Item {
            width: parent.width
            height: row.height + 2 * Theme.focusOverflow

            TileRow {
                id: row
                anchors.verticalCenter: parent.verticalCenter
                active: categoryScreen.focusShown
                keepPendingKey: categoryScreen._isMedia
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
