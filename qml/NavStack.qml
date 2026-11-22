import QtQuick
import MightyOS.Core

// Screen stack with focus memory and action routing.
//
//  - push(name, params) creates a NavScreen from `registry[name]`; pop()/popTo()
//    go back; replace() swaps the current screen in place. Lower screens stay
//    alive (hidden), so their focus is exactly where it was. Push/pop use a
//    short slide+fade (Theme.animScreen).
//  - Only `current` is enabled: screens animating out (or covered) take no
//    mouse input, and a screen leaving the stack is disconnected before it
//    animates out, so a late click can never launch or push from it.
//  - handleAction() offers each action to the top screen first; unhandled
//    actions fall back to the defaults below.
//  - The focus path [{ screen, params, row, index, key, extra }, ...] is
//    persisted through NavState (debounced) and rebuilt by restore().
//    Stale entries are dropped: unknown screen names, screens whose params
//    no longer resolve (valid === false) and everything after them.
Item {
    id: stack

    // name -> Component whose root is a NavScreen.
    property var registry: ({})
    // Selects the navstate file. Empty until profiles exist (M2).
    property string profileId: ""
    // False while a modal or the launch overlay owns input.
    property bool focusShown: true
    // (Not `top`/`root`: Item.top is a FINAL anchor line.)

    property var _screens: []
    property bool _restoring: false
    property NavScreen _optionsScreen: null

    readonly property int depth: _screens.length
    readonly property NavScreen current: _screens.length > 0 ? _screens[_screens.length - 1] : null
    readonly property NavScreen rootScreen: _screens.length > 0 ? _screens[0] : null
    readonly property var crumbs: _screens.map(s => s.title)

    signal launchRequested(var item)
    signal toastRequested(string message, bool isError)
    signal dialogRequested(var spec)
    signal optionsRequested(string title, var items)

    // Returns the new screen, or null if `name` is unknown or the screen
    // reports itself invalid for `params`.
    function push(name: string, params: var, animated: bool): NavScreen {
        const screen = _create(name, params)
        if (!screen)
            return null
        const previous = current
        _screens = _screens.concat([screen])
        const animate = animated !== false && !_restoring
        if (previous)
            previous.transitionOut(-Theme.screenShift, false, animate)
        screen.transitionIn(previous ? Theme.screenShift : 0, animate && previous !== null)
        persist()
        return screen
    }

    // Swaps the current screen for a new one at the same depth (Back still
    // goes to the screen below). direction -1 / +1 sets the slide side.
    // The screen below gets followChild() so its focus can follow.
    function replace(name: string, params: var, direction: int): NavScreen {
        if (depth === 0)
            return push(name, params, true)
        const screen = _create(name, params)
        if (!screen)
            return null
        const leaving = current
        _screens = _screens.slice(0, -1).concat([screen])
        _retire(leaving)
        leaving.transitionOut(-direction * Theme.screenShift, true, true)
        screen.transitionIn(direction * Theme.screenShift, true)
        if (depth > 1)
            _screens[depth - 2].followChild(name, screen.params)
        persist()
        return screen
    }

    function pop(): bool {
        return popTo(depth - 1)
    }

    // Back to `targetDepth` screens (1 = root). Intermediate screens go
    // away instantly; only the current one animates out.
    function popTo(targetDepth: int): bool {
        const target = Math.max(1, targetDepth)
        if (depth <= target)
            return false
        const leaving = current
        const removed = _screens.slice(target)
        _screens = _screens.slice(0, target)
        for (const screen of removed) {
            if (screen !== leaving)
                screen.destroy()
        }
        _retire(leaving)
        leaving.transitionOut(Theme.screenShift, true, true)
        current.transitionIn(-Theme.screenShift, true)
        SoundFx.play(SoundFx.Back)
        persist()
        return true
    }

    function handleAction(action: int, arg: string): bool {
        const screen = current
        if (!screen)
            return false
        if (screen.handleAction(action, arg))
            return true

        switch (action) {
        case InputRouter.Back:
            return pop()
        case InputRouter.PrevCategory:
        case InputRouter.NextCategory:
            // LB/RB from anywhere: back to Home, then switch category there.
            if (depth > 1) {
                popTo(1)
                return rootScreen.handleAction(action, arg)
            }
            return false
        case InputRouter.Options: {
            const items = screen.optionsFor()
            if (items.length === 0)
                return false
            _optionsScreen = screen
            optionsRequested(screen.optionsTitle(), items)
            return true
        }
        case InputRouter.Favorite:
            return screen.favoriteFocused()
        }
        return false
    }

    // Called by Main when the context menu requested above is answered.
    function optionChosen(id: string) {
        const screen = _optionsScreen
        _optionsScreen = null
        if (screen && screen === current)
            screen.optionChosen(id)
    }

    function wheel(steps: int) {
        if (current)
            current.wheel(steps)
    }

    function path(): var {
        return _screens.map(s => {
            const state = s.saveState()
            return {
                screen: s.screenId, params: s.params,
                row: state.row ?? 0, index: state.index ?? 0,
                key: state.key ?? "", extra: state.extra ?? {}
            }
        })
    }

    function persist() {
        if (!_restoring)
            NavState.saveLater(path(), profileId)
    }

    // Immediate write, e.g. before launching a game that might take the
    // whole machine down.
    function persistNow() {
        NavState.saveLater(path(), profileId)
        NavState.flush()
    }

    // Drops every screen (after writing any pending state). Used before
    // restoring a different path, e.g. on a profile switch (M2).
    function reset() {
        NavState.flush()
        const old = _screens
        _screens = []
        _optionsScreen = null
        for (const screen of old) {
            _unwire(screen)
            screen.destroy()
        }
    }

    // Rebuilds the saved path for `profileId`, or starts fresh at "home".
    function restore() {
        reset()
        _restoring = true
        const saved = NavState.load(profileId)
        if (saved.length > 0 && saved[0].screen === "home") {
            for (const entry of saved) {
                const screen = push(entry.screen, entry.params, false)
                if (!screen)
                    break   // stale from here on
                screen.restoreState(entry)
            }
        }
        if (depth === 0)
            push("home", {}, false)
        _restoring = false
        persist()   // write back the cleaned-up path
    }

    function _create(name: string, params: var): NavScreen {
        const component = registry[name]
        if (!component) {
            console.warn("NavStack: unknown screen", name)
            return null
        }
        const screen = component.createObject(stack, {
            screenId: name, params: params ?? {}, visible: false, opacity: 0
        }) as NavScreen
        if (!screen) {
            console.warn("NavStack: could not create screen", name, component.errorString())
            return null
        }
        if (!screen.valid) {
            screen.destroy()
            return null
        }
        _wire(screen)
        return screen
    }

    function _wire(screen: NavScreen) {
        screen.focusShown = Qt.binding(() => stack.focusShown && stack.current === screen)
        screen.enabled = Qt.binding(() => stack.current === screen)
        const handlers = {
            navState: () => stack.persist(),
            open: (name, params) => {
                if (!stack.push(name, params, true))
                    stack.toastRequested(qsTr("That is no longer available"), true)
            },
            replace: (name, params, direction) => {
                if (!stack.replace(name, params, direction))
                    stack.toastRequested(qsTr("That is no longer available"), true)
            },
            launch: item => stack.launchRequested(item),
            toast: (message, isError) => stack.toastRequested(message, isError),
            dialog: spec => stack.dialogRequested(spec)
        }
        screen.navStateChanged.connect(handlers.navState)
        screen.openRequested.connect(handlers.open)
        screen.replaceRequested.connect(handlers.replace)
        screen.launchRequested.connect(handlers.launch)
        screen.toastRequested.connect(handlers.toast)
        screen.dialogRequested.connect(handlers.dialog)
        screen._stackConnections = handlers
    }

    function _unwire(screen: NavScreen) {
        const handlers = screen._stackConnections
        if (!handlers)
            return
        screen._stackConnections = null
        screen.navStateChanged.disconnect(handlers.navState)
        screen.openRequested.disconnect(handlers.open)
        screen.replaceRequested.disconnect(handlers.replace)
        screen.launchRequested.disconnect(handlers.launch)
        screen.toastRequested.disconnect(handlers.toast)
        screen.dialogRequested.disconnect(handlers.dialog)
    }

    // A screen leaving the stack: no input, no signals, while it animates out.
    function _retire(screen: NavScreen) {
        screen.enabled = false
        screen.focusShown = false
        _unwire(screen)
    }
}
