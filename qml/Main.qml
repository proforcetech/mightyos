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
// Guide (Guide button, Ctrl+Home, the remote's Home/Root Menu) answers
// an open dialog or menu with its safe default (as Back does) and returns
// to Home > Favorites. Held while an app runs it becomes
// InputRouter.guideHeld:
//  - a Steam launch (tracked): stop waiting for / tracking the game,
//  - a web app (kiosk browser) or Moonlight stream: close it
//    (Launcher.terminate(): SIGTERM to its process group, SIGKILL 3 s
//    later). These have no exit a gamepad or TV remote reaches and no
//    unsaved state. The TV remote (lower trust, PLAN D18/D25) may only do
//    it in "full" mode and with two Home presses within 1.5 s (the first
//    arms it: "Press Home again to close"); in "navigation only" mode it
//    can't close apps at all,
//  - anything else: logged and mentioned in a toast after the app exits
//    (the Guide overlay of M6).
//
// Profiles: with exactly one profile MightyOS goes straight to Home; with
// 2+ the picker is shown first, the last used profile preselected.
// Choosing another profile resets the NavStack and restores that
// profile's saved path (profiles/<id>/navstate.json).
Window {
    id: window

    property bool startFullscreen: false
    // Games launch through Library.launch() (emulator / Steam argv; the
    // bundled test app in mock mode). Apps launch with the argv AppRegistry
    // parsed from their .desktop file / manifest. Tests set launchCommand
    // to make every game run the test app offscreen / with --crash.
    property var launchCommand: []

    // The screen and focus state to return to when the running app exits.
    property NavScreen launchScreen: null
    property var launchState: null
    property string launchTitle
    property string launchIcon
    property bool launchStarted: false
    // The running launch closes on Guide held (web apps, streams).
    property bool launchClosable: false
    property bool launchClosedByGuide: false
    // The TV remote's first Home press on a closable launch (S2).
    property bool remoteCloseArmed: false

    readonly property bool modalOpen: dialog.open || contextMenu.open || picker.open
    // Guide was held during the current launch (no overlay before M6).
    property bool guideHeldWhileRunning: false
    // Who asked for the open context menu (the NavStack or the picker).
    property Item _menuOwner: null

    width: 1280
    height: 720
    minimumWidth: 640
    minimumHeight: 360
    visible: true
    // Always map windowed first; go fullscreen after the first frame. Asking
    // for fullscreen before the surface's initial commit makes cage 0.2.1
    // (wlroots 0.19) abort in wlr_xdg_surface_schedule_configure.
    visibility: Window.Windowed
    property bool _fullscreenRequested: false
    Connections {
        target: window
        // Disconnected (disabled) once the request is out: no handler per frame.
        enabled: window.startFullscreen && !window._fullscreenRequested
        function onFrameSwapped() {
            window._fullscreenRequested = true
            // Queued: not from inside the render loop's swap notification.
            Qt.callLater(() => window.visibility = Window.FullScreen)
        }
    }
    title: qsTr("MightyOS")
    color: Theme.background

    Binding { target: Theme; property: "windowWidth"; value: window.width }
    Binding { target: Theme; property: "windowHeight"; value: window.height }
    Binding { target: InputRouter; property: "blocked"; value: Launcher.running }
    Binding { target: InputRouter; property: "cursorHideDelay"; value: Theme.cursorHideDelay }
    Binding { target: InputRouter; property: "rumbleEnabled"; value: Settings.rumble }
    Binding { target: Library; property: "showAllSystems"; value: Settings.showAllSystems }
    // No artwork downloads while a game runs (bandwidth, CPU).
    Binding { target: Scraper; property: "paused"; value: Launcher.running }
    // PLAN D18: TV remote trust level, and the contexts it may only cancel.
    Binding {
        target: InputRouter; property: "remoteMode"
        value: Settings.tvRemote === "off" ? InputRouter.RemoteOff
             : Settings.tvRemote === "full" ? InputRouter.RemoteFull : InputRouter.RemoteNavigation
    }
    Binding {
        target: InputRouter; property: "acceptRemote"
        value: !(dialog.open && !dialog.acceptsRemote)
               && !(navStack.current && !navStack.current.acceptsRemote)
    }
    Binding {
        target: InputRouter; property: "secureEntry"
        value: !!navStack.current && navStack.current.secureEntry
    }

    // Guide: close what is open (each modal's Back is its safe answer), then
    // Home. The startup profile picker has nothing to go back to.
    function goHome() {
        for (let i = 0; i < 4 && window.modalOpen; ++i) {
            if (dialog.open)
                dialog.handleAction(InputRouter.Back, "")
            else if (contextMenu.open)
                contextMenu.handleAction(InputRouter.Back, "")
            else if (picker.open && navStack.depth > 0)
                picker.handleAction(InputRouter.Back, "")
            else
                break
        }
        if (window.modalOpen || !navStack.goHome())
            SoundFx.play(SoundFx.Bump)
    }

    function route(action: int, arg: string) {
        if (action === InputRouter.Guide) {
            goHome()
            return
        }
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
        const isApp = item.kind === "app"
        const isMedia = item.kind === "webapp" || item.kind === "stream"
        if (item.kind === "stream" && !Media.ready) {
            toast.show(qsTr("Still looking for Moonlight hosts, try %1 again in a moment").arg(item.title), false)
            return
        }
        if (isApp && !Library.itemAllowed(item.key)) {
            toast.show(qsTr("Ask a parent: this isn't available in a kids profile"), false)
            return
        }
        const argv = isApp ? AppRegistry.command(item.key) : launchCommand
        if (isApp && (!argv || argv.length === 0)) {
            if (!AppRegistry.ready)
                toast.show(qsTr("Still looking for apps, try %1 again in a moment").arg(item.title), false)
            else
                toast.show(qsTr("%1 is no longer installed").arg(item.title), true)
            return
        }
        launchScreen = navStack.current
        launchState = navStack.current ? navStack.current.saveState() : null
        launchTitle = item.title
        launchIcon = item.icon
        launchStarted = false
        guideHeldWhileRunning = false
        launchClosable = isMedia
        launchClosedByGuide = false
        navStack.persistNow()
        if (isMedia) {
            // Media builds the kiosk-browser / Moonlight argv and calls Launcher.
            const started = Media.launch(item.key)
            if (!started.attempted) {
                _returnFromLaunch()
                toast.show(started.message, true)
            }
            return
        }
        if (isApp || argv.length > 0) {
            Launcher.launch(item.key, argv)
            return
        }
        // A game: Library picks the emulator (or Steam) and calls Launcher.
        const result = Library.launch(item.key)
        if (!result.attempted) {
            _returnFromLaunch()
            toast.show(result.message, true)   // e.g. "Install Dolphin from the App Store ..."
        }
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
        if (Library.restricted) {
            // PLAN D13: leaving a kids profile needs the PIN (M8). Until
            // the PIN exists it isn't possible from the kids profile at all.
            toast.show(qsTr("Ask a parent to switch profiles (the PIN arrives in M8)"), false)
            return
        }
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
        launchClosable = false
        remoteCloseArmed = false
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
        function onGuideHeld(source) {
            // InputRouter logs it (mightyos.input); the overlay is M6.
            // A Steam launch (tracked): Guide gives the screen back, whether
            // the game was seen or not (the heuristics can be wrong).
            if (Launcher.tracked) {
                Launcher.stopTracking()
                return
            }
            if (Launcher.running && window.launchClosable) {
                if (source === InputRouter.Remote) {
                    if (Settings.tvRemote !== "full")
                        return   // navigation only: the remote can't close apps
                    if (!window.remoteCloseArmed) {
                        window.remoteCloseArmed = true
                        remoteCloseTimer.restart()
                        return
                    }
                }
                remoteCloseTimer.stop()
                window.remoteCloseArmed = false
                window.launchClosedByGuide = Launcher.terminate()
                return
            }
            if (Launcher.running)
                window.guideHeldWhileRunning = true
        }
        function onDeviceChanged(connected, name, source, initial) {
            if (initial || Launcher.running)
                return
            toast.show(connected ? qsTr("%1 connected").arg(name) : qsTr("%1 disconnected").arg(name), false)
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
            const guideHeld = window.guideHeldWhileRunning
            const closed = window.launchClosedByGuide
            window.guideHeldWhileRunning = false
            window.launchClosedByGuide = false
            if (closed)
                toast.show(qsTr("Closed %1").arg(window.launchTitle), false)
            else if (exitCode === -1)
                toast.show(qsTr("%1 crashed").arg(window.launchTitle), true)
            else if (exitCode !== 0)
                toast.show(qsTr("%1 exited with code %2").arg(window.launchTitle).arg(exitCode), true)
            else if (guideHeld)
                toast.show(qsTr("The Guide menu (resume / close a game) arrives in M6"), false)
        }
        function onFailed(id, error) {
            if (Launcher.running)
                return   // a rejected extra launch must not touch the running app's state
            window._returnFromLaunch()
            toast.show(qsTr("Could not launch %1: %2").arg(window.launchTitle || id).arg(error), true)
        }
    }

    Timer {
        id: remoteCloseTimer
        interval: 1500
        onTriggered: window.remoteCloseArmed = false
    }

    // (f) The minute timer may be far off after suspend or a long game.
    onActiveChanged: {
        if (active)
            clock.refresh()
    }

    Component.onCompleted: {
        // A kids profile used last starts directly: the picker would let
        // anyone leave it without the PIN.
        if (ProfileService.pickerAtStartup && ProfileService.active.kind !== "kids")
            picker.show(true)
        else
            enterProfile(ProfileService.activeId)
    }

    Component { id: homeScreen; HomeScreen {} }
    Component { id: systemListScreen; SystemListScreen {} }
    Component { id: gameGridScreen; GameGridScreen {} }
    Component { id: categoryScreen; CategoryScreen {} }
    Component { id: appListScreen; AppListScreen {} }
    Component { id: artworkScreen; ArtworkScreen {} }
    Component { id: mediaScreen; MediaScreen {} }
    Component { id: moonlightScreen; MoonlightScreen {} }
    Component { id: mediaSettingsScreen; MediaSettingsScreen {} }

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
                    apps: appListScreen,
                    artwork: artworkScreen,
                    media: mediaScreen,
                    moonlight: moonlightScreen,
                    mediasettings: mediaSettingsScreen
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
            waiting: Launcher.waiting
            tracked: Launcher.tracked
            closable: window.launchClosable
            remoteCloseArmed: window.remoteCloseArmed
            title: window.launchTitle
            iconName: window.launchIcon
        }
    }
}
