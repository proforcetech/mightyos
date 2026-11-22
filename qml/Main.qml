import QtQuick
import QtQuick.Window
import MightyOS.Core

// Root window: safe area, top bar (breadcrumb + clock), the NavStack,
// prompt bar and global overlays.
//
// Input: InputRouter (C++) filters the window's key and mouse-button events
// and emits `triggered`. route() sends each action to the first open modal
// (Dialog, then ContextMenu: focus trap), else to the NavStack, which
// offers it to the top screen and then applies its defaults.
Window {
    id: window

    property bool startFullscreen: false
    // M1: the argv every game/app launches (the bundled test app). Real
    // commands come from AppRegistry (M2) and the library (M4). Tests set
    // it to run the test app offscreen / with --crash.
    property var launchCommand: Launcher.testAppCommand()

    // The screen and focus state to return to when the running app exits.
    property NavScreen launchScreen: null
    property var launchState: null
    property string launchTitle
    property string launchIcon
    property bool launchStarted: false

    readonly property bool modalOpen: dialog.open || contextMenu.open

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
    Binding { target: InputRouter; property: "blocked"; value: Launcher.running }
    Binding { target: InputRouter; property: "cursorHideDelay"; value: Theme.cursorHideDelay }

    function route(action: int, arg: string) {
        if (dialog.open) {
            dialog.handleAction(action, arg)
            return
        }
        if (contextMenu.open) {
            contextMenu.handleAction(action, arg)
            return
        }
        if (navStack.handleAction(action, arg))
            return
        // Global fallbacks for features that arrive later; every input gets
        // some feedback.
        switch (action) {
        case InputRouter.Search:
            toast.show(qsTr("Search arrives with the App Store (M7)"), false)
            break
        case InputRouter.QuickPanel:
            toast.show(qsTr("Quick panel (volume, Wi-Fi, power) arrives in M8"), false)
            break
        default:
            SoundFx.play(SoundFx.Bump)
            break
        }
    }

    function launch(item: var) {
        if (Launcher.running)
            return   // never clobber the running app's launch state
        const argv = launchCommand
        if (!argv || argv.length === 0) {
            toast.show(qsTr("No test application found on this system"), true)
            return
        }
        launchScreen = navStack.current
        launchState = navStack.current ? navStack.current.saveState() : null
        launchTitle = item.label
        launchIcon = item.icon
        launchStarted = false
        navStack.persistNow()
        Launcher.launch(item.itemId, argv)
    }

    function _returnFromLaunch() {
        if (launchScreen && launchScreen === navStack.current && launchState)
            launchScreen.restoreState(launchState)
        launchScreen = null
        launchState = null
    }

    Connections {
        target: InputRouter
        function onTriggered(action, arg) {
            window.route(action, arg)
        }
    }

    Connections {
        target: Launcher
        function onLaunched(id) {
            window.launchStarted = true
        }
        function onExited(id, exitCode) {
            InputRouter.muteFor(Theme.inputMuteAfterExit)
            window._returnFromLaunch()
            window.requestActivate()
            clock.refresh()
            if (exitCode === -1)
                toast.show(qsTr("%1 crashed").arg(window.launchTitle), true)
            else if (exitCode !== 0)
                toast.show(qsTr("%1 exited with code %2").arg(window.launchTitle).arg(exitCode), true)
        }
        function onFailed(id, error) {
            if (Launcher.running)
                return   // a rejected extra launch must not touch the running app's state
            window._returnFromLaunch()
            toast.show(qsTr("Could not launch %1: %2").arg(window.launchTitle || id).arg(error), true)
        }
    }

    // (f) The minute timer may be far off after suspend or a long game.
    onActiveChanged: {
        if (active)
            clock.refresh()
    }

    Component.onCompleted: navStack.restore()

    Component { id: homeScreen; HomeScreen {} }
    Component { id: systemListScreen; SystemListScreen {} }
    Component { id: gameGridScreen; GameGridScreen {} }
    Component { id: categoryScreen; CategoryScreen {} }

    Item {
        id: inputRoot
        anchors.fill: parent
        focus: true

        // Mouse fallbacks for empty space: right click = Options, wheel
        // steps the focused row/grid. Tiles and rows handle their own.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: {
                if (InputRouter.pointerAction())
                    InputRouter.dispatch(InputRouter.Options, "", InputRouter.Mouse)
            }
        }
        WheelStepper {
            onStepped: (steps) => {
                if (InputRouter.pointerAction() && !window.modalOpen)
                    navStack.wheel(steps)
            }
        }

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
                height: Math.max(breadcrumb.implicitHeight, clock.implicitHeight)

                Breadcrumb {
                    id: breadcrumb
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    crumbs: navStack.crumbs
                    onCrumbClicked: (depth) => {
                        if (!window.modalOpen)
                            navStack.popTo(depth)
                    }
                }
                Clock {
                    id: clock
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    // PLAN section 3: the clock area opens the quick panel.
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -Theme.spacingS
                        onClicked: {
                            if (InputRouter.pointerAction())
                                InputRouter.dispatch(InputRouter.QuickPanel, "", InputRouter.Mouse)
                        }
                    }
                }
            }

            NavStack {
                id: navStack
            objectName: "navStack"
                anchors {
                    left: parent.left; right: parent.right
                    top: topBar.bottom; bottom: promptArea.top
                    topMargin: Theme.spacingM; bottomMargin: Theme.spacingM
                }
                registry: ({
                    home: homeScreen,
                    systems: systemListScreen,
                    games: gameGridScreen,
                    category: categoryScreen
                })
                focusShown: !window.modalOpen && !Launcher.running
                onLaunchRequested: (item) => window.launch(item)
                onToastRequested: (message, isError) => toast.show(message, isError)
                onDialogRequested: (spec) => dialog.show(spec)
                onOptionsRequested: (title, items) => contextMenu.show(title, items)
            }

            Toast {
                id: toast
            objectName: "toast"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: promptArea.top
                anchors.bottomMargin: Theme.spacingM
                z: 1
            }

            // Reserves the prompt bar's space; the bar itself sits above the
            // modals (see below) so its prompts stay visible and clickable.
            Item {
                id: promptArea
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: promptBar.implicitHeight
            }
        }

        ContextMenu {
            id: contextMenu
            objectName: "contextMenu"
            anchors.fill: parent
            onChosen: (id) => navStack.optionChosen(id)
        }

        Dialog {
            id: dialog
            objectName: "dialog"
            anchors.fill: parent
        }

        PromptBar {
            id: promptBar
            anchors.left: safeArea.left
            anchors.bottom: safeArea.bottom
            prompts: dialog.open ? dialog.prompts
                   : contextMenu.open ? contextMenu.prompts
                   : navStack.current ? navStack.current.prompts : []
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
