import QtQuick

// Locale short time. Wakes once per minute (aligned to the minute boundary)
// rather than every second, to keep idle cost near zero; not at all while
// `active` is false (dormant while an app runs). Turning it back on
// refreshes at once.
Text {
    textFormat: Text.PlainText
    id: clock

    property bool active: true

    color: Theme.text
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontBody
    font.weight: Font.DemiBold

    function refresh() {
        const now = new Date()
        // Some locales' (and "C"'s) short format still includes seconds.
        const fmt = Qt.locale().timeFormat(Locale.ShortFormat).replace(/:ss?/, "")
        text = now.toLocaleTimeString(Qt.locale(), fmt)
        tick.interval = (60 - now.getSeconds()) * 1000 - now.getMilliseconds() + 50
        if (active)
            tick.restart()
        else
            tick.stop()
    }

    onActiveChanged: refresh()

    Timer {
        id: tick
        repeat: false
        onTriggered: clock.refresh()
    }

    Component.onCompleted: refresh()
}
