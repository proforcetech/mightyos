pragma Singleton
import QtQuick

// The single entry point for user input.
//
// Every input device is translated into a small set of named actions:
//   navigate(direction)  direction: "left" | "right" | "up" | "down"
//   accept
//   back
// Keyboard events arrive through fromKeyEvent(); mouse handlers and, from M3
// on, the SDL3 gamepad thread call dispatch() directly. Screens listen to
// `triggered` and never look at raw keys, so all devices behave the same.
QtObject {
    id: map

    signal triggered(string action, string arg)

    // True while an external app owns the screen (bound to Launcher.running).
    property bool blocked: false
    // True during the short mute window after an app exits.
    readonly property bool muted: _muteTimer.running
    readonly property bool accepting: !blocked && !muted

    // Raw key -> action. Extend here (and only here) for new bindings.
    readonly property var keyTable: ({
        [Qt.Key_Left]:      { action: "navigate", arg: "left",  repeat: true },
        [Qt.Key_Right]:     { action: "navigate", arg: "right", repeat: true },
        [Qt.Key_Up]:        { action: "navigate", arg: "up",    repeat: true },
        [Qt.Key_Down]:      { action: "navigate", arg: "down",  repeat: true },
        [Qt.Key_Return]:    { action: "accept",   arg: "",      repeat: false },
        [Qt.Key_Enter]:     { action: "accept",   arg: "",      repeat: false },
        [Qt.Key_Space]:     { action: "accept",   arg: "",      repeat: false },
        [Qt.Key_Escape]:    { action: "back",     arg: "",      repeat: false },
        [Qt.Key_Backspace]: { action: "back",     arg: "",      repeat: false },
        [Qt.Key_Back]:      { action: "back",     arg: "",      repeat: false }
    })

    // Returns true if the event was mapped (and therefore consumed), even if
    // input is currently blocked or muted, so keys never leak elsewhere.
    function fromKeyEvent(event) {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
            return false
        const entry = keyTable[event.key]
        if (!entry)
            return false
        if (event.isAutoRepeat && !entry.repeat)
            return true
        dispatch(entry.action, entry.arg)
        return true
    }

    // Returns true if the action was delivered.
    function dispatch(action, arg) {
        if (!accepting)
            return false
        triggered(action, arg === undefined ? "" : arg)
        return true
    }

    function muteFor(ms) {
        _muteTimer.interval = ms
        _muteTimer.restart()
    }

    // Mouse hover may only move focus after the pointer really moved; items
    // scrolling under a resting cursor also generate hover events.
    property point _lastPointer: Qt.point(-1, -1)
    function pointerMoved(scenePos) {
        const dx = scenePos.x - _lastPointer.x
        const dy = scenePos.y - _lastPointer.y
        if (_lastPointer.x >= 0 && dx * dx + dy * dy < 4)
            return false
        const first = _lastPointer.x < 0
        _lastPointer = scenePos
        return !first
    }

    property Timer _muteTimer: Timer { repeat: false }
}
