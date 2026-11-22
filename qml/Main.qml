import QtQuick
import QtQuick.Window

// Root window: safe area, top bar, current screen, prompt bar and global
// overlays. All keyboard input enters through InputMap here.
Window {
    id: window

    property bool startFullscreen: false

    // Tile index to give focus back to when the running app exits.
    property int launchIndex: -1
    property string launchTitle
    property string launchIcon
    property bool launchStarted: false

    width: 1280
    height: 720
    minimumWidth: 640
    minimumHeight: 360
    visible: true
    visibility: startFullscreen ? Window.FullScreen : Window.Windowed
    title: qsTr("MightyOS")
    color: Theme.background

    Binding { target: Theme; property: "windowWidth"; value: window.width }
    Binding { target: Theme; property: "windowHeight"; value: window.height }
    Binding { target: InputMap; property: "blocked"; value: Launcher.running }

    function activateCategory(index, entry) {
        if (Launcher.running)
            return   // never clobber the running app's launch state
        if (!entry.launchable) {
            toast.show(qsTr("%1: coming soon").arg(entry.label), false)
            return
        }
        const argv = Launcher.testAppCommand()
        if (argv.length === 0) {
            toast.show(qsTr("No test application found on this system"), true)
            return
        }
        launchIndex = index
        launchTitle = entry.label
        launchIcon = entry.icon
        launchStarted = false
        Launcher.launch("test:" + entry.categoryId, argv)
    }

    Connections {
        target: InputMap
        function onTriggered(action, arg) {
            home.handleAction(action, arg)
        }
    }

    Connections {
        target: Launcher
        function onLaunched(id) {
            window.launchStarted = true
        }
        function onExited(id, exitCode) {
            InputMap.muteFor(Theme.inputMuteAfterExit)
            if (window.launchIndex >= 0)
                home.currentIndex = window.launchIndex
            window.launchIndex = -1
            window.requestActivate()
            inputRoot.forceActiveFocus()
            if (exitCode === -1)
                toast.show(qsTr("%1 crashed").arg(window.launchTitle), true)
            else if (exitCode !== 0)
                toast.show(qsTr("%1 exited with code %2").arg(window.launchTitle).arg(exitCode), true)
        }
        function onFailed(id, error) {
            if (Launcher.running)
                return   // a rejected extra launch must not touch the running app's state
            if (window.launchIndex >= 0)
                home.currentIndex = window.launchIndex
            window.launchIndex = -1
            toast.show(qsTr("Could not launch %1: %2").arg(window.launchTitle || id).arg(error), true)
        }
    }

    Item {
        id: inputRoot
        anchors.fill: parent
        focus: true

        Keys.onPressed: (event) => { event.accepted = InputMap.fromKeyEvent(event) }

        // Safe area: 5% on every side for TV overscan.
        Item {
            id: safeArea
            anchors.fill: parent
            anchors.leftMargin: Math.round(window.width * Theme.safeAreaRatio)
            anchors.rightMargin: Math.round(window.width * Theme.safeAreaRatio)
            anchors.topMargin: Math.round(window.height * Theme.safeAreaRatio)
            anchors.bottomMargin: Math.round(window.height * Theme.safeAreaRatio)

            Item {
                id: topBar
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: Math.max(wordmark.implicitHeight, clock.implicitHeight)

                Text {
                    id: wordmark
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "MightyOS"
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Bold
                }
                Clock {
                    id: clock
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Home {
                id: home
                anchors {
                    left: parent.left; right: parent.right
                    top: topBar.bottom; bottom: promptBar.top
                    topMargin: Theme.spacingM; bottomMargin: Theme.spacingM
                }
                onCategoryActivated: (index, entry) => window.activateCategory(index, entry)
            }

            Toast {
                id: toast
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: promptBar.top
                anchors.bottomMargin: Theme.spacingM
            }

            PromptBar {
                id: promptBar
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                prompts: [
                    { button: "A", key: "Enter", label: qsTr("Open") }
                    // "Back" prompt returns with nested screens (M1).
                ]
            }
        }

        LaunchOverlay {
            anchors.fill: parent
            active: Launcher.running
            launching: !window.launchStarted
            title: window.launchTitle
            iconName: window.launchIcon
        }
    }
}
