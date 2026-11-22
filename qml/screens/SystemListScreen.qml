import QtQuick
import MightyOS.Core
import MightyOS.UI
import "../mock/MockData.js" as Mock

// The systems of one category: Emulators -> [NES, SNES, ...] and
// Games -> [Steam]. Steam is a system like any emulator (PLAN D12), so both
// use this screen and both open GameGrid.
//
// params: { category: "emulators" | "games" }
NavScreen {
    id: systems

    readonly property var _category: Mock.category(params.category ?? "")

    title: _category ? _category.label : ""
    valid: _category !== null && (params.category === "emulators" || params.category === "games")

    prompts: [
        { action: InputRouter.Accept, label: qsTr("Open") },
        { action: InputRouter.Back, label: qsTr("Back") },
        { action: InputRouter.Options, label: qsTr("Options") }
    ]

    function _updateFocused() {
        focusedItem = row.currentIndex >= 0 && row.currentIndex < row.count ? systemModel.get(row.currentIndex) : null
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
        return { row: 0, index: Math.max(0, row.currentIndex), key: row.keyAt(row.currentIndex), extra: {} }
    }

    function restoreState(state: var) {
        row.restoreFocus(state.index, state.key)
    }

    function wheel(steps: int) {
        row.wheelStep(steps)
    }

    ListModel { id: systemModel }

    Component.onCompleted: {
        if (!valid)
            return
        systemModel.append(Mock.systemRows(params.category))
        row.currentIndex = 0
        _updateFocused()
    }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        InfoHeader {
            width: parent.width
            heading: systems.focusedItem ? systems.focusedItem.label : systems.title
            detail: systems.focusedItem ? systems.focusedItem.subtitle : ""
        }

        Item {
            width: parent.width
            height: row.height + 2 * Theme.focusOverflow

            TileRow {
                id: row
                anchors.verticalCenter: parent.verticalCenter
                model: systemModel
                active: systems.focusShown
                onCurrentIndexChanged: {
                    systems._updateFocused()
                    systems.navStateChanged()
                }
                onActivated: (index) => systems.activateItem(systemModel.get(index))
            }
        }
    }
}
