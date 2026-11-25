import QtQuick
import QtTest
import MightyOS.Core
import MightyOS.UI

// M5 in the whole Main window: Home > Media, the Media screen's rows and
// focus memory, launching a web app without a browser (install hint),
// Settings > Media adding / hiding tiles, and Guide held closing a
// closable launch (web app / stream) through Launcher.terminate().
Item {
    id: root
    width: 100
    height: 100

    Component { id: mainComponent; Main {} }

    TestCase {
        name: "Media"
        when: windowShown

        property var win: null
        property NavStack stack: null
        property Dialog dialog: null
        property Toast toast: null

        function act(action, arg) {
            return InputRouter.dispatch(action, arg ?? "", InputRouter.Keyboard)
        }

        function init() {
            InputRouter.muteFor(0)
            for (const p of ProfileService.profiles) {
                if (p.id !== ProfileService.activeId)
                    ProfileService.remove(p.id)
            }
            NavState.save([], ProfileService.activeId)
            win = createTemporaryObject(mainComponent, null, {
                launchCommand: [mightyosTestApp, "-platform", "offscreen", "--quit-after", "60000"] })
            verify(win)
            stack = findChild(win, "navStack")
            dialog = findChild(win, "dialog")
            toast = findChild(win, "toast")
            tryVerify(() => stack.width > 0 && stack.height > 0)
            tryVerify(() => AppRegistry.ready && Media.ready, 5000)
        }

        function cleanup() {
            if (Launcher.running && !Launcher.tracked)
                Launcher.terminate()   // untracked only (M3)
            tryVerify(() => !Launcher.running, 10000)
            InputRouter.muteFor(0)
        }

        function test_homeMediaRowAndScreen() {
            // Home > Media shows the built-in streaming tiles (no browser here).
            for (let i = 0; i < 3; ++i)
                act(InputRouter.NextCategory)
            compare(stack.current.saveState().key, "media")
            verify(Media.home.indexOfKey("webapp:netflix") >= 0)
            verify(act(InputRouter.Accept))
            compare(stack.current.screenId, "media")
            const screen = stack.current
            // Rows: Apps first if there are any (other tests install a media app), then Streaming.
            compare(screen.focusRow, Media.apps.count > 0 ? 0 : 1)
            if (screen.focusRow === 0)
                verify(act(InputRouter.Navigate, "down"))
            compare(screen.focusRow, 1)
            compare(screen.focusedItem.key, "webapp:youtube")
            verify(screen.focusedCanFavorite)
            act(InputRouter.Navigate, "right")
            compare(screen.focusedItem.key, "webapp:netflix")
            compare(screen.focusedItem.subtitle, "Needs Google Chrome")
            // Everything fits on screen at 1280x720.
            const hosts = findChild(screen, "mediaWebApps")
            verify(hosts.mapToItem(screen, 0, hosts.height).y <= screen.height)
            // Launching without a browser: an install hint, nothing runs.
            verify(act(InputRouter.Accept))
            verify(!Launcher.running)
            verify(toast.shown)
            verify(toast.message.indexOf("Google Chrome") >= 0, toast.message)
            // Saved path round trip (restart): back on Netflix in the Streaming row.
            const saved = stack.current.saveState()
            compare(saved.row, 1)
            compare(saved.key, "webapp:netflix")
            verify(act(InputRouter.Back))
            verify(act(InputRouter.Accept))
            stack.current.restoreState(saved)
            compare(stack.current.focusRow, 1)
            compare(stack.current.focusedItem.key, "webapp:netflix")
            // Favorites: web apps can be pinned.
            verify(act(InputRouter.Favorite))
            verify(Favorites.isFavorite("webapp:netflix"))
            compare(Favorites.model.get(Favorites.indexOf("webapp:netflix")).kind, "webapp")
            verify(act(InputRouter.Favorite))
            verify(!Favorites.isFavorite("webapp:netflix"))
        }

        function test_settingsAddAndHideWebApp() {
            verify(stack.push("mediasettings", {}, false))
            const screen = stack.current
            const row = findChild(screen, "manageRow")
            compare(screen.focusedItem.key, "addwebapp")
            verify(act(InputRouter.Accept))
            verify(screen.editing)
            const name = findChild(screen, "webAppName")
            const address = findChild(screen, "webAppAddress")
            tryVerify(() => name.activeFocus)
            name.text = "Jelly"
            verify(act(InputRouter.Accept))              // Next
            tryVerify(() => address.activeFocus)
            address.text = "http://jelly.example.org/"
            verify(act(InputRouter.Accept))              // rejected: not https
            verify(screen.editing)
            verify(toast.message.indexOf("https") >= 0, toast.message)
            address.text = "https://jelly.example.org/web/"
            verify(act(InputRouter.Accept))              // Save
            verify(!screen.editing)
            compare(screen.focusedItem.key, "webapp:user-jelly")
            verify(Media.webApps.indexOfKey("webapp:user-jelly") >= 0)
            verify(testFiles.exists("config", "mightyos/webapps.d/user-jelly.json"))
            // Remove it again through the dialog (default: Close).
            verify(act(InputRouter.Accept))
            verify(dialog.open)
            compare(dialog.buttons[0].id, "close")
            dialog.handleAction(InputRouter.Navigate, "right")
            dialog.handleAction(InputRouter.Accept, "")
            verify(!dialog.open)
            compare(Media.webApps.indexOfKey("webapp:user-jelly"), -1)
            // Hide a built-in, then show it again.
            row.restoreFocus(Media.manage.indexOfKey("webapp:twitch"), "webapp:twitch")
            verify(act(InputRouter.Accept))
            dialog.handleAction(InputRouter.Navigate, "right")
            dialog.handleAction(InputRouter.Accept, "")
            compare(Media.webApps.indexOfKey("webapp:twitch"), -1)
            verify(act(InputRouter.Accept))
            dialog.handleAction(InputRouter.Navigate, "right")
            dialog.handleAction(InputRouter.Accept, "")
            verify(Media.webApps.indexOfKey("webapp:twitch") >= 0)
        }

        function test_guideHeldClosesAClosableLaunch() {
            verify(stack.push("games", { system: "nes" }, false))
            verify(act(InputRouter.Accept))
            verify(Launcher.running)
            tryVerify(() => win.launchStarted, 5000)
            win.launchClosable = true                    // as for a web app / stream
            InputRouter.postGuideHeld(InputRouter.Gamepad)
            tryVerify(() => win.guideOpen)
            InputRouter.muteFor(0)
            // Close needs no confirmation for a web app / stream (D25).
            verify(InputRouter.dispatch(InputRouter.Navigate, "down", InputRouter.Gamepad))
            verify(InputRouter.dispatch(InputRouter.Accept, "", InputRouter.Gamepad))
            verify(!win.guideOpen)
            tryVerify(() => !Launcher.running, 6000)     // terminated, not after 60 s
            verify(toast.shown)
            verify(toast.message.indexOf("Closed") >= 0, toast.message)
            verify(!win.launchClosable)
        }

        function test_forgetMissingNeedsMissingGames() {
            // The mock library has no missing games: no "Forget" option.
            verify(stack.push("systems", { category: "emulators" }, false))
            const items = stack.current.optionsFor()
            verify(items.some(i => i.id === "scan") || Library.mock)
            verify(!items.some(i => i.id === "forgetMissing"))
        }

        function test_moonlightPathRestoredBeforeReady() {
            // H1: a saved path into a Moonlight host survives the startup
            // window before Moonlight's settings are read, and isn't
            // written back truncated meanwhile.
            const host = "moonlight:8d8c1f3a-2b7e-4c11-9a51-0123456789ab"
            const path = [
                { screen: "home", params: {}, row: 0, index: 3, key: "media", extra: { category: "media" } },
                { screen: "media", params: {}, row: 2, index: 0, key: host, extra: {} },
                { screen: "moonlight", params: { host: host }, row: 0, index: 1, key: host + "/Desktop", extra: {} }
            ]
            Media.unloadMoonlight()
            verify(!Media.ready)
            NavState.save(path, ProfileService.activeId)
            stack.restore()
            compare(stack.depth, 3)
            compare(stack.current.screenId, "moonlight")
            compare(stack.current.title, "Game streaming")
            compare(stack.current.saveState().key, host + "/Desktop")   // still pending
            NavState.flush()
            compare(NavState.load(ProfileService.activeId).length, 3)
            Media.reload()
            tryVerify(() => Media.ready, 5000)
            // No such host in the test env: the screen stays and says so.
            compare(stack.depth, 3)
            verify(!stack.current.valid)
        }

        function test_mediaFocusFollowsRequestedRow() {
            // M1: a restored row that fills in later gets the focus, unless
            // the user moved meanwhile; an emptied row is never kept.
            const baseId = ProfileService.activeId
            const kid = ProfileService.create("Rowkid", "cat", "#ff6b8b", "kids")
            verify(kid.length > 0)
            verify(testFiles.writeConfig("mightyos/profiles/" + kid + "/allowed.json",
                                         JSON.stringify({ version: 1, apps: ["app:m1test", "webapp:youtube"] })))
            testFiles.remove("mightyos/apps/m1test.json")
            const manifest = JSON.stringify({ version: 1, id: "m1test", name: "M1 Test", category: "media",
                                              exec: [mightyosTestApp] })
            AppRegistry.rescan()
            tryVerify(() => !AppRegistry.scanning, 5000)
            win.enterProfile(kid)
            compare(Media.apps.count, 0)
            compare(Media.webApps.count, 1)
            verify(stack.push("media", {}, false))
            const screen = stack.current
            screen.restoreState({ row: 0, index: 0, key: "app:m1test", extra: {} })
            compare(screen.focusRow, 1)                  // apps empty: Streaming for now
            verify(testFiles.write("mightyos/apps/m1test.json", manifest))
            AppRegistry.rescan()
            tryCompare(screen, "focusRow", 0, 5000)
            compare(screen.focusedItem.key, "app:m1test")
            // Emptied under the focus: off the empty row.
            verify(testFiles.remove("mightyos/apps/m1test.json"))
            AppRegistry.rescan()
            tryCompare(Media.apps, "count", 0, 5000)
            compare(screen.focusRow, 1)
            // The user moved: a row filling in later doesn't pull the focus.
            screen.restoreState({ row: 0, index: 0, key: "app:m1test", extra: {} })
            act(InputRouter.Navigate, "right")
            verify(testFiles.write("mightyos/apps/m1test.json", manifest))
            AppRegistry.rescan()
            tryCompare(Media.apps, "count", 1, 5000)
            compare(screen.focusRow, 1)
            testFiles.remove("mightyos/apps/m1test.json")
            AppRegistry.rescan()
            tryVerify(() => !AppRegistry.scanning, 5000)
            win.enterProfile(baseId)
        }

        function test_tvRemoteNeedsTwoHomePressesInFullMode() {
            // S2: "navigation only" can't close; "full" needs Home twice within 1.5 s.
            const before = Settings.tvRemote
            verify(stack.push("games", { system: "nes" }, false))
            verify(act(InputRouter.Accept))
            tryVerify(() => win.launchStarted, 5000)
            win.launchClosable = true
            // Navigation only: Home opens the Guide menu, which the remote can
            // only leave (Back / Home); it never arms closing.
            Settings.tvRemote = "navigation"
            InputRouter.postGuideHeld(InputRouter.Remote)
            tryVerify(() => win.guideOpen, 1000)
            verify(!win.remoteCloseArmed)
            InputRouter.muteFor(0)
            verify(!InputRouter.dispatch(InputRouter.Accept, "", InputRouter.Remote))
            verify(InputRouter.dispatch(InputRouter.Guide, "", InputRouter.Remote))
            verify(!win.guideOpen)
            verify(Launcher.running)
            // Full: the first Home opens the menu with "Press Home again to close".
            Settings.tvRemote = "full"
            InputRouter.postGuideHeld(InputRouter.Remote)
            tryVerify(() => win.remoteCloseArmed, 1000)
            verify(win.guideOpen)
            verify(Launcher.running)
            tryVerify(() => !win.remoteCloseArmed, 2500)  // too slow: disarmed
            verify(Launcher.running)
            InputRouter.muteFor(0)
            verify(InputRouter.dispatch(InputRouter.Guide, "", InputRouter.Remote))   // just resumes now
            verify(!win.guideOpen)
            verify(Launcher.running)
            InputRouter.postGuideHeld(InputRouter.Remote)
            tryVerify(() => win.remoteCloseArmed, 1000)
            // The second Home arrives as a Guide press (the menu has input).
            InputRouter.muteFor(0)
            verify(InputRouter.dispatch(InputRouter.Guide, "", InputRouter.Remote))
            tryVerify(() => !Launcher.running, 6000)
            verify(toast.message.indexOf("Closed") >= 0, toast.message)
            Settings.tvRemote = before
        }
    }
}
