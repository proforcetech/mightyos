import QtQuick
import QtTest
import MightyOS.Core
import MightyOS.UI

// The whole Main window: input routing through InputRouter, the launch
// cycle with the real test app (offscreen), the modal focus trap and the
// Dialog defaults. Actions enter via InputRouter.dispatch(), i.e. the same
// path the window event filter uses (the filter itself is covered by
// tst_inputrouter).
//
// `mightyosTestApp` is set by tests/tst_qml.cpp.
Item {
    id: root
    width: 100
    height: 100

    Component { id: mainComponent; Main {} }
    Component { id: spyComponent; SignalSpy {} }

    TestCase {
        name: "Main"
        when: windowShown

        property var win: null
        property NavStack stack: null
        property ContextMenu menu: null
        property Dialog dialog: null
        property Toast toast: null

        function testApp(args) {
            return [mightyosTestApp, "-platform", "offscreen"].concat(args)
        }

        function act(action, arg) {
            return InputRouter.dispatch(action, arg ?? "", InputRouter.Keyboard)
        }

        function init() {
            InputRouter.muteFor(0)
            // One profile (so no picker) and its saved path empty: Home.
            for (const p of ProfileService.profiles) {
                if (p.id !== ProfileService.activeId)
                    ProfileService.remove(p.id)
            }
            NavState.save([], ProfileService.activeId)
            win = createTemporaryObject(mainComponent, null,
                                        { launchCommand: testApp(["--quit-after", "300"]) })
            verify(win)
            stack = findChild(win, "navStack")
            menu = findChild(win, "contextMenu")
            dialog = findChild(win, "dialog")
            toast = findChild(win, "toast")
            verify(stack && menu && dialog && toast)
            compare(stack.depth, 1)
            tryVerify(() => stack.width > 0 && stack.height > 0)   // laid out
        }

        function cleanup() {
            tryVerify(() => !Launcher.running, 10000)
            InputRouter.muteFor(0)
        }

        function gridIndex() {
            return stack.current.saveState().index
        }

        function test_launchBlocksThenMutesThenRestoresFocus() {
            verify(stack.push("games", { system: "snes" }, false))
            for (let i = 0; i < 3; ++i)
                verify(act(InputRouter.Navigate, "right"))
            compare(gridIndex(), 3)

            verify(act(InputRouter.Accept))
            verify(Launcher.running)
            // Blocked while the app runs: consumed, not delivered.
            verify(InputRouter.blocked)
            verify(!act(InputRouter.Navigate, "right"))
            verify(!act(InputRouter.Back))
            compare(stack.depth, 1 + 1)
            // Something moves the focus behind the overlay...
            stack.current.restoreState({ row: 0, index: 10, key: "" })
            compare(gridIndex(), 10)

            tryVerify(() => !Launcher.running, 10000)
            // ...and on exit it is back on the launched tile.
            compare(gridIndex(), 3)
            verify(!InputRouter.blocked)
            // 300 ms mute right after exit.
            verify(InputRouter.muted)
            verify(!act(InputRouter.Navigate, "right"))
            compare(gridIndex(), 3)
            tryCompare(InputRouter, "muted", false, 2000)
            verify(act(InputRouter.Navigate, "right"))
            compare(gridIndex(), 4)
            verify(!toast.shown)                         // clean exit: no toast
        }

        function test_crashShowsToastAndMutes() {
            win.launchCommand = testApp(["--crash"])
            verify(stack.push("games", { system: "nes" }, false))
            verify(act(InputRouter.Accept))
            tryVerify(() => !Launcher.running, 10000)
            verify(InputRouter.muted)
            verify(toast.shown)
            verify(toast.error)
            verify(toast.message.indexOf("crashed") >= 0, toast.message)
            compare(gridIndex(), 0)
        }

        function test_contextMenuTrapsFocus() {
            act(InputRouter.Navigate, "right")           // Emulators
            act(InputRouter.Navigate, "down")            // content row: NES
            const before = JSON.stringify(stack.path())
            verify(act(InputRouter.Options))
            verify(menu.open)
            compare(menu.currentIndex, 0)
            act(InputRouter.Navigate, "down")
            act(InputRouter.Navigate, "right")
            act(InputRouter.NextCategory)
            act(InputRouter.PageDown)
            compare(menu.currentIndex, 1)
            compare(JSON.stringify(stack.path()), before)
            act(InputRouter.Back)
            verify(!menu.open)
            compare(stack.depth, 1)                      // Back closed the menu only
            compare(JSON.stringify(stack.path()), before)
        }

        function test_dialogDefaultsToCancel() {
            // Settings -> Power (the last tile) opens the confirmation dialog.
            for (let i = 0; i < 5; ++i)
                act(InputRouter.NextCategory)
            act(InputRouter.Navigate, "down")
            for (let i = 0; i < 8; ++i)
                act(InputRouter.Navigate, "right")
            compare(stack.current.saveState().key, "settings:power")
            const spy = createTemporaryObject(spyComponent, root, { target: dialog, signalName: "finished" })
            verify(act(InputRouter.Accept))
            verify(dialog.open)
            compare(dialog.buttons[dialog.currentIndex].id, "cancel")
            // Navigation stays in the dialog; Back answers with cancel.
            act(InputRouter.Navigate, "up")
            act(InputRouter.NextCategory)
            act(InputRouter.Back)
            verify(!dialog.open)
            compare(spy.count, 1)
            compare(spy.signalArguments[0][0], "cancel")
            compare(stack.current.saveState().key, "settings:power")   // nothing else moved

            // The safe button is the default even when listed second.
            dialog.show({ title: "Erase?", buttons: [{ id: "erase", label: "Erase", destructive: true },
                                                     { id: "keep", label: "Keep" }] })
            compare(dialog.currentIndex, 1)
            act(InputRouter.Accept)
            compare(spy.signalArguments[1][0], "keep")
        }

        function homeState() {
            return stack.current.saveState()
        }

        function test_guideGoesHomeFromAnywhere() {
            // Deep in a grid, a context menu open: Guide closes the menu and
            // lands on Home > Favorites, on the category bar.
            act(InputRouter.NextCategory)                 // Emulators
            act(InputRouter.Navigate, "down")
            verify(stack.push("games", { system: "snes" }, false))
            act(InputRouter.Navigate, "right")
            verify(act(InputRouter.Options))
            verify(menu.open)
            verify(act(InputRouter.Guide))
            verify(!menu.open)
            compare(stack.depth, 1)
            const state = homeState()
            compare(state.row, 0)
            compare(state.key, "favorites")
            compare(state.extra.category, "favorites")
            // Emulators still remembers its position.
            act(InputRouter.NextCategory)
            compare(homeState().extra.category, "emulators")

            // A dialog is answered with its safe button.
            const spy = createTemporaryObject(spyComponent, root, { target: dialog, signalName: "finished" })
            dialog.show({ title: "Erase?", buttons: [{ id: "keep", label: "Keep" },
                                                     { id: "erase", label: "Erase", destructive: true }] })
            InputRouter.dispatch(InputRouter.Guide, "", InputRouter.Gamepad)
            verify(!dialog.open)
            compare(spy.signalArguments[0][0], "keep")
            compare(homeState().key, "favorites")
        }

        function test_guideWhileRunning() {
            const heldSpy = createTemporaryObject(spyComponent, root,
                                                  { target: InputRouter, signalName: "guideHeld" })
            verify(stack.push("games", { system: "nes" }, false))
            verify(act(InputRouter.Accept))
            verify(Launcher.running)
            // A short Guide (and everything else) is blocked while it runs.
            verify(!InputRouter.dispatch(InputRouter.Guide, "", InputRouter.Gamepad))
            compare(stack.depth, 2)
            // Held: the signal gets through (Guide overlay in M6).
            InputRouter.postGuideHeld(InputRouter.Gamepad)
            tryCompare(heldSpy, "count", 1)
            verify(win.guideHeldWhileRunning)
            tryVerify(() => !Launcher.running, 10000)
            compare(stack.depth, 2)                      // nothing moved
            verify(toast.shown)
            verify(toast.message.indexOf("M6") >= 0, toast.message)
            verify(!win.guideHeldWhileRunning)
        }

        function openControllers() {
            for (let i = 0; i < 5; ++i)
                act(InputRouter.NextCategory)            // Settings
            act(InputRouter.Navigate, "down")
            for (let i = 0; i < 5; ++i)
                act(InputRouter.Navigate, "right")
            compare(homeState().key, "settings:controllers")
            verify(act(InputRouter.Accept))
            verify(dialog.open)
        }

        function test_controllersDialogTogglesRumble() {
            const before = Settings.rumble
            openControllers()
            compare(dialog.buttons[dialog.currentIndex].id, "close")   // safe default
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            verify(!dialog.open)
            compare(Settings.rumble, !before)
            compare(InputRouter.rumbleEnabled, Settings.rumble)
            // And back.
            act(InputRouter.Accept)
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            compare(Settings.rumble, before)
        }

        function test_controllersDialogTvRemoteLevel() {
            Settings.tvRemote = "navigation"
            compare(InputRouter.remoteMode, InputRouter.RemoteNavigation)
            openControllers()
            // The remote can't raise its own trust level: only cancel.
            verify(!InputRouter.acceptRemote)
            verify(!InputRouter.dispatch(InputRouter.Navigate, "right", InputRouter.Remote))
            verify(!InputRouter.dispatch(InputRouter.Accept, "", InputRouter.Remote))
            verify(dialog.open)
            // The pad (or keyboard) can: navigation -> full -> off -> navigation.
            act(InputRouter.Navigate, "right")
            act(InputRouter.Navigate, "right")
            compare(dialog.buttons[dialog.currentIndex].id, "remote")
            act(InputRouter.Accept)
            compare(Settings.tvRemote, "full")
            compare(InputRouter.remoteMode, InputRouter.RemoteFull)
            verify(InputRouter.acceptRemote)              // dialog closed
            act(InputRouter.Accept)
            act(InputRouter.Navigate, "right")
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            compare(Settings.tvRemote, "off")
            verify(!InputRouter.dispatch(InputRouter.Navigate, "left", InputRouter.Remote))
            act(InputRouter.Accept)
            act(InputRouter.Navigate, "right")
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            compare(Settings.tvRemote, "navigation")
        }

        function test_remoteNavigationModeAndRestrictedDialogs() {
            Settings.tvRemote = "navigation"
            // Navigation only: moving and opening work, Favorite/Options don't.
            verify(InputRouter.dispatch(InputRouter.NextCategory, "", InputRouter.Remote))
            verify(InputRouter.dispatch(InputRouter.Navigate, "down", InputRouter.Remote))
            verify(!InputRouter.dispatch(InputRouter.Options, "", InputRouter.Remote))
            verify(!menu.open)
            verify(!InputRouter.dispatch(InputRouter.Favorite, "", InputRouter.Remote))
            // Settings > Power: the remote may open it, but only cancel it.
            for (let i = 0; i < 4; ++i)
                act(InputRouter.NextCategory)
            for (let i = 0; i < 8; ++i)
                act(InputRouter.Navigate, "right")
            compare(homeState().key, "settings:power")
            verify(InputRouter.dispatch(InputRouter.Accept, "", InputRouter.Remote))
            verify(dialog.open)
            verify(!InputRouter.acceptRemote)
            verify(!InputRouter.dispatch(InputRouter.Navigate, "right", InputRouter.Remote))
            verify(!InputRouter.dispatch(InputRouter.Accept, "", InputRouter.Remote))
            compare(dialog.buttons[dialog.currentIndex].id, "cancel")
            verify(InputRouter.dispatch(InputRouter.Back, "", InputRouter.Remote))
            verify(!dialog.open)
            verify(InputRouter.acceptRemote)
            // An ordinary dialog accepts the remote.
            dialog.show({ title: "Info", buttons: [{ id: "ok", label: "OK" }] })
            verify(InputRouter.acceptRemote)
            verify(InputRouter.dispatch(InputRouter.Accept, "", InputRouter.Remote))
            verify(!dialog.open)
            act(InputRouter.Navigate, "left")             // keyboard as last source again
        }

        function textItems(item, out) {
            if (item instanceof Text)
                out.push(item)
            const kids = item.children ?? []
            for (let i = 0; i < kids.length; ++i)
                textItems(kids[i], out)
            return out
        }

        function test_everyTextIsPlain() {
            // Untrusted strings (device and app names) must never render as
            // rich text: every Text in the window uses Text.PlainText.
            openControllers()
            toast.show("x", false)
            const texts = textItems(win.contentItem, [])
            verify(texts.length > 30, texts.length)
            for (const t of texts)
                compare(t.textFormat, Text.PlainText, t.text)
            act(InputRouter.Back)
        }

        function test_markupInNamesIsShownLiterally() {
            const evil = "<img src=x><b>x</b>"
            InputRouter.postDeviceNotice(true, 777, evil, InputRouter.Gamepad, false)
            tryCompare(toast, "message", evil + " connected")
            const label = textItems(toast, [])[0]
            compare(label.text, evil + " connected")
            compare(label.textFormat, Text.PlainText)
            // Plain text: the markup takes room as characters, it isn't eaten
            // (what Text.AutoText / RichText would have done).
            const rich = createTemporaryQmlObject("import QtQuick; Text { textFormat: Text.RichText }", root)
            rich.font = label.font
            rich.text = label.text
            verify(rich.implicitWidth < label.implicitWidth, rich.implicitWidth + " vs " + label.implicitWidth)
            InputRouter.postDeviceNotice(false, 777, evil, InputRouter.Gamepad, false)
            tryCompare(toast, "message", evil + " disconnected")
        }

        function test_rumbleOnBumpAndErrorOnlyForGamepad() {
            const spy = createTemporaryObject(spyComponent, root,
                                              { target: InputRouter, signalName: "rumbleRequested" })
            Settings.rumble = true
            // Bump at the left end of the category bar, from a gamepad.
            InputRouter.dispatch(InputRouter.Navigate, "left", InputRouter.Gamepad)
            compare(spy.count, 1)
            compare(spy.signalArguments[0][0], InputRouter.Bump)
            // The same bump from the keyboard: no rumble.
            act(InputRouter.Navigate, "left")
            compare(spy.count, 1)
            // An error toast after gamepad input rumbles (Error). (Wait for
            // the bump animation: a bump during one is silent.)
            wait(Theme.animFast * 3)
            InputRouter.dispatch(InputRouter.Navigate, "left", InputRouter.Gamepad)
            compare(spy.count, 2)
            toast.show("Something failed", true)
            compare(spy.count, 3)
            compare(spy.signalArguments[2][0], InputRouter.Error)
            toast.show("Just info", false)
            compare(spy.count, 3)
            // Off in Settings: none.
            Settings.rumble = false
            wait(Theme.animFast * 3)
            InputRouter.dispatch(InputRouter.Navigate, "left", InputRouter.Gamepad)
            compare(spy.count, 3)
            Settings.rumble = true
            act(InputRouter.Navigate, "left")              // leave the keyboard as last source
        }

        function test_deviceToasts() {
            InputRouter.postDeviceNotice(true, 4242, "Test Pad", InputRouter.Gamepad, true)
            wait(20)
            verify(!toast.shown)                          // present at startup: quiet
            InputRouter.postDeviceNotice(false, 4242, "Test Pad", InputRouter.Gamepad, false)
            tryVerify(() => toast.shown)
            compare(toast.message, "Test Pad disconnected")
            InputRouter.postDeviceNotice(true, 4243, "Other Pad", InputRouter.Gamepad, false)
            tryCompare(toast, "message", "Other Pad connected")
            InputRouter.postDeviceNotice(false, 4243, "Other Pad", InputRouter.Gamepad, false)
            tryCompare(toast, "message", "Other Pad disconnected")
        }
    }
}
