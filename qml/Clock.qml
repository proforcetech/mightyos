import QtQuick

// Locale short time. Wakes once per minute (aligned to the minute boundary)
// rather than every second, to keep idle cost near zero.
Text {
    id: clock

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
        tick.restart()
    }

    Timer {
        id: tick
        repeat: false
        onTriggered: clock.refresh()
    }

    Component.onCompleted: refresh()
}
