import QtQuick
import QtTest
import MightyOS.Core
import MightyOS.UI

// Profiles, favorites and history through the whole Main window: the
// startup rule (picker only with 2+ profiles), switching resets the stack
// and restores the profile's path, X toggles favorites that then appear
// (in order) in Favorites, reordering, a launch fills the Recent row, and
// create / edit / delete in the picker.
//
// `mightyosTestApp` and `testFiles` come from tests/tst_qml.cpp.
Item {
    id: root
    width: 100
    height: 100

    Component { id: mainComponent; Main {} }

    TestCase {
        name: "Profiles"
        when: windowShown

        property var win: null
        property NavStack stack: null
        property ProfilePicker picker: null
        property ContextMenu menu: null
        property Dialog dialog: null
        property Toast toast: null
        property string baseId

        readonly property string appKey: "app:testapp"

        function act(action, arg) {
            return InputRouter.dispatch(action, arg ?? "", InputRouter.Keyboard)
        }

        function init() {
            InputRouter.blocked = false
            InputRouter.muteFor(0)
            baseId = ProfileService.activeId
            for (const p of ProfileService.profiles) {
                if (p.id !== baseId)
                    ProfileService.remove(p.id)
            }
            ProfileService.select(baseId)
            verify(ProfileService.rename(baseId, "Base"))     // names below never clash
            for (const id of Favorites.ids())
                Favorites.remove(id)
            NavState.save([], baseId)
        }

        function cleanup() {
            tryVerify(() => !Launcher.running, 10000)
            destroyMain()
            NavState.flush()
            InputRouter.muteFor(0)
        }

        // destroy() is deferred; a Main that still exists keeps routing
        // InputRouter actions to its own screens. Wait until it is gone.
        function destroyMain() {
            if (!win)
                return
            let gone = false
            win.Component.destruction.connect(() => { gone = true })
            win.destroy()
            win = null
            tryVerify(() => gone, 2000)
        }

        function makeMain() {
            win = createTemporaryObject(mainComponent, null, {
                launchCommand: [mightyosTestApp, "-platform", "offscreen", "--quit-after", "300"]
            })
            verify(win)
            stack = findChild(win, "navStack")
            picker = findChild(win, "profilePicker")
            menu = findChild(win, "contextMenu")
            dialog = findChild(win, "dialog")
            toast = findChild(win, "toast")
            verify(stack && picker && menu && dialog && toast)
            tryVerify(() => stack.width > 0 && stack.height > 0)
            return win
        }

        function profileRow() {
            return findChild(picker, "profileRow")
        }

        // Picks the context-menu entry with `id`.
        function chooseOption(id) {
            verify(act(InputRouter.Options))
            verify(menu.open, "options menu did not open")
            const index = menu.items.findIndex(item => item.id === id)
            verify(index >= 0, "no option " + id + " in " + JSON.stringify(menu.items))
            menu.currentIndex = index
            act(InputRouter.Accept)
            verify(!menu.open)
        }

        function installTestApp() {
            verify(testFiles.write("mightyos/apps/testapp.json", JSON.stringify({
                version: 1, id: "testapp", name: "Test App", category: "media",
                exec: [mightyosTestApp, "-platform", "offscreen", "--quit-after", "300"]
            })))
            AppRegistry.rescan()
            tryVerify(() => AppRegistry.contains(appKey), 5000)
        }

        function homeOnFavorites() {
            stack.popTo(1)
            stack.current.restoreState({ row: 0, index: 0, key: "favorites", extra: { category: "favorites" } })
            compare(stack.current.saveState().extra.category, "favorites")
        }

        function test_singleProfileGoesStraightHome() {
            compare(ProfileService.count, 1)
            makeMain()
            verify(!picker.open)
            compare(stack.depth, 1)
            compare(stack.current.screenId, "home")
            compare(stack.profileId, baseId)
        }

        function test_twoProfilesShowPickerWithLastUsed() {
            const other = ProfileService.create("Comet", "cat", "#ff6b8b", "kids")
            verify(other.length > 0)
            ProfileService.select(baseId)                     // last used
            makeMain()
            verify(picker.open)
            verify(picker.startup)
            compare(stack.depth, 0)                           // nothing behind the picker yet
            compare(profileRow().currentEntry().key, baseId)  // preselected
            compare(profileRow().model.get(1).badge, "Kids")
            act(InputRouter.Back)                             // at startup: must choose
            verify(picker.open)
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            verify(!picker.open)
            compare(ProfileService.activeId, other)
            compare(stack.profileId, other)
            compare(stack.depth, 1)

            // Next start: the kids profile was used last, so it starts
            // directly (S2: the picker would let anyone leave it without
            // the PIN), and the picker can't be opened from it.
            destroyMain()
            makeMain()
            verify(!picker.open)
            compare(stack.profileId, other)
            win.openProfilePicker()
            verify(!picker.open)
            // A standard profile used last: the picker preselects it.
            ProfileService.select(baseId)
            destroyMain()
            makeMain()
            verify(picker.open)
            compare(profileRow().currentEntry().key, baseId)
        }

        function test_switchResetsStackAndRestoresPath() {
            const other = ProfileService.create("Comet", "cat", "#ff6b8b")
            verify(other.length > 0)
            ProfileService.select(baseId)
            makeMain()
            act(InputRouter.Accept)                            // choose the preselected base profile
            verify(!picker.open)

            // Home's options menu offers "Switch profile".
            chooseOption("profiles")
            verify(picker.open)
            verify(!picker.startup)
            act(InputRouter.Back)
            verify(!picker.open)

            verify(stack.push("games", { system: "snes" }, false))
            for (let i = 0; i < 3; ++i)
                act(InputRouter.Navigate, "right")
            compare(stack.current.saveState().index, 3)

            win.openProfilePicker()                            // e.g. the top-bar profile chip
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            verify(!picker.open)
            compare(stack.profileId, other)
            compare(stack.depth, 1)                            // reset: a fresh path for the new profile
            verify(toast.shown)
            verify(stack.push("games", { system: "n64" }, false))

            // Back to the first profile: its path and focus come back.
            win.openProfilePicker()
            act(InputRouter.Navigate, "left")
            act(InputRouter.Accept)
            compare(stack.profileId, baseId)
            compare(stack.depth, 2)
            compare(stack.current.params.system, "snes")
            compare(stack.current.saveState().index, 3)

            // And the other one kept its own.
            win.openProfilePicker()
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            compare(stack.depth, 2)
            compare(stack.current.params.system, "n64")
        }

        // Review #1: switching must not rewrite the previous profile's
        // saved path (Favorites resets under live Home screens).
        function test_switchKeepsOtherProfilesPath() {
            const other = ProfileService.create("Comet", "cat", "#ff6b8b")
            verify(other.length > 0)
            const games = Library.games("snes")
            verify(Favorites.add(games.keyAt(0)))
            verify(Favorites.add(games.keyAt(1)))
            ProfileService.select(other)
            verify(Favorites.add(games.keyAt(5)))     // a different single favorite
            ProfileService.select(baseId)
            makeMain()
            act(InputRouter.Accept)                   // base profile
            homeOnFavorites()
            act(InputRouter.Navigate, "down")
            act(InputRouter.Navigate, "right")        // second favorite
            compare(stack.current.saveState().key, games.keyAt(1))
            stack.persistNow()
            const before = JSON.stringify(NavState.load(baseId))

            win.openProfilePicker()
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)                   // -> other
            compare(stack.profileId, other)
            NavState.flush()
            compare(JSON.stringify(NavState.load(baseId)), before)

            win.openProfilePicker()
            act(InputRouter.Navigate, "left")
            act(InputRouter.Accept)                   // back
            compare(stack.current.saveState().row, 1)
            compare(stack.current.saveState().key, games.keyAt(1))
        }

        // Review #2: deleting the profile on screen leaves nothing behind.
        function test_deletingActiveProfileLeavesNoDirs() {
            const other = ProfileService.create("Comet", "cat", "#ff6b8b")
            ProfileService.select(baseId)
            verify(Favorites.add(Library.games("snes").keyAt(0)))
            makeMain()
            act(InputRouter.Accept)
            homeOnFavorites()
            act(InputRouter.Navigate, "down")         // focus inside the Favorites row
            stack.persistNow()
            verify(testFiles.exists("state", "mightyos/profiles/" + baseId + "/navstate.json"))
            verify(testFiles.exists("config", "mightyos/profiles/" + baseId + "/favorites.json"))
            win.openProfilePicker()
            chooseOption("delete")
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            compare(ProfileService.activeId, other)
            act(InputRouter.Back)
            compare(stack.profileId, other)
            wait(600)                                 // past the save debounce
            NavState.flush()
            verify(!testFiles.exists("state", "mightyos/profiles/" + baseId))
            verify(!testFiles.exists("config", "mightyos/profiles/" + baseId))
        }

        function test_backFromPickerKeepsProfile() {
            makeMain()
            verify(stack.push("games", { system: "snes" }, false))
            win.openProfilePicker()
            verify(picker.open)
            act(InputRouter.Back)
            verify(!picker.open)
            compare(stack.depth, 2)                            // untouched
        }

        function test_favoriteGameAndAppThenReorder() {
            installTestApp()
            makeMain()
            // A game: X / F on the focused tile.
            verify(stack.push("games", { system: "snes" }, false))
            act(InputRouter.Navigate, "right")
            const game = stack.current.saveState().key
            verify(game.startsWith("rom:snes:"))
            verify(act(InputRouter.Favorite))
            verify(Favorites.isFavorite(game))
            verify(toast.shown)
            const grid = findChild(stack.current, "gameGrid")
            tryVerify(() => grid.currentItem && grid.currentItem.favorite)   // star badge

            // An app, from Settings > All apps.
            verify(stack.push("apps", {}, false))
            stack.current.restoreState({ index: 0, key: appKey })
            compare(stack.current.saveState().key, appKey)
            verify(act(InputRouter.Favorite))
            compare(Favorites.ids(), [game, appKey])

            // Favorites on Home shows them in saved order.
            homeOnFavorites()
            act(InputRouter.Navigate, "down")
            const content = findChild(stack.current, "homeContent")
            compare(content.count, 2)
            compare(content.keyAt(0), game)
            compare(content.keyAt(1), appKey)

            // Reorder through the options menu; the focus follows the tile.
            act(InputRouter.Navigate, "right")
            compare(stack.current.saveState().key, appKey)
            chooseOption("moveLeft")
            compare(Favorites.ids(), [appKey, game])
            compare(content.keyAt(0), appKey)
            compare(stack.current.saveState().key, appKey)
            compare(stack.current.saveState().index, 0)
            // No "Move left" at the first place.
            verify(act(InputRouter.Options))
            verify(menu.items.findIndex(i => i.id === "moveLeft") < 0)
            verify(menu.items.findIndex(i => i.id === "moveRight") >= 0)
            act(InputRouter.Back)

            // X again removes it.
            verify(act(InputRouter.Favorite))
            compare(Favorites.ids(), [game])
            compare(content.count, 1)
        }

        function test_unavailableFavoriteCanBeRemoved() {
            installTestApp()
            verify(Favorites.add(appKey))
            verify(testFiles.remove("mightyos/apps/testapp.json"))   // "uninstalled"
            AppRegistry.rescan()
            tryVerify(() => !AppRegistry.contains(appKey), 5000)
            makeMain()
            homeOnFavorites()
            act(InputRouter.Navigate, "down")
            const row = stack.current.focusedItem
            compare(row.key, appKey)
            compare(row.available, false)                      // greyed "Unavailable"
            compare(stack.current.prompts[0].label, "Remove")
            act(InputRouter.Accept)
            verify(dialog.open)
            compare(dialog.buttons[dialog.currentIndex].id, "keep")   // safe default
            // Keep it, then the app comes back: prompts follow in place.
            act(InputRouter.Accept)                            // "Keep" (default)
            verify(!dialog.open)
            installTestApp()
            tryCompare(stack.current, "focusedAcceptLabel", "Launch", 5000)
            compare(stack.current.focusedItem.available, true)
            compare(stack.current.prompts[0].label, "Launch")
            // Uninstall again and remove it.
            verify(testFiles.remove("mightyos/apps/testapp.json"))
            AppRegistry.rescan()
            tryCompare(stack.current, "focusedAcceptLabel", "Remove", 5000)
            act(InputRouter.Accept)
            verify(dialog.open)
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            verify(!dialog.open)
            compare(Favorites.count, 0)
        }

        function test_launchFillsRecentRow() {
            installTestApp()
            makeMain()
            const before = History.launchCount(appKey)
            verify(stack.push("apps", {}, false))
            stack.current.restoreState({ index: 0, key: appKey })
            act(InputRouter.Accept)
            verify(Launcher.running)
            tryVerify(() => !Launcher.running, 10000)
            compare(History.launchCount(appKey), before + 1)
            compare(History.recent.keyAt(0), appKey)
            tryCompare(InputRouter, "muted", false, 2000)

            homeOnFavorites()
            const recent = findChild(stack.current, "homeRecent")
            verify(recent.count > 0)
            verify(recent.parent.visible)
            act(InputRouter.Navigate, "down")                  // no favorites: straight to Recent
            compare(stack.current.saveState().row, 2)
            compare(stack.current.saveState().key, appKey)
        }

        function test_createEditDeleteInPicker() {
            makeMain()
            win.openProfilePicker()
            const row = profileRow()
            compare(row.count, 2)                              // base + "Add profile"
            act(InputRouter.Navigate, "right")
            compare(row.currentEntry().kind, "add")
            act(InputRouter.Accept)
            const editor = findChild(picker, "profileEditor")
            verify(editor.open)
            const name = findChild(editor, "editorName").currentId
            for (let i = 0; i < 4; ++i)
                act(InputRouter.Accept)                        // Next, Next, Next, Next -> buttons
            compare(editor.focusRow, 4)
            act(InputRouter.Accept)                            // Create
            verify(!editor.open)
            compare(ProfileService.count, 2)
            const created = row.currentEntry()
            compare(created.title, name)                       // focus on the new profile
            compare(ProfileService.activeId, baseId)           // creating doesn't switch

            // Edit: rename + kids.
            chooseOption("edit")
            verify(editor.open)
            compare(editor.mode, "edit")
            act(InputRouter.Navigate, "right")                 // another name
            const newName = findChild(editor, "editorName").currentId
            verify(newName !== name)
            for (let i = 0; i < 3; ++i)
                act(InputRouter.Navigate, "down")              // Type row
            act(InputRouter.Navigate, "right")                 // Kids
            act(InputRouter.Navigate, "down")
            act(InputRouter.Accept)                            // Save
            const edited = ProfileService.profile(created.key)
            compare(edited.name, newName)
            compare(edited.kind, "kids")
            compare(row.currentEntry().badge, "Kids")

            // Delete asks first; Keep is the default.
            chooseOption("delete")
            verify(dialog.open)
            compare(dialog.buttons[dialog.currentIndex].id, "keep")
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            compare(ProfileService.count, 1)

            // The last profile can't be deleted.
            compare(row.currentEntry().key, baseId)
            chooseOption("delete")
            verify(!dialog.open)
            verify(toast.shown && toast.error)
            compare(ProfileService.count, 1)
            act(InputRouter.Back)
            verify(!picker.open)
        }

        function test_deletingActiveProfileSwitchesOnClose() {
            const other = ProfileService.create("Comet", "cat", "#ff6b8b")
            verify(other.length > 0)
            ProfileService.select(baseId)
            makeMain()
            act(InputRouter.Accept)                            // base
            win.openProfilePicker()
            compare(profileRow().currentEntry().key, baseId)
            chooseOption("delete")
            act(InputRouter.Navigate, "right")
            act(InputRouter.Accept)
            compare(ProfileService.count, 1)
            compare(ProfileService.activeId, other)
            act(InputRouter.Back)
            verify(!picker.open)
            compare(stack.profileId, other)
            compare(stack.depth, 1)
        }
    }
}
