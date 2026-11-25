import QtQuick
import QtTest
import MightyOS.Core
import MightyOS.UI

// M4 UI on the mock library: A-Z letter jumps in the game grid, kids
// profiles seeing only allowed systems, deleting a profile that has saves
// (keep is the default), and Settings > Artwork (API key entry, masked).
Item {
    id: root
    width: 100
    height: 100

    Component { id: mainComponent; Main {} }

    TestCase {
        name: "Library"
        when: windowShown

        property var win: null
        property NavStack stack: null
        property Dialog dialog: null
        property Toast toast: null
        property string baseId

        function act(action, arg) {
            return InputRouter.dispatch(action, arg ?? "", InputRouter.Keyboard)
        }

        function init() {
            InputRouter.muteFor(0)
            baseId = ProfileService.activeId
            for (const p of ProfileService.profiles) {
                if (p.id !== baseId)
                    ProfileService.remove(p.id, false)
            }
            ProfileService.select(baseId)
            verify(ProfileService.setKind(baseId, "standard"))   // other test files may have changed it
            NavState.save([], baseId)
            win = createTemporaryObject(mainComponent, null, {
                launchCommand: [mightyosTestApp, "-platform", "offscreen", "--quit-after", "300"]
            })
            verify(win)
            stack = findChild(win, "navStack")
            dialog = findChild(win, "dialog")
            toast = findChild(win, "toast")
            tryVerify(() => stack.width > 0 && stack.height > 0)
        }

        function cleanup() {
            tryVerify(() => !Launcher.running, 10000)
            if (Scraper.hasKey)
                Scraper.clearApiKey()
            InputRouter.muteFor(0)
        }

        function focusedTitle() {
            return stack.current.focusedItem ? stack.current.focusedItem.title : ""
        }

        function test_letterJumps() {
            verify(stack.push("games", { system: "snes" }, false))
            const grid = findChild(stack.current, "gameGrid")
            const model = Library.games("snes")
            verify(model.count > 10)
            // Keyboard letter: first title at or after "M".
            verify(act(InputRouter.Letter, "M"))
            const m = focusedTitle()
            verify(m.length > 0)
            compare(grid.currentIndex, model.firstIndexForLetter("M"))
            compare(findChild(stack.current, "letterBadge").text, model.letterAt(grid.currentIndex))
            // RT: the next letter group; LT: back to its start, then the previous group.
            const before = model.letterAt(grid.currentIndex)
            act(InputRouter.PageDown)
            verify(model.letterAt(grid.currentIndex) !== before)
            compare(grid.currentIndex, model.letterStep(model.firstIndexForLetter("M"), 1))
            act(InputRouter.PageUp)
            compare(model.letterAt(grid.currentIndex), before)
            // At the first title LT bumps (stays).
            grid.currentIndex = 0
            act(InputRouter.PageUp)
            compare(grid.currentIndex, 0)
            // Letters are not actions: "F" doesn't toggle a favorite.
            const favorites = Favorites.count
            act(InputRouter.Letter, "F")
            compare(Favorites.count, favorites)
            // The prompt says what LT/RT do here.
            verify(stack.current.prompts.some(p => p.label === "A–Z"))
        }

        function test_kidsProfileSeesOnlyAllowed() {
            const kid = ProfileService.create("Kiddo", "cat", "#ff6b8b", "kids")
            verify(kid.length > 0)
            verify(Library.systems("emulators").count > 0)
            ProfileService.select(kid)
            verify(Library.restricted)
            compare(Library.systems("emulators").count, 0)   // nothing allowed yet
            verify(Library.games("snes") === null)
            verify(!stack.push("games", { system: "snes" }, false))   // stale paths are refused too
            // S2: no settings, store, app list, artwork setup or profile switching.
            verify(!stack.push("artwork", {}, false))
            verify(!stack.push("apps", {}, false))
            verify(!stack.push("category", { category: "settings" }, false))
            verify(!stack.push("category", { category: "store" }, false))
            verify(!stack.push("mediasettings", {}, false))
            // M5: Media follows the allow-list too (default deny), no "Get it" tiles.
            verify(stack.push("media", {}, false))
            compare(Media.home.count, 0)
            compare(Media.apps.count, 0)
            compare(Media.webApps.count, 0)
            verify(!Media.addWebApp("X", "https://x.example/").ok)
            stack.popTo(1)
            win.openProfilePicker()
            verify(!findChild(win, "profilePicker").open)
            verify(toast.message.indexOf("parent") >= 0)
            stack.current.activateItem({ key: "settings", kind: "category", title: "Settings" })
            compare(stack.depth, 1)
            stack.current.activateItem({ key: "settings:apps", kind: "allapps", title: "All apps" })
            compare(stack.depth, 1)
            const bar = findChild(stack.current, "categoryBar")
            verify(bar.model.indexOfKey("settings") < 0)
            verify(bar.model.indexOfKey("store") < 0)
            verify(bar.model.indexOfKey("emulators") >= 0)
            ProfileService.select(baseId)
            const first = Library.games("snes").keyAt(0)
            verify(testFiles.writeConfig("mightyos/profiles/" + kid + "/allowed.json",
                                         JSON.stringify({ version: 1, systems: ["n64"], games: [first] })))
            ProfileService.select(kid)
            compare(Library.systems("emulators").count, 2)   // n64 + snes (one game)
            compare(Library.games("snes").count, 1)
            compare(Library.games("snes").keyAt(0), first)
            compare(Media.webApps.count, 0)
            ProfileService.select(baseId)
            verify(testFiles.writeConfig("mightyos/profiles/" + kid + "/allowed.json",
                                         JSON.stringify({ version: 1, apps: ["webapp:youtube"] })))
            ProfileService.select(kid)
            compare(Media.webApps.count, 1)
            compare(Media.webApps.keyAt(0), "webapp:youtube")
            compare(Media.home.indexOfKey("webapp:netflix"), -1)
            ProfileService.select(baseId)
            verify(!Library.restricted)
            verify(findChild(stack.current, "categoryBar").model.indexOfKey("settings") >= 0)
        }

        function test_deleteProfileKeepsSavesByDefault() {
            const other = ProfileService.create("Saver", "cat", "#ff6b8b")
            verify(testFiles.write("mightyos/profiles/" + other + "/saves/snes/Game.srm", "save"))
            verify(ProfileService.hasSaves(other))
            win.openProfilePicker()
            const picker = findChild(win, "profilePicker")
            verify(picker.open)
            const row = findChild(picker, "profileRow")
            row.restoreFocus(0, other)
            compare(row.currentEntry().key, other)
            act(InputRouter.Options)
            const menu = findChild(win, "contextMenu")
            menu.currentIndex = menu.items.findIndex(i => i.id === "delete")
            act(InputRouter.Accept)
            verify(dialog.open)
            compare(dialog.buttons.length, 3)
            compare(dialog.buttons[dialog.currentIndex].id, "cancel")   // safe default
            act(InputRouter.Navigate, "right")
            compare(dialog.buttons[dialog.currentIndex].id, "delete")   // "Delete, keep saves"
            act(InputRouter.Accept)
            verify(!ProfileService.contains(other))
            verify(testFiles.exists("data", "mightyos/profiles/" + other + "/saves/snes/Game.srm"))
            verify(!testFiles.exists("config", "mightyos/profiles/" + other))
            act(InputRouter.Back)
        }

        function test_artworkKeyEntry() {
            verify(stack.push("artwork", {}, false))
            const screen = stack.current
            verify(!screen.acceptsRemote)
            verify(!Scraper.hasKey)
            act(InputRouter.Accept)                       // "Enter key"
            verify(screen.editing)
            verify(InputRouter.secureEntry)               // no action logging while typing
            const field = findChild(screen, "apiKeyField")
            verify(field.activeFocus)
            field.text = "not a key"
            act(InputRouter.Accept)
            verify(screen.editing)                        // rejected, still open
            verify(toast.error)
            field.text = "0123456789abcdef0123456789abcdef"
            act(InputRouter.Accept)
            verify(!screen.editing)
            compare(field.text, "")                       // not kept around
            tryVerify(() => Scraper.hasKey, 3000)
            verify(Scraper.maskedKey.endsWith("cdef"))
            verify(!Scraper.maskedKey.includes("0123"))
            verify(testFiles.exists("config", "mightyos/secrets/steamgriddb.key"))
            // Esc / B cancels an edit without touching the key.
            act(InputRouter.Accept)
            verify(screen.editing)
            act(InputRouter.Back)
            verify(!screen.editing)
            verify(Scraper.hasKey)
            compare(stack.depth, 2)
        }
    }
}
