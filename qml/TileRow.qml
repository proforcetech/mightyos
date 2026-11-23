pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// Horizontal row of tiles with a single FocusRing, no wrap-around and an edge
// "bump". The row never reads keys: its screen forwards InputRouter actions
// to moveFocus() / activateCurrent().
//
// Model contract: a TileListModel (C++). Roles key, title, subtitle, icon,
// artwork, kind, available, tint, badge; lookups go through its
// indexOfKey() / keyAt() (hash-backed), never a JavaScript loop. A model
// reset (re-scan, profile switch) keeps the focus on the same key.
//
// Mouse: hovering focuses the row and the tile (after real pointer
// movement), a left click activates the clicked tile itself, a right click
// focuses it and asks for Options, and the wheel steps the focus.
// Custom delegates (CategoryBar) call hoverAt()/clickAt() for the same
// behaviour.
ListView {
    id: row

    signal activated(int index)
    // The model row under the focus changed in place (dataChanged, reset):
    // screens refresh their focusedItem.
    signal currentDataChanged()
    // Hover / click / wheel: the screen should make this row the focused one.
    signal focusRequested()

    property bool active: true        // shows the focus ring
    property real bumpOffset: 0
    readonly property bool bumping: _bump.running
    property int tileWidth: Theme.tileSize
    property int tileHeight: Theme.tileSize
    // Star badges on favorites (off for rows that never hold favorites).
    property bool showFavorites: true

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

        width: row.tileWidth
        height: row.tileHeight
        label: tileDelegate.model.title ?? ""
        subtitle: tileDelegate.model.subtitle ?? ""
        iconName: tileDelegate.model.icon ?? ""
        artwork: tileDelegate.model.artwork ?? ""
        tint: tileDelegate.model.tint || Theme.accent
        tintIcon: tileDelegate.model.kind === "profile"
        available: tileDelegate.model.available ?? true
        badge: tileDelegate.model.badge ?? ""
        // `revision` makes the binding re-evaluate when favorites change.
        favorite: row.showFavorites && Favorites.revision >= 0
                  && Favorites.isFavorite(tileDelegate.model.key ?? "")
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

    // Rows added to an empty model (the first favorite) leave the view at
    // index -1; there is always a focused tile when there are tiles.
    onCountChanged: {
        if (currentIndex < 0 && count > 0)
            currentIndex = 0
        _takePending()
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
        _pendingKey = ""
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
            _pendingKey = ""
            currentIndex = index
            SoundFx.play(SoundFx.Move)
        }
    }

    function clickAt(index: int, button: int) {
        if (!InputRouter.pointerAction())
            return
        focusRequested()
        _pendingKey = ""
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
        return model && index >= 0 ? model.keyAt(index) : ""
    }

    function indexOfKey(key: string): int {
        return model && key.length > 0 ? model.indexOfKey(key) : -1
    }

    // The model row under the focus, as a plain object (or null).
    function currentEntry(): var {
        return model && currentIndex >= 0 && currentIndex < count ? model.get(currentIndex) : null
    }

    // A reset replaces every row: put the focus back on the same key (or
    // the same, clamped, index if that item is gone).
    property string _resetKey
    property int _resetIndex: 0
    // App-backed rows (Media, All apps, Recent): a saved key that isn't in
    // the model before AppRegistry's first scan stays pending instead of
    // being clamped away. Static rows keep the plain clamp.
    property bool keepPendingKey: false
    property string _pendingKey
    property int _pendingIndex: 0
    Connections {
        target: AppRegistry
        function onReadyChanged() {
            // After a real scan a missing key is really gone.
            row._takePending()
            row._pendingKey = ""
        }
    }
    Connections {
        target: row.model
        ignoreUnknownSignals: true
        function onItemsAboutToReset() {
            row._resetKey = row.savedKey()
            row._resetIndex = row.savedIndex()
        }
        function onItemsReset() {
            row.restoreFocus(row._resetIndex, row._resetKey)
        }
        function onDataChanged(topLeft: var, bottomRight: var) {
            if (row.currentIndex >= topLeft.row && row.currentIndex <= bottomRight.row)
                row.currentDataChanged()
        }
        function onItemMoved(from: int, to: int) {
            // Focus stays on the same tile (reordering favorites).
            if (row.currentIndex === from)
                row.currentIndex = to
            else if (from < row.currentIndex && to >= row.currentIndex)
                row.currentIndex -= 1
            else if (from > row.currentIndex && to <= row.currentIndex)
                row.currentIndex += 1
        }
    }

    // Focus by stable key if it still exists, else by (clamped) index. Used
    // when restoring a saved path, where items may have changed meanwhile.
    function restoreFocus(index: int, key: string) {
        _pendingKey = ""
        let target = indexOfKey(key)
        if (target < 0 && key.length > 0 && keepPendingKey && !AppRegistry.ready) {
            // Not there (yet): apps arrive with the first scan, Recent after
            // a re-scan. Remember it until it shows up or the user moves,
            // and save it unchanged meanwhile (savedKey/savedIndex).
            _pendingKey = key
            _pendingIndex = Math.max(0, index)
        }
        if (count === 0)
            return
        if (target < 0)
            target = Math.max(0, Math.min(index, count - 1))
        currentIndex = target
        _keepCurrentVisible()
        currentDataChanged()
    }

    // What to persist: a still-pending saved position wins over the
    // fallback tile the view shows meanwhile.
    function savedKey(): string {
        return _pendingKey.length > 0 ? _pendingKey : keyAt(currentIndex)
    }
    function savedIndex(): int {
        return _pendingKey.length > 0 ? _pendingIndex : Math.max(0, currentIndex)
    }

    function _takePending() {
        if (_pendingKey.length === 0)
            return
        const i = indexOfKey(_pendingKey)
        if (i >= 0) {
            _pendingKey = ""
            currentIndex = i
            _keepCurrentVisible()
            currentDataChanged()
        }
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
