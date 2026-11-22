import QtQuick
import QtQuick.Window

// Tiny stand-in "game" for the M0 launch flow. Only uses QtQuick so that a
// system qml/qmlscene runtime can show this file directly.
// Quit with Esc, Q, Enter or a click.
Window {
    id: win
    width: 640
    height: 360
    visible: true
    color: "#22324a"
    title: "MightyOS test app"

    // Set by mightyos-testapp --quit-after <ms> (tests); 0 = never.
    property int quitAfter: 0

    Timer {
        interval: win.quitAfter
        running: win.quitAfter > 0
        onTriggered: Qt.quit()
    }

    Item {
        anchors.fill: parent
        focus: true
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Q
                    || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                event.accepted = true
                Qt.quit()
            }
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 16
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "MightyOS test app"
            color: "white"
            font.pixelSize: 36
            font.bold: true
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Press Esc / Enter or click to quit"
            color: "#c8d3e6"
            font.pixelSize: 24
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: Qt.quit()
    }
}
