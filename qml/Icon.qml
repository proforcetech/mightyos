import QtQuick
import "Icons.js" as Icons

// Vector icon from the shared 24x24 path table (Icons.js), rasterised by
// the SVG image plugin once per (name, colour, size) and cached as a
// texture. Compared with a Shape this costs nothing per frame, every tile
// showing the same icon shares one texture, and it is clipped correctly by
// scrolling views in the software scene-graph backend too (Shape render
// nodes ignore clipping there; software rendering is the M9 fallback).
Item {
    id: icon

    property string name
    property color color: Theme.text

    implicitWidth: Theme.tileIconSize
    implicitHeight: Theme.tileIconSize

    // Rasterise at the focused (scaled-up) size so focus stays crisp.
    readonly property int _pixels: Math.ceil(Math.min(width, height) * Theme.focusScale)

    Image {
        objectName: "iconImage"
        anchors.fill: parent
        fillMode: Image.PreserveAspectFit
        sourceSize.width: icon._pixels
        sourceSize.height: icon._pixels
        smooth: true
        source: Icons.dataUrl(icon.name, icon.color)
    }
}
