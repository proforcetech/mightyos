import QtQuick
import QtTest
import MightyOS.Core
import MightyOS.UI

// TileGrid with 38 items in 4 columns (last row partial: 36, 37).
Item {
    id: root
    width: 1200
    height: 800

    TileListModel { id: gridModel }

    TileGrid {
        id: grid
        // Exactly 4 columns and 2 visible rows at Theme scale 1.
        availableWidth: 4 * cellWidth + 2 * overflow + 10
        height: 2 * cellHeight + 2 * overflow
        model: gridModel
    }

    SignalSpy { id: activatedSpy; target: grid; signalName: "activated" }
    SignalSpy { id: focusSpy; target: grid; signalName: "focusRequested" }

    TestCase {
        name: "TileGrid"
        when: windowShown

        function init() {
            InputRouter.blocked = false
            InputRouter.muteFor(0)
            gridModel.clear()
            for (let i = 0; i < 38; ++i) {
                const letter = String.fromCharCode(65 + Math.floor(i / 2))   // A A B B C C ...
                gridModel.appendRow({ key: "g" + i, title: letter + " game " + i, sortTitle: letter + " game " + i,
                                   icon: "cartridge", tint: "#9b8cff" })
            }
            grid.currentIndex = 0
            grid.positionViewAtBeginning()
            tryCompare(grid, "bumping", false)
            activatedSpy.clear()
            focusSpy.clear()
        }

        function test_geometry() {
            compare(grid.columns, 4)
            compare(grid.visibleRows, 2)
            compare(grid.rowCount, 10)
        }

        function test_noWrapAtRowEnds() {
            grid.currentIndex = 3
            verify(!grid.moveFocus("right"))
            compare(grid.currentIndex, 3)
            verify(grid.bumping)
            tryCompare(grid, "bumping", false)
            grid.currentIndex = 4
            verify(!grid.moveFocus("left"))
            compare(grid.currentIndex, 4)
            grid.currentIndex = 37
            verify(!grid.moveFocus("right"))          // last item, mid-row
            compare(grid.currentIndex, 37)
        }

        function test_upFromFirstRowIsLeftToTheScreen() {
            grid.currentIndex = 2
            verify(!grid.moveFocus("up"))
            compare(grid.currentIndex, 2)
            verify(!grid.bumping)
        }

        function test_downIntoPartialLastRow() {
            grid.currentIndex = 3
            verify(grid.moveFocus("down"))
            compare(grid.currentIndex, 7)
            grid.currentIndex = 35                     // row 8, col 3; nothing below
            verify(grid.moveFocus("down"))
            compare(grid.currentIndex, 37)             // lands on the last tile
            verify(!grid.moveFocus("down"))
            verify(grid.bumping)
            compare(grid.currentIndex, 37)
        }

        function test_paging() {
            verify(grid.page(1))
            compare(grid.currentIndex, 8)              // 2 visible rows x 4 columns
            grid.currentIndex = 34                     // row 8, col 2
            verify(grid.page(1))
            compare(grid.currentIndex, 37)             // clamped into the last row
            verify(!grid.page(1))
            verify(grid.bumping)
            tryCompare(grid, "bumping", false)
            grid.currentIndex = 5
            verify(grid.page(-1))
            compare(grid.currentIndex, 1)              // first row, same column
            verify(!grid.page(-1))
            verify(grid.bumping)
        }

        function test_letterJump() {
            verify(grid.jumpToLetter("C"))
            compare(grid.currentIndex, 4)
            verify(!grid.jumpToLetter("c"))            // already there
            verify(grid.jumpToLetter("s"))
            compare(grid.currentIndex, 36)
            verify(!grid.jumpToLetter("Z"))            // nothing at or after Z
            compare(grid.currentIndex, 36)
        }

        function test_clickActivatesOwnTile() {
            grid.active = false
            const tile = grid.itemAtIndex(6)
            verify(tile)
            mouseClick(tile)
            compare(activatedSpy.count, 1)
            compare(activatedSpy.signalArguments[0][0], 6)
            compare(grid.currentIndex, 6)
            verify(focusSpy.count > 0)
            grid.active = true
        }

        function test_hover() {
            const tile = grid.itemAtIndex(5)
            mouseMove(tile, 15, 15)
            mouseMove(tile, 60, 80)
            compare(grid.currentIndex, 5)
            verify(focusSpy.count > 0)
        }

        function test_wheelMovesByRows() {
            const tile = grid.itemAtIndex(1)
            mouseWheel(tile, 10, 10, 0, -120)
            compare(grid.currentIndex, 4)
            mouseWheel(tile, 10, 10, 0, 120)
            compare(grid.currentIndex, 0)
        }

        function test_restoreFocus() {
            grid.restoreFocus(0, "g20")
            compare(grid.currentIndex, 20)
            grid.restoreFocus(500, "")
            compare(grid.currentIndex, 37)
        }
    }
}
