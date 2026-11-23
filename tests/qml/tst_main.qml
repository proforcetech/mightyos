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
            for (let i = 0; i < 7; ++i)
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
    }
}
