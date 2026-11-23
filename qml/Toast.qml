import QtQuick
import MightyOS.Core

// Transient, non-focusable notification. show() replaces any current toast.
// Errors play the Error sound, which also rumbles the controller
// (Feedback::wire, InputRouter.rumble policy).
Rectangle {
    id: toast

    property bool error: false
    readonly property alias message: label.text
    readonly property bool shown: state === "shown"

    function show(message, isError) {
        label.text = message
        error = !!isError
        if (error)
            SoundFx.play(SoundFx.Error)
        state = "shown"
        hideTimer.restart()
    }

    width: Math.min(label.implicitWidth + 2 * Theme.spacingL, parent ? parent.width : 9999)
    height: label.implicitHeight + 2 * Theme.spacingM
    radius: height / 2
    color: Theme.surfaceHigh
    border.width: Math.max(1, Theme.px(3))
    border.color: error ? Theme.danger : Theme.accent
    opacity: 0
    visible: opacity > 0

    Text {
        textFormat: Text.PlainText
        id: label
        anchors.centerIn: parent
        width: Math.min(implicitWidth, toast.width - 2 * Theme.spacingL)
        elide: Text.ElideRight
        color: Theme.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
    }

    Timer {
        id: hideTimer
        interval: Theme.toastDuration
        onTriggered: toast.state = ""
    }

    states: State {
        name: "shown"
        PropertyChanges { toast.opacity: 1 }
    }
    transitions: Transition {
        NumberAnimation { property: "opacity"; duration: Theme.animNormal }
    }
}
