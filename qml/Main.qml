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
// to Home > Favorites. While an app runs, Guide *held* (InputRouter.guideHeld;
// gamepad: 1 s, PLAN D25) or the labwc panic key Ctrl+Alt+Home (SIGUSR1 ->
// Session.guideRequested) opens the Guide menu (GuideMenu: Resume / Close,
// or Stop waiting / Back to MightyOS for a Steam launch):
//  - in the layer-shell overlay window over the app when available
//    (Session.overlayAvailable, GuideOverlay),
//  - else inside this window, but only if this window can be seen (it is
//    active or not fullscreen); otherwise the M5 behaviour stays: a Steam
//    launch stops tracking, a web app / stream closes, anything else gets
//    a hint after it exits.
// While it is open, input is unblocked and routed to the menu; Back and
// Guide resume. Close of a game asks first; a web app or Moonlight stream
// (no unsaved state, D25) closes at once (Launcher.terminate(): its systemd
// scope or process group, SIGKILL 3 s later). The TV remote (lower trust,
// D18/D25) can open the menu and resume; it closes only a web app / stream,
// only in "full" mode and with two Home presses within 1.5 s (the first
// arms it: "Press Home again to close", shown in the menu).
// Ctrl+Alt+End (SIGUSR2 -> Session.closeRequested) closes the running app
// at once, for apps that hang. Ctrl+Alt+Backspace (SIGRTMIN+1 ->
// Session.quitRequested) asks "Leave MightyOS?" in a standard profile (the
// remote can only cancel), says "Ask a parent" in a kids profile, and opens
// the Guide menu while an app runs (close it first).
//
// Dormant (Session.dormant): 1.5 s after an app started, while this window
// has nothing to show (the Guide menu is closed or in its own overlay
// window), the screens are hidden, the clock stops and caches are released;
// the first frame after the app exits draws them again.
//
// Safe mode (Session.safeMode, after repeated crashes in the session): a
// banner in the top bar, no artwork downloads.
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
    // Guide was held during the current launch while no menu could be shown.
    property bool guideHeldWhileRunning: false
    // The Guide menu over the running app (overlay window or in-window).
    property bool guideOpen: false
    // The overlay window's menu while it exists (GuideOverlay sets it).
    property GuideMenu overlayMenu: null
    readonly property GuideMenu activeGuideMenu: overlayMenu ?? inWindowGuide
    property double launchStartedAt: 0
    // Nothing of ours to draw while the app runs (see the header).
    property bool dormant: false
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
    Binding { target: InputRouter; property: "blocked"; value: Launcher.running && !window.guideOpen }
    Binding { target: InputRouter; property: "cursorHideDelay"; value: Theme.cursorHideDelay }
    Binding { target: InputRouter; property: "rumbleEnabled"; value: Settings.rumble }
    Binding { target: Library; property: "showAllSystems"; value: Settings.showAllSystems }
    // No artwork downloads while a game runs (bandwidth, CPU), nor in safe mode.
    Binding { target: Scraper; property: "paused"; value: Launcher.running || Session.safeMode }
    Binding { target: Session; property: "guideOpen"; value: window.guideOpen }
    Binding { target: Session; property: "dormant"; value: window.dormant }
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
               && !(window.guideOpen && !window.activeGuideMenu.acceptsRemote)
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
        if (window.guideOpen) {
            if (action === InputRouter.Guide) {
                // The remote's second Home on an armed closable launch.
                if (window.remoteCloseArmed && InputRouter.lastSource === InputRouter.Remote)
                    guideCloseApp()
                else
                    closeGuide()
            } else {
                window.activeGuideMenu.handleAction(action, arg)
            }
            return
        }
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

    // Opens the Guide menu over the running app. False if no menu can be
    // seen (no layer shell and this window is covered).
    function openGuide(): bool {
        if (!Launcher.running)
            return false
        if (window.guideOpen)
            return true
        // The overlay window registers its menu (overlayMenu) while it is
        // created, so the in-window menu never opens underneath.
        if (Session.overlayAvailable && Session.showOverlay()) {
            window.guideOpen = true
            InputRouter.muteFor(250)       // the hold's release, stray presses
            SoundFx.play(SoundFx.Accept)
            return true
        }
        if (window.active || window.visibility !== Window.FullScreen) {
            window.guideOpen = true
            InputRouter.muteFor(250)
            SoundFx.play(SoundFx.Accept)
            return true
        }
        return false
    }

    function closeGuide() {
        if (!window.guideOpen)
            return
        window.guideOpen = false
        remoteCloseTimer.stop()
        window.remoteCloseArmed = false
        Session.hideOverlay()
    }

    function guideCloseApp() {
        const tracked = Launcher.tracked
        closeGuide()
        if (tracked)
            Launcher.stopTracking()
        else
            window.launchClosedByGuide = Launcher.terminate()
    }

    function guideStopTracking() {
        closeGuide()
        Launcher.stopTracking()
    }

    // Guide held while nothing can show the menu: the M5 behaviour.
    function _guideHeldCovered(source: int) {
        if (Launcher.tracked) {
            Launcher.stopTracking()
            return
        }
        if (window.launchClosable) {
            if (source === InputRouter.Remote) {
                if (Settings.tvRemote !== "full")
                    return
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
        window.guideHeldWhileRunning = true
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
        launchStartedAt = 0
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

    // "Leave MightyOS?": Stay is the default and Back; the TV remote can
    // only cancel (D18). Leaving ends the session (with autologin it
    // starts again).
    function confirmQuit() {
        dialog.show({
            title: qsTr("Leave MightyOS?"),
            message: qsTr("This ends the session."),
            buttons: [{ id: "stay", label: qsTr("Stay") },
                      { id: "leave", label: qsTr("Leave"), destructive: true }],
            acceptsRemote: false,
            onFinished: (answer) => {
                if (answer === "leave")
                    Session.quit()
            }
        })
    }

    function _returnFromLaunch() {
        closeGuide()
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
            // InputRouter logs it (mightyos.input).
            if (!Launcher.running)
                return
            const remote = source === InputRouter.Remote
            // D25: the remote's first Home on a closable launch arms "Press
            // Home again to close" (full mode only).
            const arm = remote && window.launchClosable && Settings.tvRemote === "full" && !Launcher.tracked
            if (window.guideOpen) {
                // Delivered as guideHeld (the input thread still saw the app
                // running): a second press.
                if (remote && window.remoteCloseArmed)
                    window.guideCloseApp()
                else
                    window.closeGuide()
                return
            }
            if (window.openGuide()) {
                if (arm) {
                    window.remoteCloseArmed = true
                    remoteCloseTimer.restart()
                }
                return
            }
            window._guideHeldCovered(source)
        }
        function onDeviceChanged(connected, name, source, initial) {
            if (initial || Launcher.running)
                return
            toast.show(connected ? qsTr("%1 connected").arg(name) : qsTr("%1 disconnected").arg(name), false)
        }
    }

    Connections {
        target: Session
        // Ctrl+Alt+Home / the Home media key (labwc keybind -> SIGUSR1).
        function onGuideRequested() {
            if (!Launcher.running)
                window.route(InputRouter.Guide, "")
            else if (window.guideOpen)
                window.closeGuide()
            else if (!window.openGuide())
                window._guideHeldCovered(InputRouter.Keyboard)
        }
        // Ctrl+Alt+End (SIGUSR2): close the running app now.
        function onCloseRequested() {
            if (Launcher.running)
                window.guideCloseApp()
        }
        // Ctrl+Alt+Backspace (SIGRTMIN+1): leave the session, if allowed.
        function onQuitRequested() {
            if (Library.restricted) {
                toast.show(qsTr("Ask a parent to leave MightyOS"), false)
                return
            }
            if (Launcher.running) {
                if (!window.openGuide())
                    window._guideHeldCovered(InputRouter.Keyboard)
                return
            }
            window.confirmQuit()
        }
        function onOverlayChanged() {
            // The overlay window went away underneath us (compositor closed
            // it, creation failed): don't leave input routed to nothing.
            if (window.guideOpen && Session.overlayAvailable && !Session.overlayShown && window.overlayMenu === null)
                window.closeGuide()
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
            window.launchStartedAt = Date.now()
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
                toast.show(qsTr("The Guide menu over games needs layer-shell-qt (see the README)"), false)
        }
        function onFailed(id, error) {
            if (Launcher.running)
                return   // a rejected extra launch must not touch the running app's state
            window._returnFromLaunch()
            toast.show(qsTr("Could not launch %1: %2").arg(window.launchTitle || id).arg(error), true)
        }
    }

    // Acknowledged (not shown again for this group set) only once shown.
    function _showGroupWarning() {
        toast.show(qsTr("This user is in the %1 group. A MightyOS TV user shouldn't be (see the README).")
                   .arg(Session.riskyGroups.join(", ")), false)
        Session.acknowledgeGroupWarning()
    }
    Timer {
        id: groupWarningTimer
        interval: Theme.toastDuration + 500
        onTriggered: window._showGroupWarning()
    }

    Timer {
        id: remoteCloseTimer
        interval: 1500
        onTriggered: window.remoteCloseArmed = false
    }

    // Dormant 1.5 s after the app started (the launch overlay has faded,
    // the app's window is up), awake at once when it exits or Guide opens.
    readonly property bool _dormantWanted: Launcher.running && window.launchStarted
                                           && !(window.guideOpen && window.overlayMenu === null)
    on_DormantWantedChanged: {
        if (_dormantWanted) {
            dormantTimer.restart()
        } else {
            dormantTimer.stop()
            window.dormant = false
        }
    }
    Timer {
        id: dormantTimer
        interval: 1500
        onTriggered: window.dormant = window._dormantWanted
    }

    // (f) The minute timer may be far off after suspend or a long game.
    onActiveChanged: {
        if (active)
            clock.refresh()
    }

    Component.onCompleted: {
        if (Session.safeMode) {
            toast.show(qsTr("Safe mode: MightyOS restarted after repeated crashes. Artwork and the TV remote are off."), true)
            if (Session.groupWarning)
                groupWarningTimer.start()   // after the safe-mode toast
        } else if (Session.groupWarning) {
            window._showGroupWarning()
        }
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
                visible: !window.dormant

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
                Rectangle {
                    id: safeModeBanner
                    objectName: "safeModeBanner"
                    visible: Session.safeMode
                    anchors.right: profileChip.visible ? profileChip.left : clock.left
                    anchors.rightMargin: Theme.spacingL
                    anchors.verticalCenter: parent.verticalCenter
                    width: safeModeLabel.implicitWidth + 2 * Theme.spacingM
                    height: safeModeLabel.implicitHeight + Theme.spacingS
                    radius: height / 2
                    color: Theme.surfaceHigh
                    border.width: Math.max(1, Theme.px(3))
                    border.color: Theme.danger
                    Text {
                        id: safeModeLabel
                        textFormat: Text.PlainText
                        anchors.centerIn: parent
                        text: qsTr("Safe mode")
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.DemiBold
                    }
                }
                Clock {
                    id: clock
                    active: !window.dormant
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
                visible: !picker.open && !window.dormant
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
            guideAvailable: Session.overlayAvailable
            launching: !window.launchStarted
            waiting: Launcher.waiting
            tracked: Launcher.tracked
            closable: window.launchClosable
            remoteCloseArmed: window.remoteCloseArmed
            title: window.launchTitle
            iconName: window.launchIcon
        }

        // The in-window Guide menu (no layer shell); above everything.
        GuideMenu {
            id: inWindowGuide
            objectName: "guideMenu"
            anchors.fill: parent
            z: 10
            open: window.guideOpen && window.overlayMenu === null
            title: window.launchTitle
            iconName: window.launchIcon
            closable: window.launchClosable
            tracked: Launcher.tracked
            waiting: Launcher.waiting
            remoteCloseArmed: window.remoteCloseArmed
            startedAt: window.launchStartedAt
            onResumeRequested: window.closeGuide()
            onCloseRequested: window.guideCloseApp()
            onStopTrackingRequested: window.guideStopTracking()
        }
    }
}
