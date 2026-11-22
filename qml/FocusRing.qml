import QtQuick

// The one focus indicator. It tracks `target` (an item in the same
// coordinate space, e.g. a view's current delegate), glides between targets
// in Theme.animFast and scales with the target. `offsetX`/`offsetY` carry
// the edge "bump". Geometry is bound here rather than left to ListView's highlight
// sizing, which stretches the highlight to the full view height.
Item {
    id: ring

    property Item target: null
    property real offsetX: 0
    property real offsetY: 0
    property bool shown: true

    z: 10
    visible: shown && target !== null
    x: target ? target.x : 0
    y: target ? target.y : 0
    width: target ? target.width : 0
    height: target ? target.height : 0
    scale: target ? target.scale : 1

    Behavior on x {
        enabled: ring.visible
        NumberAnimation { duration: Theme.animFast; easing.type: Easing.OutCubic }
    }
    Behavior on y {
        enabled: ring.visible
        NumberAnimation { duration: Theme.animFast; easing.type: Easing.OutCubic }
    }
    transform: Translate { x: ring.offsetX; y: ring.offsetY }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -(Theme.focusGap + Theme.focusBorder)
        radius: Theme.radius + Theme.focusGap + Theme.focusBorder
        color: "transparent"
        border.color: Theme.accent
        border.width: Theme.focusBorder
        antialiasing: true
    }
}
