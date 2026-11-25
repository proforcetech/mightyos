import QtQuick
import MightyOS.Core
import "Catalog.js" as Catalog

// Base type for every NavStack screen.
//
// Routing contract: NavStack hands each action to the top screen's
// handleAction(action, arg). Returning false lets it bubble to NavStack's
// defaults (Back pops, LB/RB jumps categories on Home, Options opens the
// context menu from optionsFor(), Favorite calls favoriteFocused()).
//
// Focus memory: saveState() returns { row, index, key, extra } where `row`
// is the focusable row/component inside the screen (0 for single-row
// screens), `index` the focused item, `key` its stable model key and `extra`
// optional screen-specific data. restoreState() gets the same object back,
// possibly stale: prefer `key`, clamp `index`, never assume it still fits.
// Emit navStateChanged() whenever the focus moves so the path is persisted.
//
// Focused item: screens assign `focusedItem` (the model row under the
// ring, from TileListModel.get()) whenever their focus moves, instead of
// bindings calling get() on every evaluation. Prompts that depend on it should bind to
// `focusedAcceptLabel` (a string, so it only notifies when the text changes
// and the PromptBar is not rebuilt on every focus move).
//
// Screens never call NavStack directly; they emit the *Requested signals.
Item {
    id: screen

    // NavStack registry name and construction parameters. Both end up in
    // the persisted focus path, so keep params small and JSON-friendly.
    property string screenId
    property var params: ({})
    property string title
    // PromptBar entries for the current focus: [{ action, label }].
    property var prompts: []
    // False if `params` refer to something that no longer exists (stale
    // saved path, uninstalled system). NavStack then refuses the screen.
    property bool valid: true
    // Bound by NavStack: only the top screen, with no modal open, shows
    // its focus ring ("exactly one focused element").
    property bool focusShown: true
    // The model row under the focus ring, or null. Set by the screen.
    property var focusedItem: null
    readonly property string focusedAcceptLabel: acceptLabel(focusedItem)
    // Whether the focused item can be a favorite (X / Insert), for prompts.
    readonly property bool focusedCanFavorite: canFavorite(focusedItem)
    // PLAN D18: false on screens the TV remote must not operate (secure
    // screens in M8); while such a screen is on top the remote can only go
    // Back / Home. secureEntry additionally silences action logging (PIN
    // pad, admin keyboard). Main binds InputRouter to both.
    property bool acceptsRemote: true
    property bool secureEntry: false
    // NavStack's connections to this screen (so they can be cut when the
    // screen leaves the stack). Not for screens to touch.
    property var _stackConnections: null

    signal navStateChanged()
    signal openRequested(string screen, var params)
    // Swap this screen for another in place (no push): direction -1 / +1
    // picks the slide direction.
    signal replaceRequested(string screen, var params, int direction)
    signal launchRequested(var item)
    signal toastRequested(string message, bool isError)
    signal dialogRequested(var spec)
    // A second-level menu (e.g. "Emulator…"): NavStack shows it like the
    // options menu and hands the answer to optionChosen().
    signal menuRequested(string title, var items)
    // Open the profile picker (Home options, Settings > Profiles).
    signal profilePickerRequested()

    // --- Overridable --------------------------------------------------------

    function handleAction(action: int, arg: string): bool {
        return false
    }
    function saveState(): var {
        return { row: 0, index: 0, key: "", extra: {} }
    }
    function restoreState(state: var) {
    }
    function focusedRow(): var {
        return focusedItem
    }
    // Called on the screen below when the screen above was replaced in
    // place (e.g. GameGrid stepping SNES -> N64), so its focus can follow.
    function followChild(screenName: string, childParams: var) {
    }
    // Mouse wheel over empty screen space.
    function wheel(steps: int) {
    }
    // Guide on the root screen (Home overrides it).
    function goHome() {
    }
    // True if the focused tile is shown as part of the favorites list
    // (Home > Favorites row, the Favorites grid): enables reordering.
    function focusedInFavoritesList(): bool {
        return false
    }
    // Screen-specific entries appended to the options menu, and their
    // handler (return true if `id` was one of them).
    function extraOptions(): var {
        return []
    }
    function extraOptionChosen(id: string): bool {
        return false
    }

    // --- Shared behaviour (driven by the focused row's `kind`) -------------

    // PLAN D13: a kids profile has no store, settings, app list, profile
    // management or artwork setup. A UI restriction, not a security boundary.
    function isRestrictedEntry(row: var): bool {
        if (!row)
            return false
        if (row.kind === "category")
            return row.key === "store" || row.key === "settings"
        return ["profiles", "allapps", "controllers", "power", "artwork", "section", "mediasettings",
                "getapp", "addwebapp", "managewebapp"].indexOf(row.kind) >= 0
    }

    function canFavorite(row: var): bool {
        return !!row && ["game", "app", "system", "webapp", "stream", "streamhost"].indexOf(row.kind) >= 0
    }

    function acceptLabel(row: var): string {
        if (!row)
            return qsTr("Select")
        if (row.available === false)
            return qsTr("Remove")
        return row.kind === "game" || row.kind === "stream" ? qsTr("Play")
             : row.kind === "app" || row.kind === "webapp" ? qsTr("Launch")
             : row.kind === "getapp" ? qsTr("Get")
             : row.kind === "addwebapp" ? qsTr("Add")
             : row.kind === "showall" ? qsTr("Show")
             : qsTr("Open")
    }

    function activateItem(row: var) {
        if (!row)
            return
        if (row.available === false) {
            _confirmRemoveUnavailable(row)
            return
        }
        if (Library.restricted && isRestrictedEntry(row)) {
            toastRequested(qsTr("Ask a parent: this isn't available in a kids profile"), false)
            return
        }
        switch (row.kind) {
        case "category": {
            const category = Catalog.category(row.key)
            if (category)
                openRequested(category.open.screen, category.open.params)
            break
        }
        case "system":
            openRequested("games", { system: row.key })
            break
        case "showall":
            // "+N more" at the end of a system row: list empty systems too.
            Settings.showAllSystems = true
            if (!Settings.showAllSystems)
                toastRequested(qsTr("Could not save the setting"), true)
            break
        case "artwork":
            openRequested("artwork", {})
            break
        case "game":
        case "app":
        case "webapp":
        case "stream":
            launchRequested({ key: row.key, title: row.title, icon: row.icon, kind: row.kind })
            break
        case "streamhost":
            openRequested("moonlight", { host: row.key })
            break
        case "mediasettings":
            openRequested("mediasettings", {})
            break
        case "getapp":
            _showGetApp(row)
            break
        case "profiles":
            profilePickerRequested()
            break
        case "allapps":
            openRequested("apps", {})
            break
        case "controllers":
            _showControllers()
            break
        case "power":
            dialogRequested({
                title: qsTr("Power off?"),
                message: qsTr("Power controls arrive in M8. This dialog shows the safe default: "
                              + "focus starts on Cancel, and Back also cancels."),
                acceptsRemote: false,
                buttons: [
                    { id: "cancel", label: qsTr("Cancel") },
                    { id: "poweroff", label: qsTr("Power off"), destructive: true }
                ],
                onFinished: (id) => {
                    if (id === "poweroff")
                        screen.toastRequested(qsTr("Power off is not wired up yet (M8)"), false)
                }
            })
            break
        case "section": {
            const prefix = String(row.key).split(":")[0]
            toastRequested(qsTr("%1 arrives in %2").arg(row.title)
                           .arg(Catalog.sectionMilestone[prefix] ?? qsTr("a later milestone")), false)
            break
        }
        }
    }

    // Settings > Controllers (until the full screen in M8): what is
    // connected, the vibration toggle (Settings.rumble) and the TV remote
    // trust level (Settings.tvRemote). The remote can't answer this dialog
    // (it must not raise its own trust level).
    function _remoteModeText(mode: string): string {
        return mode === "off" ? qsTr("off")
             : mode === "full" ? qsTr("full control")
             : qsTr("navigation only (move, OK, Back, Home)")
    }

    function _showControllers() {
        const names = InputRouter.controllers
        const connected = names.length > 0
            ? qsTr("Connected: %1.").arg(names.join(", "))
            : qsTr("No controller or TV remote connected.")
        const rumble = Settings.rumble
        const remote = Settings.tvRemote
        const nextRemote = remote === "navigation" ? "full" : remote === "full" ? "off" : "navigation"
        dialogRequested({
            title: qsTr("Controllers"),
            message: connected + "\n"
                + (rumble ? qsTr("Vibration is on: a short pulse at the end of a row and on errors.")
                          : qsTr("Vibration is off."))
                + "\n" + qsTr("TV remote: %1.").arg(_remoteModeText(remote)),
            acceptsRemote: false,
            buttons: [
                { id: "close", label: qsTr("Close") },
                { id: "rumble", label: rumble ? qsTr("Vibration off") : qsTr("Vibration on") },
                { id: "remote", label: qsTr("TV remote: %1").arg(
                      nextRemote === "off" ? qsTr("off") : nextRemote === "full" ? qsTr("full") : qsTr("navigation")) }
            ],
            onFinished: (id) => {
                if (id === "rumble") {
                    Settings.rumble = !rumble
                    if (Settings.rumble === rumble) {
                        screen.toastRequested(qsTr("Could not save the setting"), true)
                        return
                    }
                    screen.toastRequested(Settings.rumble ? qsTr("Vibration on") : qsTr("Vibration off"), false)
                    if (Settings.rumble)
                        InputRouter.rumble(InputRouter.Bump)   // feel it
                } else if (id === "remote") {
                    Settings.tvRemote = nextRemote
                    if (Settings.tvRemote !== nextRemote) {
                        screen.toastRequested(qsTr("Could not save the setting"), true)
                        return
                    }
                    screen.toastRequested(qsTr("TV remote: %1").arg(_remoteModeText(nextRemote)), false)
                }
            }
        })
    }

    // A favorite whose target is gone: offer to remove it (Keep is the
    // default and the Back answer).
    function _confirmRemoveUnavailable(row: var) {
        const key = row.key
        const title = row.title
        dialogRequested({
            title: qsTr("%1 is unavailable").arg(title),
            message: qsTr("It was uninstalled or its files are missing. Remove it from your favorites?"),
            buttons: [
                { id: "keep", label: qsTr("Keep") },
                { id: "remove", label: qsTr("Remove"), destructive: true }
            ],
            onFinished: (id) => {
                if (id === "remove" && Favorites.remove(key))
                    screen.toastRequested(qsTr("Removed %1 from Favorites").arg(title), false)
            }
        })
    }

    function optionsTitle(): string {
        const row = focusedRow()
        return row ? row.title : ""
    }

    function optionsFor(): var {
        const row = focusedRow()
        if (!row)
            return extraOptions()   // e.g. "Look for apps again" on an empty screen
        const items = []
        if (row.available !== false)
            items.push({ id: "activate", label: acceptLabel(row) })
        if (canFavorite(row)) {
            const isFavorite = Favorites.isFavorite(row.key)
            if (isFavorite && focusedInFavoritesList()) {
                const index = Favorites.indexOf(row.key)
                if (index > 0)
                    items.push({ id: "moveLeft", label: qsTr("Move left") })
                if (index >= 0 && index < Favorites.count - 1)
                    items.push({ id: "moveRight", label: qsTr("Move right") })
            }
            items.push(isFavorite ? { id: "favorite", label: qsTr("Remove from Favorites"),
                                      destructive: row.available === false }
                                  : { id: "favorite", label: qsTr("Add to Favorites") })
        }
        if (row.kind === "game" && row.available !== false) {
            if (Library.emulatorChoices(row.key).length > 1)
                items.push({ id: "emulator", label: qsTr("Emulator…") })
            items.push({ id: "info", label: qsTr("Details") })
        }
        if (row.kind === "system" || row.kind === "showall") {
            if (row.kind === "system" && Library.emulatorChoices(row.key).length > 1)
                items.push({ id: "emulator", label: qsTr("Default emulator…") })
            if (!Library.mock)
                items.push({ id: "scan", label: qsTr("Scan for games") })
            if (!Library.mock && !Library.restricted && Library.missingCount > 0)
                items.push({ id: "forgetMissing", label: qsTr("Forget missing games (%1)").arg(Library.missingCount),
                             destructive: true })
            if (!Library.mock && !Library.restricted)
                items.push({ id: "showAll", label: Settings.showAllSystems ? qsTr("Hide systems without games")
                                                                           : qsTr("Show all systems") })
        }
        return items.concat(extraOptions())
    }

    function optionChosen(id: string) {
        if (extraOptionChosen(id))
            return
        const row = focusedRow()
        if (!row)
            return
        switch (id) {
        case "activate":  activateItem(row); break
        case "favorite":  favoriteFocused(); break
        case "moveLeft":  moveFavorite(row.key, -1); break
        case "moveRight": moveFavorite(row.key, 1); break
        case "info":      _showDetails(row); break
        case "emulator":  _chooseEmulator(row); break
        case "scan":
            Library.rescan()
            toastRequested(qsTr("Scanning for games…"), false)
            break
        case "forgetMissing":
            _confirmForgetMissing()
            break
        case "showAll":
            Settings.showAllSystems = !Settings.showAllSystems
            break
        default:
            if (id.startsWith("emu:"))
                _setEmulator(row, id.slice(4))
            break
        }
    }

    // Games whose ROM folder is gone ("Not connected"): drop them from the
    // grids and the cache after a confirmation (Keep is the default). An
    // unplugged drive's games come back with the next scan once it's back.
    function _confirmForgetMissing() {
        const count = Library.missingCount
        dialogRequested({
            title: qsTr("Forget %n missing games?", "", count),
            message: qsTr("Their folders or drive can't be found. Forget them only if you deleted them on purpose: "
                          + "games on a drive that is just unplugged come back when it is connected and scanned again."),
            acceptsRemote: false,
            buttons: [
                { id: "keep", label: qsTr("Keep") },
                { id: "forget", label: qsTr("Forget"), destructive: true }
            ],
            onFinished: (id) => {
                if (id === "forget" && Library.forgetMissing() > 0)
                    screen.toastRequested(qsTr("Forgetting %n missing games…", "", count), false)
            }
        })
    }

    // A well-known media app that isn't installed. No auto-install: the
    // App Store arrives in M7; until then the Flatpak command is shown.
    function _showGetApp(row: var) {
        const appId = String(row.key).slice("getapp:".length)
        dialogRequested({
            title: qsTr("Get %1").arg(row.title),
            message: qsTr("%1 isn't installed. The App Store arrives in M7; until then install it on this PC with:")
                         .arg(row.title) + "\nflatpak install --user flathub " + appId,
            buttons: [{ id: "close", label: qsTr("Close") }]
        })
    }

    // Per-profile emulator for a game or (on a system tile) its default.
    function _chooseEmulator(row: var) {
        const choices = Library.emulatorChoices(row.key)
        const items = choices.map(c => ({
            id: "emu:" + c.id,
            label: c.label + (c.selected ? "  ·  " + qsTr("current") : "")
                   + (c.available ? "" : "  ·  " + qsTr("not installed"))
                   + (c.sandbox === "app" ? "  ·  " + qsTr("full app access") : "")
        }))
        if (row.kind === "game")
            items.push({ id: "emu:", label: qsTr("Use the system default") })
        menuRequested(row.kind === "game" ? qsTr("Play %1 with").arg(row.title)
                                          : qsTr("%1 games play with").arg(row.title), items)
    }

    function _setEmulator(row: var, emulatorId: string) {
        if (!Library.setEmulator(row.key, emulatorId)) {
            toastRequested(qsTr("Could not save the emulator choice"), true)
            return
        }
        const chosen = Library.emulatorChoices(row.key).find(c => c.selected)
        if (chosen && !chosen.available)
            toastRequested(qsTr("%1 is not installed yet. Install it from the App Store.").arg(chosen.label), false)
        else if (chosen)
            toastRequested(qsTr("%1 plays with %2").arg(row.title).arg(chosen.label), false)
    }

    function _showDetails(row: var) {
        if (row.kind !== "game") {
            toastRequested(qsTr("%1").arg(row.subtitle || row.title), false)
            return
        }
        const d = Library.details(row.key)
        const lines = []
        if (d.system) lines.push(d.system)
        if (d.region) lines.push(qsTr("Region: %1").arg(d.region))
        if (d.tags) lines.push(qsTr("Tags: %1").arg(d.tags))
        if (d.discs > 1) lines.push(qsTr("Discs: %1").arg(d.discs))
        if (d.emulator) lines.push(qsTr("Plays with: %1").arg(d.emulator))
        if (d.file) lines.push(qsTr("File: %1").arg(d.file))
        if (d.folder) lines.push(qsTr("Folder: %1").arg(d.folder))
        dialogRequested({ title: row.title, message: lines.join("\n"),
                          buttons: [{ id: "close", label: qsTr("Close") }] })
    }

    // Reorders a favorite; the moved tile keeps the focus (the model moves
    // the row, it doesn't reset). Screens persist the new position.
    function moveFavorite(key: string, delta: int) {
        if (Favorites.move(key, delta)) {
            SoundFx.play(SoundFx.Move)
            navStateChanged()
        } else {
            SoundFx.play(SoundFx.Bump)
        }
    }

    // X / Insert. Returns true if the focused item can be a favorite (the action
    // is then consumed, whatever the outcome).
    function favoriteFocused(): bool {
        const row = focusedRow()
        if (!canFavorite(row))
            return false
        const wasFavorite = Favorites.isFavorite(row.key)
        const nowFavorite = Favorites.toggle(row.key)
        if (nowFavorite === wasFavorite) {
            toastRequested(qsTr("Could not update Favorites"), true)
        } else {
            SoundFx.play(SoundFx.Accept)
            toastRequested(nowFavorite ? qsTr("Added %1 to Favorites").arg(row.title)
                                       : qsTr("Removed %1 from Favorites").arg(row.title), false)
        }
        return true
    }

    // --- Transitions (driven by NavStack) -----------------------------------

    property real _shift: 0
    property string _afterTransition: ""   // "", "hide" or "destroy"

    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    transform: Translate { x: screen._shift }

    // fromX: start offset (+ = from the right for push, - for a reveal).
    function transitionIn(fromX: real, animated: bool) {
        _transition.stop()
        _afterTransition = ""
        visible = true
        if (!animated) {
            opacity = 1
            _shift = 0
            return
        }
        _fade.from = 0; _fade.to = 1
        _slide.from = fromX; _slide.to = 0
        _transition.start()
    }

    function transitionOut(toX: real, destroyAfter: bool, animated: bool) {
        _transition.stop()
        _afterTransition = destroyAfter ? "destroy" : "hide"
        if (!animated) {
            _finishTransition()
            return
        }
        _fade.from = opacity; _fade.to = 0
        _slide.from = _shift; _slide.to = toX
        _transition.start()
    }

    function _finishTransition() {
        const after = _afterTransition
        _afterTransition = ""
        if (after === "hide") {
            visible = false
            opacity = 0
            _shift = 0
        } else if (after === "destroy") {
            screen.destroy()
        }
    }

    ParallelAnimation {
        id: _transition
        NumberAnimation {
            id: _fade
            target: screen; property: "opacity"
            duration: Theme.animScreen; easing.type: Easing.OutCubic
        }
        NumberAnimation {
            id: _slide
            target: screen; property: "_shift"
            duration: Theme.animScreen; easing.type: Easing.OutCubic
        }
        onFinished: screen._finishTransition()
    }
}
