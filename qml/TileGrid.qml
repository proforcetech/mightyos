pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// 2D grid of poster tiles for long lists (game libraries). No wrap-around:
// moving past an edge bumps. Up from the first row returns false so the
// screen can move focus to whatever sits above the grid.
//
// Model contract: same as TileRow (a TileListModel). jumpToLetter() uses
// the model's firstIndexForLetter() (sortTitle, else title).
//
// Set `availableWidth`; the grid sizes itself to a whole number of columns
// and centres in that width. Height is up to the parent.
GridView {
    id: grid

    signal activated(int index)
    // The model row under the focus changed in place (dataChanged, reset):
    // screens refresh their focusedItem.
    signal currentDataChanged()
    signal focusRequested()

    property bool active: true
    property real availableWidth: parent ? parent.width : 0
    property real bumpX: 0
    property real bumpY: 0
    readonly property bool bumping: _bump.running

    readonly property int overflow: Theme.overflowFor(Theme.posterHeight)
    readonly property int columns: Math.max(1, Math.floor((availableWidth - 2 * overflow) / cellWidth))
    readonly property int visibleRows: Math.max(1, Math.floor((height - 2 * overflow) / cellHeight))
    readonly property int currentRow: currentIndex >= 0 ? Math.floor(currentIndex / columns) : -1
    readonly property int currentColumn: currentIndex >= 0 ? currentIndex % columns : -1
    readonly property int rowCount: Math.ceil(count / columns)

    cellWidth: Theme.posterWidth + Theme.spacingM
    cellHeight: Theme.posterHeight + Theme.spacingM
    width: columns * cellWidth + 2 * overflow
    // Margins leave room for the focused tile's scale-up and ring; clip so
    // scrolled rows don't draw over the header or prompt bar.
    leftMargin: overflow
    rightMargin: overflow
    topMargin: overflow
    bottomMargin: overflow
    clip: true
    interactive: false
    keyNavigationEnabled: false
    boundsBehavior: Flickable.StopAtBounds
    cacheBuffer: cellHeight * 2

    highlightRangeMode: GridView.ApplyRange
    preferredHighlightBegin: cellHeight * 0.25
    preferredHighlightEnd: height - 2 * overflow - cellHeight * 1.25
    highlightFollowsCurrentItem: false

    highlight: FocusRing {
        target: grid.currentItem
        offsetX: grid.bumpX
        offsetY: grid.bumpY
        shown: grid.active
    }

    delegate: Tile {
        id: tileDelegate
        required property int index
        required property var model

        poster: true
        label: tileDelegate.model.title ?? ""
        subtitle: tileDelegate.model.subtitle ?? ""
        iconName: tileDelegate.model.icon ?? ""
        artwork: tileDelegate.model.artwork ?? ""
        tint: tileDelegate.model.tint || Theme.accent
        available: tileDelegate.model.available ?? true
        badge: tileDelegate.model.badge ?? ""
        favorite: Favorites.revision >= 0 && Favorites.isFavorite(tileDelegate.model.key ?? "")
        focused: GridView.isCurrentItem && grid.active
        bumpX: GridView.isCurrentItem ? grid.bumpX : 0
        bumpY: GridView.isCurrentItem ? grid.bumpY : 0

        onHovered: grid.hoverAt(tileDelegate.index)
        onClicked: (button) => grid.clickAt(tileDelegate.index, button)
    }

    WheelStepper {
        onStepped: (steps) => {
            if (!InputRouter.pointerAction())
                return
            grid.focusRequested()
            const direction = steps > 0 ? "down" : "up"
            for (let i = 0; i < Math.abs(steps); ++i) {
                if (!grid.moveFocus(direction))
                    break
            }
        }
    }

    onCountChanged: {
        if (currentIndex < 0 && count > 0)
            currentIndex = 0
        _takePending()
    }

    // Geometry changes (first layout after a restore, window resize, column
    // count change) don't re-apply the highlight range by themselves.
    onWidthChanged: _keepCurrentVisible()
    onHeightChanged: _keepCurrentVisible()
    function _keepCurrentVisible() {
        if (currentIndex < 0 || width <= 0 || height <= 0)
            return
        positionViewAtBeginning()
        positionViewAtIndex(Math.max(0, currentIndex - columns), GridView.Contain)
        positionViewAtIndex(currentIndex, GridView.Contain)
    }

    function _setIndex(index: int) {
        _pendingKey = ""
        currentIndex = index
        SoundFx.play(SoundFx.Move)
    }

    // Returns true if focus moved. Edges bump, except up from the first row,
    // which returns false silently so the screen can decide.
    function moveFocus(direction: string): bool {
        if (count === 0)
            return false
        const i = currentIndex
        switch (direction) {
        case "left":
            if (currentColumn === 0) { bump(direction); return false }
            _setIndex(i - 1)
            return true
        case "right":
            if (currentColumn === columns - 1 || i === count - 1) { bump(direction); return false }
            _setIndex(i + 1)
            return true
        case "up":
            if (currentRow === 0)
                return false
            _setIndex(i - columns)
            return true
        case "down":
            if (i + columns < count) {
                _setIndex(i + columns)
                return true
            }
            // Partial last row below: land on its last tile.
            if (currentRow < rowCount - 1) {
                _setIndex(count - 1)
                return true
            }
            bump(direction)
            return false
        }
        return false
    }

    // Moves a page of visible rows (delta = -1 / +1), keeping the column.
    // Bumps if already on the first / last row.
    function page(delta: int): bool {
        if (count === 0)
            return false
        const step = visibleRows * columns
        let next = currentIndex + delta * step
        if ((delta < 0 && currentRow === 0) || (delta > 0 && currentRow === rowCount - 1)) {
            bump(delta < 0 ? "up" : "down")
            return false
        }
        if (next < 0)
            next = currentColumn
        else if (next >= count)
            next = Math.min(count - 1, (rowCount - 1) * columns + currentColumn)
        _setIndex(next)
        return true
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
        if (currentIndex !== index)
            _setIndex(index)
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
        if (_bump.running)
            return
        _bump.dx = direction === "left" ? -1 : direction === "right" ? 1 : 0
        _bump.dy = direction === "up" ? -1 : direction === "down" ? 1 : 0
        _bump.restart()
        SoundFx.play(SoundFx.Bump)
    }

    function keyAt(index: int): string {
        return model && index >= 0 ? model.keyAt(index) : ""
    }

    function indexOfKey(key: string): int {
        return model && key.length > 0 ? model.indexOfKey(key) : -1
    }

    function currentEntry(): var {
        return model && currentIndex >= 0 && currentIndex < count ? model.get(currentIndex) : null
    }

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
            grid._takePending()
            grid._pendingKey = ""
        }
    }
    Connections {
        target: grid.model
        ignoreUnknownSignals: true
        function onItemsAboutToReset() {
            grid._resetKey = grid.savedKey()
            grid._resetIndex = grid.savedIndex()
        }
        function onItemsReset() {
            grid.restoreFocus(grid._resetIndex, grid._resetKey)
        }
        function onDataChanged(topLeft: var, bottomRight: var) {
            if (grid.currentIndex >= topLeft.row && grid.currentIndex <= bottomRight.row)
                grid.currentDataChanged()
        }
        function onItemMoved(from: int, to: int) {
            // Focus stays on the same tile (reordering favorites).
            if (grid.currentIndex === from)
                grid.currentIndex = to
            else if (from < grid.currentIndex && to >= grid.currentIndex)
                grid.currentIndex -= 1
            else if (from > grid.currentIndex && to <= grid.currentIndex)
                grid.currentIndex += 1
        }
    }

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

    // Letter-jump hook (A-Z overlay / LT-RT letter mode arrive with the real
    // library in M4). Focuses the first item whose sort title starts with
    // `letter` or, if none does, the first one after it. Assumes the model
    // is sorted. Returns true if focus moved.
    function jumpToLetter(letter: string): bool {
        const i = model ? model.firstIndexForLetter(letter) : -1
        if (i < 0 || i === currentIndex)
            return false
        _setIndex(i)
        return true
    }

    ParallelAnimation {
        id: _bump
        property int dx: 0
        property int dy: 0
        SequentialAnimation {
            NumberAnimation {
                target: grid; property: "bumpX"
                to: _bump.dx * Theme.bumpDistance
                duration: Theme.animFast / 2; easing.type: Easing.OutQuad
            }
            NumberAnimation {
                target: grid; property: "bumpX"; to: 0
                duration: Theme.animFast; easing.type: Easing.OutBack
            }
        }
        SequentialAnimation {
            NumberAnimation {
                target: grid; property: "bumpY"
                to: _bump.dy * Theme.bumpDistance
                duration: Theme.animFast / 2; easing.type: Easing.OutQuad
            }
            NumberAnimation {
                target: grid; property: "bumpY"; to: 0
                duration: Theme.animFast; easing.type: Easing.OutBack
            }
        }
    }
}
