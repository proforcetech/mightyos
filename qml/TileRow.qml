pragma ComponentBehavior: Bound
import QtQuick

// Horizontal row of Tiles with a single FocusRing, no wrap-around and an edge
// "bump". The row does not read keys itself: callers forward InputMap
// actions to moveFocus() / activateCurrent().
//
// Model roles used: label, icon, tint.
ListView {
    id: row

    signal activated(int index)

    property bool active: true        // shows the focus ring
    property real bumpOffset: 0

    readonly property real _contentW: count > 0
        ? count * Theme.tileSize + (count - 1) * spacing : 0

    orientation: ListView.Horizontal
    spacing: Theme.spacingM
    // Center the row when it is narrower than the available width.
    width: parent ? Math.min(parent.width, _contentW) : _contentW
    // Exactly one tile high: ListView owns delegate geometry. The focused
    // tile's scale-up and the ring overflow vertically (clip is off), so the
    // parent must leave Theme.focusOverflow of room above and below.
    height: Theme.tileSize
    interactive: false
    keyNavigationEnabled: false
    clip: false
    boundsBehavior: Flickable.StopAtBounds
    cacheBuffer: Theme.tileSize * 2

    // Keep the focused tile inside the visible band when the row scrolls.
    highlightRangeMode: ListView.ApplyRange
    preferredHighlightBegin: Theme.tileSize * 0.5
    preferredHighlightEnd: width - Theme.tileSize * 1.5
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
        required property var model    // roles: label, icon, tint

        label: tileDelegate.model.label
        iconName: tileDelegate.model.icon
        tint: tileDelegate.model.tint
        focused: ListView.isCurrentItem && row.active
        bumpOffset: ListView.isCurrentItem ? row.bumpOffset : 0

        onHovered: {
            if (InputMap.accepting)
                row.currentIndex = tileDelegate.index
        }
        onClicked: {
            if (!InputMap.accepting)
                return
            row.currentIndex = tileDelegate.index
            InputMap.dispatch("accept", "")
        }
    }

    // Returns true if focus moved, false (and bumps) at an edge.
    function moveFocus(direction) {
        let next = currentIndex
        if (direction === "left") next -= 1
        else if (direction === "right") next += 1
        else return false

        if (next < 0 || next >= count) {
            // Don't restart while running: key auto-repeat (~35 ms) is faster
            // than the bump's outward leg and would leave the tile offset.
            if (!_bump.running) {
                _bump.dir = direction === "left" ? -1 : 1
                _bump.restart()
            }
            return false
        }
        currentIndex = next
        return true
    }

    function activateCurrent() {
        if (currentIndex >= 0)
            activated(currentIndex)
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
