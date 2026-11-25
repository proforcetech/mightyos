import QtQuick
import MightyOS.Core
import MightyOS.UI

// Home > Media (M5): up to three rows, empty ones hidden and skipped.
//
//   Media
//   Kodi · Media center                                       (info)
//   Apps            [Kodi] [Jellyfin] [Moonlight] [VLC] [Get Plex HTPC]
//   Streaming       [YouTube] [Netflix] [Disney+] [Prime Video] [Twitch]
//   Game streaming  [GAMING-PC] [htpc]
//
// Up/Down moves between rows (each remembers its position), Left/Right
// within one. Accept launches an app or web app, opens a Moonlight host's
// app list, or explains how to get a missing app. Options adds "Streaming
// settings" (Settings > Media) for standard profiles.
//
// Leaving a web app or a stream with a gamepad: hold Guide (Main closes
// it through Launcher.terminate()); a keyboard closes the browser with
// Alt+F4, Moonlight with Ctrl+Alt+Shift+Q (or Start+Select+LB+RB).
//
// params: {}
NavScreen {
    id: media

    title: qsTr("Media")

    property int focusRow: 0
    // The row asked for (restored path, else the top row): rows fill in
    // asynchronously (app scan, Moonlight settings), so the focus moves
    // there once it has tiles, unless the user has moved since.
    property int _requestedRow: 0
    property bool _userMoved: false

    prompts: {
        const list = [{ action: InputRouter.Accept, label: focusedAcceptLabel },
                      { action: InputRouter.Back, label: qsTr("Back") }]
        if (focusedCanFavorite)
            list.push({ action: InputRouter.Favorite, label: qsTr("Favorite") })
        list.push({ action: InputRouter.Options, label: qsTr("Options") })
        return list
    }

    function _view(index: int): var {
        return index === 0 ? appsRow : index === 1 ? webRow : hostRow
    }

    function _usable(index: int): bool {
        return _view(index).count > 0
    }

    function _firstUsable(): int {
        for (let r = 0; r < 3; ++r) {
            if (_usable(r))
                return r
        }
        return 0
    }

    function _updateFocused() {
        const item = _usable(focusRow) ? _view(focusRow).currentEntry() : null
        if (JSON.stringify(item) !== JSON.stringify(focusedItem))
            focusedItem = item
    }

    onFocusRowChanged: {
        _updateFocused()
        navStateChanged()
    }

    function _moveVertical(direction: string): bool {
        const step = direction === "down" ? 1 : -1
        for (let r = focusRow + step; r >= 0 && r <= 2; r += step) {
            if (_usable(r)) {
                focusRow = r
                SoundFx.play(SoundFx.Move)
                return true
            }
        }
        _view(focusRow).bump(direction)
        return false
    }

    function handleAction(action: int, arg: string): bool {
        switch (action) {
        case InputRouter.Navigate:
            _userMoved = true
            if (arg === "up" || arg === "down")
                _moveVertical(arg)
            else if (_usable(focusRow))
                _view(focusRow).moveFocus(arg)
            else
                SoundFx.play(SoundFx.Bump)
            return true
        case InputRouter.Accept:
            if (_usable(focusRow))
                _view(focusRow).activateCurrent()
            else
                SoundFx.play(SoundFx.Bump)
            return true
        }
        return false
    }

    function extraOptions(): var {
        const items = [{ id: "mediaReload", label: qsTr("Look for apps and hosts again") }]
        if (!Library.restricted)
            items.push({ id: "mediaSettings", label: qsTr("Streaming settings") })
        return items
    }

    function extraOptionChosen(id: string): bool {
        if (id === "mediaReload") {
            AppRegistry.rescan()
            Media.reload()
            toastRequested(qsTr("Looking for media apps and Moonlight hosts…"), false)
            return true
        }
        if (id === "mediaSettings") {
            openRequested("mediasettings", {})
            return true
        }
        return false
    }

    function saveState(): var {
        const view = _view(focusRow)
        return { row: focusRow, index: view.savedIndex(), key: view.savedKey(),
                 extra: { rows: [
                     { index: appsRow.savedIndex(), key: appsRow.savedKey() },
                     { index: webRow.savedIndex(), key: webRow.savedKey() },
                     { index: hostRow.savedIndex(), key: hostRow.savedKey() }] } }
    }

    function restoreState(state: var) {
        const rows = state.extra && Array.isArray(state.extra.rows) ? state.extra.rows : []
        for (let r = 0; r < 3; ++r) {
            const p = rows[r]
            if (p && typeof p.index === "number" && p.index >= 0)
                _view(r).restoreFocus(Math.floor(p.index), typeof p.key === "string" ? p.key : "")
        }
        const row = typeof state.row === "number" ? Math.max(0, Math.min(2, Math.floor(state.row))) : 0
        _view(row).restoreFocus(Math.max(0, state.index ?? 0), state.key ?? "")
        _requestedRow = row
        _userMoved = false
        focusRow = _usable(row) ? row : _firstUsable()
        _updateFocused()
    }

    function wheel(steps: int) {
        _view(focusRow).wheelStep(steps)
    }

    Component.onCompleted: {
        appsRow.currentIndex = 0
        webRow.currentIndex = 0
        hostRow.currentIndex = 0
        focusRow = _firstUsable()
        _updateFocused()
    }

    // Rows fill in late and can empty out under the focus (uninstall,
    // profile switch): go to the requested row once it is usable (if the
    // user hasn't moved), and never stay on an empty row.
    function _rowCountChanged(index: int) {
        if (!_userMoved && _usable(_requestedRow) && focusRow !== _requestedRow)
            focusRow = _requestedRow
        else if (!_usable(focusRow))
            focusRow = _firstUsable()
        _updateFocused()
    }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingS

        InfoHeader {
            width: parent.width
            heading: media.focusedItem ? media.focusedItem.title : media.title
            detail: {
                if (!media.focusedItem) {
                    if (!AppRegistry.ready || !Media.ready)
                        return qsTr("Looking for media apps…")
                    return Library.restricted ? qsTr("Nothing is allowed for this profile yet. Ask a parent.")
                                              : qsTr("No media apps found. Kodi, Jellyfin, Plex and Moonlight show up here once installed.")
                }
                if (media.focusedItem.available === false)
                    return qsTr("Unavailable")
                if (media.focusedItem.kind === "webapp")
                    return media.focusedItem.subtitle + "  ·  " + qsTr("Hold the Guide button to close it")
                return media.focusedItem.subtitle
            }
        }

        MediaRowLabel { text: qsTr("Apps"); shown: appsRow.count > 0 }
        Item {
            visible: appsRow.count > 0
            width: parent.width
            height: appsRow.height + 2 * Theme.overflowFor(appsRow.tileHeight)
            TileRow {
                id: appsRow
                objectName: "mediaApps"
                model: Media.apps
                keepPendingKey: true
                sourceReady: AppRegistry.ready
                tileWidth: Theme.recentTileSize
                tileHeight: Theme.recentTileSize
                anchors.verticalCenter: parent.verticalCenter
                active: media.focusShown && media.focusRow === 0
                onCountChanged: media._rowCountChanged(0)
                onCurrentIndexChanged: { media._updateFocused(); media.navStateChanged() }
                onCurrentItemChanged: media._updateFocused()
                onCurrentDataChanged: media._updateFocused()
                onFocusRequested: {
                    media._userMoved = true
                    media.focusRow = 0
                }
                onActivated: (index) => media.activateItem(Media.apps.get(index))
            }
        }

        MediaRowLabel { text: qsTr("Streaming"); shown: webRow.count > 0 }
        Item {
            visible: webRow.count > 0
            width: parent.width
            height: webRow.height + 2 * Theme.overflowFor(webRow.tileHeight)
            TileRow {
                id: webRow
                objectName: "mediaWebApps"
                model: Media.webApps
                keepPendingKey: true
                sourceReady: true   // web-app definitions load synchronously
                tileWidth: Theme.recentTileSize
                tileHeight: Theme.recentTileSize
                anchors.verticalCenter: parent.verticalCenter
                active: media.focusShown && media.focusRow === 1
                onCountChanged: media._rowCountChanged(1)
                onCurrentIndexChanged: { media._updateFocused(); media.navStateChanged() }
                onCurrentItemChanged: media._updateFocused()
                onCurrentDataChanged: media._updateFocused()
                onFocusRequested: {
                    media._userMoved = true
                    media.focusRow = 1
                }
                onActivated: (index) => media.activateItem(Media.webApps.get(index))
            }
        }

        MediaRowLabel { text: qsTr("Game streaming"); shown: hostRow.count > 0 }
        Item {
            visible: hostRow.count > 0
            width: parent.width
            height: hostRow.height + 2 * Theme.overflowFor(hostRow.tileHeight)
            TileRow {
                id: hostRow
                objectName: "mediaHosts"
                model: Media.hosts
                keepPendingKey: true
                sourceReady: Media.ready
                tileWidth: Theme.recentTileSize
                tileHeight: Theme.recentTileSize
                anchors.verticalCenter: parent.verticalCenter
                active: media.focusShown && media.focusRow === 2
                onCountChanged: media._rowCountChanged(2)
                onCurrentIndexChanged: { media._updateFocused(); media.navStateChanged() }
                onCurrentItemChanged: media._updateFocused()
                onCurrentDataChanged: media._updateFocused()
                onFocusRequested: {
                    media._userMoved = true
                    media.focusRow = 2
                }
                onActivated: (index) => media.activateItem(Media.hosts.get(index))
            }
        }
    }

    component MediaRowLabel: Text {
        property bool shown: true
        visible: shown
        textFormat: Text.PlainText
        color: Theme.textDim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSmall
        font.weight: Font.DemiBold
    }
}
