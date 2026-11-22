import QtQuick
import MightyOS.Core
import MightyOS.UI
import "../mock/MockData.js" as Mock

// Placeholder screen for Media / App Store / Settings: one TileRow with the
// category's mock content. The real screens arrive in M5 (Media), M7
// (App Store) and M8 (Settings).
//
// params: { category: "media" | "store" | "settings" }
NavScreen {
    id: categoryScreen

    readonly property var _category: Mock.category(params.category ?? "")
    readonly property var _milestone: ({ media: "M5", store: "M7", settings: "M8" })

    title: _category ? _category.label : ""
    valid: _category !== null && _milestone[params.category] !== undefined

    prompts: [
        { action: InputRouter.Accept, label: focusedAcceptLabel },
        { action: InputRouter.Back, label: qsTr("Back") },
        { action: InputRouter.Options, label: qsTr("Options") }
    ]

    function _updateFocused() {
        focusedItem = row.currentIndex >= 0 && row.currentIndex < row.count ? itemModel.get(row.currentIndex) : null
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
        return { row: 0, index: Math.max(0, row.currentIndex), key: row.keyAt(row.currentIndex), extra: {} }
    }

    function restoreState(state: var) {
        row.restoreFocus(state.index, state.key)
    }

    function wheel(steps: int) {
        row.wheelStep(steps)
    }

    ListModel { id: itemModel }

    Component.onCompleted: {
        if (!valid)
            return
        itemModel.append(Mock.contentRows(params.category))
        row.currentIndex = 0
        _updateFocused()
    }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        InfoHeader {
            width: parent.width
            heading: categoryScreen.title
            detail: qsTr("Placeholder. The full %1 screen arrives in %2.")
                    .arg(categoryScreen.title).arg(categoryScreen._milestone[categoryScreen.params.category] ?? "")
        }

        Item {
            width: parent.width
            height: row.height + 2 * Theme.focusOverflow

            TileRow {
                id: row
                anchors.verticalCenter: parent.verticalCenter
                model: itemModel
                active: categoryScreen.focusShown
                onCurrentIndexChanged: {
                    categoryScreen._updateFocused()
                    categoryScreen.navStateChanged()
                }
                onActivated: (index) => categoryScreen.activateItem(itemModel.get(index))
            }
        }
    }
}
