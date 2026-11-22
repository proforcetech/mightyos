import QtQuick
import QtTest
import MightyOS.Core
import MightyOS.UI

// NavStack with the real screens: routing/bubbling, focus memory and
// restoring saved paths, including stale ones.
Item {
    id: root
    width: 1920
    height: 1080

    Component { id: homeScreen; HomeScreen {} }
    Component { id: systemListScreen; SystemListScreen {} }
    Component { id: gameGridScreen; GameGridScreen {} }
    Component { id: categoryScreen; CategoryScreen {} }

    Component {
        id: stackComponent
        NavStack {
            anchors.fill: parent
            profileId: "qmltest"     // own file under the test state dir
            registry: ({
                home: homeScreen,
                systems: systemListScreen,
                games: gameGridScreen,
                category: categoryScreen
            })
        }
    }

    TestCase {
        name: "NavStack"
        when: windowShown

        property NavStack stack: null

        function init() {
            InputRouter.blocked = false
            InputRouter.muteFor(0)
        }

        function cleanup() {
            if (stack) {
                stack.destroy()
                stack = null
            }
            NavState.flush()
        }

        function makeStack(): NavStack {
            stack = createTemporaryObject(stackComponent, root) as NavStack
            verify(stack)
            return stack
        }

        function entry(screen, params, row, index, key, extra) {
            return { screen: screen, params: params, row: row, index: index, key: key ?? "", extra: extra ?? {} }
        }

        function test_freshStartIsHome() {
            NavState.save([], "qmltest")
            makeStack().restore()
            compare(stack.depth, 1)
            compare(stack.current.screenId, "home")
        }

        function test_routingAndBubbling() {
            NavState.save([], "qmltest")
            makeStack().restore()
            // Home: Right moves the category bar, Accept opens Emulators.
            verify(stack.handleAction(InputRouter.Navigate, "right"))
            verify(stack.handleAction(InputRouter.Accept, ""))
            compare(stack.depth, 2)
            compare(stack.current.screenId, "systems")
            compare(stack.crumbs, ["MightyOS", "Emulators"])
            // SNES -> grid.
            stack.handleAction(InputRouter.Navigate, "right")
            stack.handleAction(InputRouter.Accept, "")
            compare(stack.depth, 3)
            compare(stack.current.title, "SNES")
            stack.handleAction(InputRouter.PageDown, "")
            const gridState = stack.current.saveState()
            verify(gridState.index > 0)
            // Back is not handled by the grid screen: it bubbles and pops,
            // and the system list is still on SNES.
            verify(stack.handleAction(InputRouter.Back, ""))
            compare(stack.depth, 2)
            compare(stack.current.saveState().key, "snes")
            // Back to Home; Back at Home's bar is unhandled.
            verify(stack.handleAction(InputRouter.Back, ""))
            compare(stack.depth, 1)
            verify(!stack.handleAction(InputRouter.Back, ""))
        }

        // From the system list (system grids use LB/RB to step systems,
        // see test_systemStepping).
        function test_categoryJumpUnwindsToHome() {
            NavState.save([
                entry("home", {}, 0, 1, "emulators", { category: "emulators" }),
                entry("systems", { category: "emulators" }, 0, 1, "snes")
            ], "qmltest")
            makeStack().restore()
            compare(stack.depth, 2)
            verify(stack.handleAction(InputRouter.NextCategory, ""))
            compare(stack.depth, 1)
            compare(stack.current.saveState().extra.category, "games")
        }

        function test_restoreDeepPath() {
            NavState.save([
                entry("home", {}, 1, 1, "snes", { category: "emulators" }),
                entry("systems", { category: "emulators" }, 0, 1, "snes"),
                entry("games", { system: "snes" }, 0, 23, "")
            ], "qmltest")
            makeStack().restore()
            compare(stack.depth, 3)
            compare(stack.current.saveState().index, 23)
            stack.pop()
            compare(stack.current.saveState().key, "snes")
            stack.pop()
            const home = stack.current.saveState()
            compare(home.row, 1)
            compare(home.key, "snes")
            compare(home.extra.category, "emulators")
        }

        function test_staleSystemTruncates() {
            NavState.save([
                entry("home", {}, 0, 1, "emulators", { category: "emulators" }),
                entry("systems", { category: "emulators" }, 0, 3, "ps1"),
                entry("games", { system: "uninstalled-console" }, 0, 3, "")
            ], "qmltest")
            makeStack().restore()
            compare(stack.depth, 2)
            compare(stack.current.saveState().key, "ps1")
            // The cleaned-up path is written back.
            stack.persistNow()
            compare(NavState.load("qmltest").length, 2)
        }

        function test_unknownScreenTruncates() {
            ignoreWarning(/NavStack: unknown screen/)
            NavState.save([
                entry("home", {}, 0, 2, "games", { category: "games" }),
                entry("profiles", {}, 0, 0, ""),
                entry("games", { system: "steam" }, 0, 0, "")
            ], "qmltest")
            makeStack().restore()
            compare(stack.depth, 1)
            compare(stack.current.saveState().extra.category, "games")
        }

        function test_staleKeyAndIndexAreClamped() {
            NavState.save([
                entry("home", {}, 0, 99, "no-such-category", {}),
            ], "qmltest")
            makeStack().restore()
            compare(stack.current.saveState().index, 5)        // last category
            NavState.save([
                entry("home", {}, 0, 0, "", { category: "emulators" }),
                entry("systems", { category: "emulators" }, 0, 500, "gone")
            ], "qmltest")
            stack.destroy()
            makeStack().restore()
            compare(stack.depth, 2)
            compare(stack.current.saveState().key, "arcade")   // last system
        }

        function test_pathMustStartAtHome() {
            NavState.save([entry("games", { system: "snes" }, 0, 3, "")], "qmltest")
            makeStack().restore()
            compare(stack.depth, 1)
            compare(stack.current.screenId, "home")
        }

        function test_optionsBubbleToContextMenuRequest() {
            NavState.save([
                entry("home", {}, 0, 1, "emulators", { category: "emulators" }),
                entry("systems", { category: "emulators" }, 0, 0, "nes"),
                entry("games", { system: "nes" }, 0, 0, "")
            ], "qmltest")
            makeStack().restore()
            const spy = createTemporaryObject(signalSpyComponent, root, { target: stack, signalName: "optionsRequested" })
            verify(stack.handleAction(InputRouter.Options, ""))
            compare(spy.count, 1)
            const items = spy.signalArguments[0][1]
            compare(items[0].id, "activate")
        }

        function deepSnesPath(index) {
            return [
                entry("home", {}, 0, 1, "emulators", { category: "emulators" }),
                entry("systems", { category: "emulators" }, 0, 1, "snes"),
                entry("games", { system: "snes" }, 0, index ?? 0, "")
            ]
        }

        // Review item 1: a screen animating out takes no input and its
        // signals no longer reach the stack.
        function test_leavingScreenIsInert() {
            NavState.save(deepSnesPath(0), "qmltest")
            makeStack().restore()
            compare(stack.depth, 3)
            verify(stack.current.enabled)
            verify(!stack.rootScreen.enabled)            // covered screens too
            const launchSpy = createTemporaryObject(signalSpyComponent, root,
                                                    { target: stack, signalName: "launchRequested" })
            const leaving = stack.current
            verify(stack.pop())
            verify(leaving.visible)                      // still animating out
            verify(!leaving.enabled)
            verify(!leaving.focusShown)
            // A quick click where the posters are, and late signals from it.
            mouseClick(leaving, leaving.width / 2, leaving.height * 0.8)
            leaving.launchRequested({ itemId: "rom:snes:x", label: "x", icon: "" })
            leaving.openRequested("games", { system: "nes" })
            compare(launchSpy.count, 0)
            compare(stack.depth, 2)
            verify(stack.current.enabled)
            tryVerify(() => !leaving.visible || leaving.opacity === 0, 1000)
        }

        function test_focusSurvivesReload() {
            NavState.save(deepSnesPath(0), "qmltest")
            makeStack().restore()
            stack.handleAction(InputRouter.Navigate, "right")
            stack.handleAction(InputRouter.Navigate, "down")
            stack.handleAction(InputRouter.PageDown, "")
            const before = stack.path()
            stack.persistNow()
            stack.destroy()
            makeStack().restore()
            compare(stack.depth, 3)
            const after = stack.path()
            compare(after.length, before.length)
            for (let i = 0; i < after.length; ++i) {
                compare(after[i].screen, before[i].screen)
                compare(after[i].index, before[i].index)
                compare(after[i].key, before[i].key)
            }
        }

        function test_restoreReplacesInsteadOfStacking() {
            NavState.save(deepSnesPath(4), "qmltest")
            makeStack().restore()
            compare(stack.depth, 3)
            stack.restore()                               // e.g. profile switch
            compare(stack.depth, 3)
            compare(stack.rootScreen.screenId, "home")
            NavState.save([], "qmltest")
            stack.restore()
            compare(stack.depth, 1)
        }

        // Review item 3: moving between items of the same kind must not
        // rebuild the prompt list.
        function test_promptsStableWhileMoving() {
            NavState.save(deepSnesPath(0), "qmltest")
            makeStack().restore()
            const gridSpy = createTemporaryObject(signalSpyComponent, root,
                                                  { target: stack.current, signalName: "promptsChanged" })
            for (let i = 0; i < 5; ++i)
                stack.handleAction(InputRouter.Navigate, "right")
            compare(gridSpy.count, 0)
            stack.popTo(1)
            const home = stack.current
            home.handleAction(InputRouter.Navigate, "down")       // row change: rebuild once
            const homeSpy = createTemporaryObject(signalSpyComponent, root,
                                                  { target: home, signalName: "promptsChanged" })
            for (let i = 0; i < 4; ++i)
                home.handleAction(InputRouter.Navigate, "right")
            compare(homeSpy.count, 0)
            compare(home.prompts[0].label, "Open")
        }

        // Review item 5: each category's content position survives a restart.
        function test_categoryPositionsPersist() {
            NavState.save([entry("home", {}, 0, 1, "emulators", { category: "emulators" })], "qmltest")
            makeStack().restore()
            const home = stack.current
            home.handleAction(InputRouter.Navigate, "down")
            for (let i = 0; i < 4; ++i)
                home.handleAction(InputRouter.Navigate, "right")
            compare(home.saveState().key, "ps2")
            home.handleAction(InputRouter.NextCategory, "")          // Games
            home.handleAction(InputRouter.NextCategory, "")          // Media
            home.handleAction(InputRouter.Navigate, "right")         // Jellyfin
            stack.persistNow()
            stack.destroy()
            makeStack().restore()
            const restored = stack.current
            compare(restored.saveState().extra.category, "media")
            compare(restored.saveState().index, 1)
            restored.handleAction(InputRouter.PrevCategory, "")
            restored.handleAction(InputRouter.PrevCategory, "")      // back to Emulators
            compare(restored.saveState().key, "ps2")
        }

        // Design change: LB/RB inside a system grid step systems in place.
        function test_systemStepping() {
            NavState.save(deepSnesPath(7), "qmltest")
            makeStack().restore()
            verify(stack.handleAction(InputRouter.NextCategory, ""))
            compare(stack.depth, 3)
            compare(stack.current.params.system, "n64")
            compare(stack.current.saveState().index, 0)
            compare(stack.crumbs, ["MightyOS", "Emulators", "N64"])
            compare(stack.current.prompts[stack.current.prompts.length - 1].label, "System")
            verify(stack.handleAction(InputRouter.PrevCategory, ""))
            verify(stack.handleAction(InputRouter.PrevCategory, ""))
            compare(stack.current.params.system, "nes")
            // First system: bump, nothing replaced.
            const nes = stack.current
            verify(stack.handleAction(InputRouter.PrevCategory, ""))
            compare(stack.current, nes)
            // The system list followed; Back lands on NES.
            verify(stack.handleAction(InputRouter.Back, ""))
            compare(stack.depth, 2)
            compare(stack.current.saveState().key, "nes")
            // Steam is alone in Games: both directions bump.
            NavState.save([entry("home", {}, 0, 2, "games", { category: "games" }),
                           entry("systems", { category: "games" }, 0, 0, "steam"),
                           entry("games", { system: "steam" }, 0, 0, "")], "qmltest")
            stack.restore()
            const steam = stack.current
            verify(stack.handleAction(InputRouter.NextCategory, ""))
            compare(stack.current, steam)
        }

        function test_favoritesGridKeepsGlobalCategoryJump() {
            NavState.save([entry("home", {}, 0, 0, "favorites", { category: "favorites" }),
                           entry("games", { collection: "favorites" }, 0, 0, "")], "qmltest")
            makeStack().restore()
            compare(stack.depth, 2)
            verify(stack.handleAction(InputRouter.NextCategory, ""))
            compare(stack.depth, 1)
            compare(stack.current.saveState().extra.category, "emulators")
        }

        Component { id: signalSpyComponent; SignalSpy {} }
    }
}
