import QtQuick
import QtQuick.Shapes

// Full-screen scrim shown while an external app starts / runs. Swallows all
// mouse input. The spinner only animates during "launching" so a running
// game does not pay for our animation.
Rectangle {
    id: overlay

    property string title
    property string iconName: "placeholder"
    property bool launching: false   // true until the process has started
    property bool waiting: false     // a Steam launch waiting for its game
    property bool tracked: false     // a Steam launch: Guide (held) returns
    property bool closable: false    // a web app / stream: Guide (held) closes it
    // The TV remote pressed Home once: a second press within 1.5 s closes it.
    // (Under cage the app's window covers ours until M6, so this hint shows
    // only where MightyOS stays visible; the double press works either way.)
    property bool remoteCloseArmed: false
    property bool active: false

    color: Theme.scrim
    opacity: active ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: Theme.animFast } }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Column {
        anchors.centerIn: parent
        spacing: Theme.spacingL

        Item {
            id: spinner
            anchors.horizontalCenter: parent.horizontalCenter
            width: Theme.px(96)
            height: width

            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                visible: overlay.launching
                ShapePath {
                    fillColor: "transparent"
                    strokeColor: Theme.accent
                    strokeWidth: Theme.px(8)
                    capStyle: ShapePath.RoundCap
                    PathAngleArc {
                        centerX: spinner.width / 2
                        centerY: spinner.height / 2
                        radiusX: spinner.width / 2 - Theme.px(8)
                        radiusY: radiusX
                        startAngle: 0
                        sweepAngle: 270
                    }
                }
                RotationAnimator on rotation {
                    from: 0; to: 360
                    duration: 900
                    loops: Animation.Infinite
                    running: overlay.visible && overlay.launching
                }
            }

            Icon {
                anchors.fill: parent
                visible: !overlay.launching
                name: overlay.iconName
                color: Theme.accent
            }
        }

        Text {
            textFormat: Text.PlainText
            anchors.horizontalCenter: parent.horizontalCenter
            text: overlay.launching ? qsTr("Launching %1…").arg(overlay.title)
                : overlay.waiting ? qsTr("Waiting for Steam to start %1…").arg(overlay.title)
                : qsTr("%1 is running").arg(overlay.title)
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }

        Text {
            textFormat: Text.PlainText
            anchors.horizontalCenter: parent.horizontalCenter
            text: overlay.remoteCloseArmed ? qsTr("Press Home again to close")
                : overlay.waiting ? qsTr("Hold the Guide button to stop waiting")
                : overlay.tracked ? qsTr("Quit the game to return, or hold the Guide button")
                : overlay.closable ? qsTr("Hold the Guide button to close it")
                : qsTr("Close the app to return to MightyOS")
            color: Theme.textDim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
            visible: !overlay.launching
        }
    }
}
