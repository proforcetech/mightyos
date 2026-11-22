pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// "MightyOS › Emulators › SNES". `crumbs` holds one title per NavStack
// level (root first). Earlier crumbs are clickable and emit
// crumbClicked(depth), depth 1 being the root.
Row {
    id: breadcrumb

    property var crumbs: []

    signal crumbClicked(int depth)

    spacing: Theme.spacingS

    Repeater {
        model: breadcrumb.crumbs

        delegate: Row {
            id: crumb
            required property int index
            required property string modelData

            readonly property bool isLast: index === breadcrumb.crumbs.length - 1
            spacing: Theme.spacingS

            Text {
                visible: crumb.index > 0
                text: "›"
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
            }
            Text {
                text: crumb.modelData
                color: crumb.isLast || area.containsMouse ? Theme.text : Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                font.weight: crumb.index === 0 ? Font.Bold : Font.DemiBold
                font.underline: area.containsMouse && !crumb.isLast

                MouseArea {
                    id: area
                    anchors.fill: parent
                    anchors.margins: -Theme.px(6)
                    enabled: !crumb.isLast
                    hoverEnabled: true
                    onClicked: {
                        if (InputRouter.pointerAction())
                            breadcrumb.crumbClicked(crumb.index + 1)
                    }
                }
            }
        }
    }
}
