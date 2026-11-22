pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// Horizontal row of tiles with a single FocusRing, no wrap-around and an edge
// "bump". The row never reads keys: its screen forwards InputRouter actions
// to moveFocus() / activateCurrent().
//
// Model contract (ListModel now, C++ models from M2):
//   roles  itemId (stable id), label, icon, tint; optional subtitle, available
//   get(i) returns an object with those roles (used for focus-by-key).
//
// Mouse: hovering focuses the row and the tile (after real pointer
// movement), a left click activates the clicked tile itself, a right click
// focuses it and asks for Options, and the wheel steps the focus.
// Custom delegates (CategoryBar) call hoverAt()/clickAt() for the same
// behaviour.
ListView {
    id: row

    signal activated(int index)
    // Hover / click / wheel: the screen should make this row the focused one.
    signal focusRequested()

    property bool active: true        // shows the focus ring
    property real bumpOffset: 0
    readonly property bool bumping: _bump.running
    property int tileWidth: Theme.tileSize
    property int tileHeight: Theme.tileSize

    readonly property real _contentW: count > 0 ? count * tileWidth + (count - 1) * spacing : 0

    orientation: ListView.Horizontal
    spacing: Theme.spacingM
    width: parent ? Math.min(parent.width, _contentW) : _contentW
    // Exactly one tile high: ListView owns delegate geometry. The focused
    // tile's scale-up and the ring overflow (clip is off), so the parent
    // must leave Theme.overflowFor(tileHeight) of room around the row.
    height: tileHeight
    interactive: false
    keyNavigationEnabled: false
    clip: false
    boundsBehavior: Flickable.StopAtBounds
    cacheBuffer: tileWidth * 2

    // Keep the focused tile inside the visible band when the row scrolls.
    highlightRangeMode: ListView.ApplyRange
    preferredHighlightBegin: tileWidth * 0.5
    preferredHighlightEnd: width - tileWidth * 1.5
    // FocusRing animates and sizes itself; ListView only owns its lifetime.
    highlightFollowsCurrentItem: false

    highlight: FocusRing {
        target: row.currentItem
        offsetX: row.bumpOffset
        shown: row.active
    }

    delegate: Tile {
        id: tileDelegate
        required property int index
        required property var model    // see model contract above

        label: tileDelegate.model.label ?? ""
        subtitle: tileDelegate.model.subtitle ?? ""
        iconName: tileDelegate.model.icon ?? ""
        tint: tileDelegate.model.tint ?? Theme.accent
        available: tileDelegate.model.available ?? true
        focused: ListView.isCurrentItem && row.active
        bumpX: ListView.isCurrentItem ? row.bumpOffset : 0

        onHovered: row.hoverAt(tileDelegate.index)
        onClicked: (button) => row.clickAt(tileDelegate.index, button)
    }

    WheelStepper {
        onStepped: (steps) => {
            if (!InputRouter.pointerAction())
                return
            row.focusRequested()
            row.wheelStep(steps)
        }
    }

    // Geometry changes (first layout after a restore, window resize) don't
    // re-apply the highlight range by themselves.
    onWidthChanged: _keepCurrentVisible()
    // Deterministic: from the start, bring the neighbours and then the
    // current tile into view, so the result matches having navigated there
    // (a neighbour peeks on each side, the focus never sits at the edge).
    function _keepCurrentVisible() {
        if (currentIndex < 0 || width <= 0)
            return
        positionViewAtBeginning()
        positionViewAtIndex(Math.min(count - 1, currentIndex + 1), ListView.Contain)
        positionViewAtIndex(Math.max(0, currentIndex - 1), ListView.Contain)
        positionViewAtIndex(currentIndex, ListView.Contain)
    }

    // Returns true if focus moved. Left/right at an edge bumps and returns
    // false; up/down always return false (the screen owns vertical moves).
    function moveFocus(direction: string): bool {
        let next = currentIndex
        if (direction === "left") next -= 1
        else if (direction === "right") next += 1
        else return false

        if (next < 0 || next >= count) {
            bump(direction)
            return false
        }
        currentIndex = next
        SoundFx.play(SoundFx.Move)
        return true
    }

    function wheelStep(steps: int) {
        const direction = steps > 0 ? "right" : "left"
        for (let i = 0; i < Math.abs(steps); ++i) {
            if (!moveFocus(direction))
                break
        }
    }

    function activateCurrent() {
        if (currentIndex >= 0 && currentIndex < count) {
            SoundFx.play(SoundFx.Accept)
            activated(currentIndex)
        }
    }

    function hoverAt(index: int) {
        if (!InputRouter.accepting)
            return
        focusRequested()
        if (currentIndex !== index) {
            currentIndex = index
            SoundFx.play(SoundFx.Move)
        }
    }

    function clickAt(index: int, button: int) {
        if (!InputRouter.pointerAction())
            return
        focusRequested()
        currentIndex = index
        if (button === Qt.RightButton) {
            InputRouter.dispatch(InputRouter.Options, "", InputRouter.Mouse)
        } else {
            SoundFx.play(SoundFx.Accept)
            activated(index)
        }
    }

    function bump(direction: string) {
        // Don't restart while running: key auto-repeat (~35 ms) is faster
        // than the bump's outward leg and would leave the tile offset.
        if (_bump.running)
            return
        _bump.dir = direction === "left" ? -1 : 1
        _bump.restart()
        SoundFx.play(SoundFx.Bump)
    }

    function keyAt(index: int): string {
        if (index < 0 || index >= count || !model || typeof model.get !== "function")
            return ""
        return model.get(index).itemId ?? ""
    }

    function indexOfKey(key: string): int {
        if (key.length === 0)
            return -1
        for (let i = 0; i < count; ++i) {
            if (keyAt(i) === key)
                return i
        }
        return -1
    }

    // Focus by stable key if it still exists, else by (clamped) index. Used
    // when restoring a saved path, where items may have changed meanwhile.
    function restoreFocus(index: int, key: string) {
        if (count === 0)
            return
        let target = indexOfKey(key)
        if (target < 0)
            target = Math.max(0, Math.min(index, count - 1))
        currentIndex = target
        _keepCurrentVisible()
    }

    SequentialAnimation {
        id: _bump
        property int dir: 1
        NumberAnimation {
            target: row; property: "bumpOffset"
            to: _bump.dir * Theme.bumpDistance
            duration: Theme.animFast / 2; easing.type: Easing.OutQuad
        }
        NumberAnimation {
            target: row; property: "bumpOffset"
            to: 0
            duration: Theme.animFast; easing.type: Easing.OutBack
        }
    }
}
