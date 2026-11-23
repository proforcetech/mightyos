import QtQuick
import QtQuick.Window
import MightyOS.Core

// Root window: safe area, top bar (breadcrumb + clock), the NavStack,
// prompt bar and global overlays.
//
// Input: InputRouter (C++) filters the window's key and mouse-button events
// and emits `triggered`. route() sends each action to the first open modal
// (Dialog, then ContextMenu: focus trap, then the ProfilePicker), else to
// the NavStack, which offers it to the top screen and then applies its
// defaults.
//
// Profiles: with exactly one profile MightyOS goes straight to Home; with
// 2+ the picker is shown first, the last used profile preselected.
// Choosing another profile resets the NavStack and restores that
// profile's saved path (profiles/<id>/navstate.json).
Window {
    id: window

    property bool startFullscreen: false
    // The argv every *game* launches (the bundled test app) until the real
    // library arrives in M4. Apps launch with the argv AppRegistry parsed
    // from their .desktop file / manifest. Tests set it to run the test app
    // offscreen / with --crash.
    property var launchCommand: Launcher.testAppCommand()

    // The screen and focus state to return to when the running app exits.
    property NavScreen launchScreen: null
    property var launchState: null
    property string launchTitle
    property string launchIcon
    property bool launchStarted: false

    readonly property bool modalOpen: dialog.open || contextMenu.open || picker.open
    // Who asked for the open context menu (the NavStack or the picker).
    property Item _menuOwner: null

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
        if (picker.open) {
            picker.handleAction(action, arg)
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
        const argv = item.kind === "app" ? AppRegistry.command(item.key) : launchCommand
        if (!argv || argv.length === 0) {
            if (item.kind === "app" && !AppRegistry.ready)
                toast.show(qsTr("Still looking for apps, try %1 again in a moment").arg(item.title), false)
            else
                toast.show(item.kind === "app" ? qsTr("%1 is no longer installed").arg(item.title)
                                               : qsTr("No test application found on this system"), true)
            return
        }
        launchScreen = navStack.current
        launchState = navStack.current ? navStack.current.saveState() : null
        launchTitle = item.title
        launchIcon = item.icon
        launchStarted = false
        navStack.persistNow()
        Launcher.launch(item.key, argv)
    }

    // Makes `id` the active profile and rebuilds its navigation.
    function enterProfile(id: string) {
        // Drop the old profile's screens first (reset() writes its pending
        // path under its own id). Only then switch: Favorites/History reset
        // their models on activeChanged, and live screens would react by
        // persisting their fallback focus into the old profile's file.
        navStack.reset()
        ProfileService.select(id)          // stamps lastUsed
        navStack.profileId = ProfileService.activeId
        navStack.restore()                 // that profile's path
    }

    // The picker is about to delete `id`. If it is the profile on screen,
    // its screens go first (same reason as above), so nothing writes into
    // the directories being removed.
    function _profileRemoving(id: string) {
        if (id === navStack.profileId)
            navStack.reset()
    }

    function openProfilePicker() {
        if (Launcher.running || picker.open)
            return
        navStack.persistNow()              // the current profile's position is saved
        picker.show(false)
    }

    function _pickerChosen(id: string) {
        picker.close()
        const switching = id !== navStack.profileId || navStack.depth === 0
        if (!switching) {
            ProfileService.select(id)
            return
        }
        const wasStartup = navStack.depth === 0
        enterProfile(id)
        if (!wasStartup)
            toast.show(qsTr("Playing as %1").arg(ProfileService.active.name ?? ""), false)
    }

    // Back from the picker: stay, unless the current profile was deleted.
    function _pickerClosed() {
        if (navStack.profileId !== ProfileService.activeId || navStack.depth === 0)
            enterProfile(ProfileService.activeId)
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
        target: ProfileService
        // A late debounced save would recreate a deleted profile's dir.
        function onProfileRemoved(id) {
            NavState.discard(id)
        }
    }

    Connections {
        target: Launcher
        function onLaunched(id) {
            window.launchStarted = true
            History.record(id)             // only successful launches count
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

    Component.onCompleted: {
        if (ProfileService.pickerAtStartup)
            picker.show(true)
        else
            enterProfile(ProfileService.activeId)
    }

    Component { id: homeScreen; HomeScreen {} }
    Component { id: systemListScreen; SystemListScreen {} }
    Component { id: gameGridScreen; GameGridScreen {} }
    Component { id: categoryScreen; CategoryScreen {} }
    Component { id: appListScreen; AppListScreen {} }

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
                    crumbs: picker.open ? [qsTr("MightyOS"), qsTr("Profiles")] : navStack.crumbs
                    onCrumbClicked: (depth) => {
                        if (!window.modalOpen)
                            navStack.popTo(depth)
                    }
                }
                ProfileChip {
                    id: profileChip
                    visible: !picker.open
                    anchors.right: clock.left
                    anchors.rightMargin: Theme.spacingL
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: {
                        if (!window.modalOpen)
                            window.openProfilePicker()
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
                    category: categoryScreen,
                    apps: appListScreen
                })
                visible: !picker.open
                focusShown: !window.modalOpen && !Launcher.running
                onLaunchRequested: (item) => window.launch(item)
                onToastRequested: (message, isError) => toast.show(message, isError)
                onDialogRequested: (spec) => dialog.show(spec)
                onOptionsRequested: (title, items) => {
                    window._menuOwner = navStack
                    contextMenu.show(title, items)
                }
                onProfilePickerRequested: window.openProfilePicker()
            }

            ProfilePicker {
                id: picker
                objectName: "profilePicker"
                anchors.fill: navStack
                onChosen: (id) => window._pickerChosen(id)
                onRemoving: (id) => window._profileRemoving(id)
                onClosed: window._pickerClosed()
                onToastRequested: (message, isError) => toast.show(message, isError)
                onDialogRequested: (spec) => dialog.show(spec)
                onOptionsRequested: (title, items) => {
                    window._menuOwner = picker
                    contextMenu.show(title, items)
                }
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
            onChosen: (id) => {
                const owner = window._menuOwner
                window._menuOwner = null
                if (owner === picker)
                    picker.optionChosen(id)
                else
                    navStack.optionChosen(id)
            }
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
                   : picker.open ? picker.prompts
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
