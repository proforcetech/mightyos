import QtQuick
import QtTest
import MightyOS.Core
import MightyOS.UI

// TileRow: no wrap + bump at the edges, hover only after real pointer
// movement, clicks activate the clicked tile itself, right click asks for
// Options, input is ignored while InputRouter is muted, wheel steps focus.
Item {
    id: root
    width: 1600
    height: 700

    ListModel { id: rowModel }

    TileRow {
        id: row
        x: 50
        y: 100
        width: 1500
        model: rowModel
    }

    // A row long enough to scroll (about 4 of 15 tiles visible).
    ListModel { id: longModel }
    TileRow {
        id: longRow
        x: 50
        y: 400
        width: 1000
        model: longModel
        active: false
    }

    // Must rasterise: fails if the SVG image plugin is missing.
    Icon { id: probeIcon; x: 1500; y: 0; width: 64; height: 64; name: "settings" }

    SignalSpy { id: activatedSpy; target: row; signalName: "activated" }
    SignalSpy { id: focusSpy; target: row; signalName: "focusRequested" }
    SignalSpy { id: actionSpy; target: InputRouter; signalName: "triggered" }

    TestCase {
        name: "TileRow"
        when: windowShown

        function init() {
            InputRouter.blocked = false
            InputRouter.muteFor(0)
            rowModel.clear()
            for (let i = 0; i < 5; ++i)
                rowModel.append({ itemId: "item" + i, label: "Item " + i, icon: "games", tint: "#4da3ff" })
            row.currentIndex = 0
            row.active = true
            tryCompare(row, "bumping", false)
            activatedSpy.clear()
            focusSpy.clear()
            actionSpy.clear()
        }

        function test_noWrapRight() {
            row.currentIndex = 4
            verify(!row.moveFocus("right"))
            compare(row.currentIndex, 4)
            verify(row.bumping)
            tryCompare(row, "bumping", false)
            compare(row.bumpOffset, 0)
        }

        function test_noWrapLeft() {
            verify(!row.moveFocus("left"))
            compare(row.currentIndex, 0)
            verify(row.bumping)
        }

        function test_moveWithin() {
            verify(row.moveFocus("right"))
            verify(row.moveFocus("right"))
            compare(row.currentIndex, 2)
            verify(row.moveFocus("left"))
            compare(row.currentIndex, 1)
            verify(!row.bumping)
        }

        function test_verticalIsNotTheRowsJob() {
            verify(!row.moveFocus("up"))
            verify(!row.moveFocus("down"))
            compare(row.currentIndex, 0)
            verify(!row.bumping)
        }

        function test_clickActivatesOwnTile() {
            row.active = false            // another row has the focus
            const tile = row.itemAtIndex(3)
            verify(tile)
            mouseClick(tile)
            compare(activatedSpy.count, 1)
            compare(activatedSpy.signalArguments[0][0], 3)
            compare(row.currentIndex, 3)
            verify(focusSpy.count > 0)
            // Clicks act directly; they don't go through the Accept action.
            compare(actionSpy.count, 0)
        }

        function test_rightClickRequestsOptions() {
            const tile = row.itemAtIndex(2)
            mouseClick(tile, tile.width / 2, tile.height / 2, Qt.RightButton)
            compare(row.currentIndex, 2)
            compare(activatedSpy.count, 0)
            compare(actionSpy.count, 1)
            compare(actionSpy.signalArguments[0][0], InputRouter.Options)
            compare(InputRouter.lastSource, InputRouter.Mouse)
        }

        function test_hoverNeedsRealMovement() {
            const tile = row.itemAtIndex(2)
            mouseMove(tile, 20, 20)
            mouseMove(tile, 60, 60)
            compare(row.currentIndex, 2)
            verify(focusSpy.count > 0)

            // Keyboard moves on; a resting cursor (hover at the same spot,
            // e.g. while tiles scroll) must not pull the focus back.
            row.moveFocus("right")
            row.moveFocus("right")
            compare(row.currentIndex, 4)
            mouseMove(tile, 60, 60)
            compare(row.currentIndex, 4)
        }

        // The scrolled row moves a different tile under a resting cursor;
        // that must not pull the focus (only real pointer movement may).
        function test_restingCursorAfterScroll() {
            longModel.clear()
            for (let i = 0; i < 15; ++i)
                longModel.append({ itemId: "long" + i, label: "Long " + i, icon: "games", tint: "#4da3ff" })
            longRow.currentIndex = 0
            longRow.positionViewAtBeginning()
            const tile = longRow.itemAtIndex(1)
            const p = tile.mapToItem(root, tile.width / 2, tile.height / 2)
            mouseMove(root, p.x - 30, p.y - 30)
            mouseMove(root, p.x, p.y)
            compare(longRow.currentIndex, 1)

            for (let i = 0; i < 8; ++i)
                verify(longRow.moveFocus("right"))
            compare(longRow.currentIndex, 9)
            tryVerify(() => longRow.contentX > 0)
            wait(300)                                   // synthetic hover has had its chance
            // Tile 1 has scrolled away from under the resting cursor (it
            // may now be over another tile or a gap between tiles).
            const t1 = longRow.itemAtIndex(1)
            const p1 = t1 ? t1.mapToItem(root, 0, 0) : Qt.point(-10000, 0)
            verify(!t1 || p.x < p1.x || p.x > p1.x + t1.width, "tile 1 still under the cursor")
            compare(longRow.currentIndex, 9)
            mouseMove(root, p.x, p.y)                   // same spot again: still no hover-focus
            compare(longRow.currentIndex, 9)
            // Real movement onto a visible tile focuses it.
            const t8 = longRow.itemAtIndex(8)
            const p8 = t8.mapToItem(root, t8.width / 2, t8.height / 2)
            mouseMove(root, p8.x, p8.y)
            compare(longRow.currentIndex, 8)
        }

        function test_iconRasterises() {
            const image = findChild(probeIcon, "iconImage")
            verify(image)
            tryCompare(image, "status", Image.Ready)
            verify(image.implicitWidth > 0)
        }

        function test_mutedIgnoresMouse() {
            InputRouter.muteFor(10000)
            const tile = row.itemAtIndex(3)
            mouseMove(tile, 10, 10)
            mouseMove(tile, 50, 50)
            mouseClick(tile)
            compare(activatedSpy.count, 0)
            compare(row.currentIndex, 0)
            InputRouter.muteFor(0)
        }

        function test_blockedIgnoresMouse() {
            InputRouter.blocked = true
            mouseClick(row.itemAtIndex(1))
            compare(activatedSpy.count, 0)
            InputRouter.blocked = false
        }

        function test_wheelSteps() {
            const tile = row.itemAtIndex(0)
            mouseWheel(tile, 10, 10, 0, -120)          // one notch down = right
            compare(row.currentIndex, 1)
            mouseWheel(tile, 10, 10, 0, -360)          // three notches
            compare(row.currentIndex, 4)
            mouseWheel(tile, 10, 10, 0, -120)          // at the end: bump
            compare(row.currentIndex, 4)
            verify(row.bumping)
            verify(focusSpy.count > 0)
        }

        function test_restoreFocus() {
            row.restoreFocus(1, "item3")               // key wins
            compare(row.currentIndex, 3)
            row.restoreFocus(10, "gone")               // stale key, index clamped
            compare(row.currentIndex, 4)
            row.restoreFocus(2, "")
            compare(row.currentIndex, 2)
            compare(row.keyAt(2), "item2")
            compare(row.indexOfKey("item4"), 4)
            compare(row.indexOfKey("nope"), -1)
        }

        function test_activateCurrent() {
            row.currentIndex = 1
            row.activateCurrent()
            compare(activatedSpy.count, 1)
            compare(activatedSpy.signalArguments[0][0], 1)
            rowModel.clear()
            row.activateCurrent()
            compare(activatedSpy.count, 1)
        }
    }
}
