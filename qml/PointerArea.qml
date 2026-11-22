import QtQuick
import MightyOS.Core

// Mouse handling shared by every focusable thing (tiles, pills, menu rows,
// dialog buttons, prompts). `hovered` only fires after the pointer really
// moved (InputRouter.pointerMoved), so items scrolling under a resting
// cursor never steal focus. `tapped` reports the button; the owner
// decides what it means. (Named `tapped` because MouseArea already has
// `clicked`.)
MouseArea {
    id: area

    signal hovered()
    signal tapped(int button)

    hoverEnabled: true
    acceptedButtons: Qt.LeftButton | Qt.RightButton

    onPositionChanged: (mouse) => {
        if (InputRouter.pointerMoved(mapToItem(null, mouse.x, mouse.y)))
            area.hovered()
    }
    onClicked: (mouse) => area.tapped(mouse.button)
}
