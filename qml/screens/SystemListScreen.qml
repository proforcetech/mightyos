import QtQuick
import MightyOS.Core
import MightyOS.UI
import "../Catalog.js" as Catalog

// The systems of one category: Emulators -> [NES, SNES, ...] and
// Games -> [Steam]. Steam is a system like any emulator (PLAN D12), so both
// use this screen and both open GameGrid. Model: Library.systems(): only
// systems with games, then a "+N more" tile that shows the others too
// (Settings.showAllSystems; Options toggles it back).
//
// params: { category: "emulators" | "games" }
NavScreen {
    id: systems

    readonly property var _category: Catalog.category(params.category ?? "")
    readonly property var _model: valid ? Library.systems(params.category) : null

    title: _category ? _category.title : ""
    valid: _category !== null && (params.category === "emulators" || params.category === "games")

    prompts: {
        const list = [
            { action: InputRouter.Accept, label: qsTr("Open") },
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

    // GameGrid stepped to another system (LB/RB): focus follows it, so Back
    // lands on the system you were last looking at.
    function followChild(screenName: string, childParams: var) {
        if (screenName === "games" && childParams && childParams.system)
            row.restoreFocus(row.currentIndex, childParams.system)
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

    Component.onCompleted: {
        if (!valid)
            return
        row.currentIndex = 0
        _updateFocused()
    }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        InfoHeader {
            width: parent.width
            heading: systems.focusedItem ? systems.focusedItem.title : systems.title
            detail: systems.focusedItem ? systems.focusedItem.subtitle
                  : Library.restricted ? qsTr("Nothing is allowed for this profile yet. Ask a parent.")
                  : Library.scanning ? qsTr("Looking for games…") : ""
        }

        Item {
            width: parent.width
            height: row.height + 2 * Theme.focusOverflow

            TileRow {
                id: row
                anchors.verticalCenter: parent.verticalCenter
                model: systems._model
                keepPendingKey: true
                sourceReady: Library.scanned
                active: systems.focusShown
                onCurrentDataChanged: systems._updateFocused()
                onCurrentIndexChanged: {
                    systems._updateFocused()
                    systems.navStateChanged()
                }
                onActivated: (index) => systems.activateItem(systems._model.get(index))
            }
        }
    }
}
