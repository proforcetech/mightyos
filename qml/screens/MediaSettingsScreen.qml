import QtQuick
import MightyOS.Core
import MightyOS.UI

// Settings > Media (M5): the streaming / web-app tiles.
//
//   Media
//   Streaming tiles open in Google Chrome (Flatpak)
//   [+ Add a web app] [YouTube] [Netflix] [Disney+] ... [My Jellyfin]
//   Name     [ Jellyfin              ]    <- only while adding
//   Address  [ https://jelly.home.example/web ]
//
// Accept on a tile: a dialog to hide a built-in (or show it again) or
// remove one you added. Adding: Accept on "Add a web app" focuses the Name
// field; Enter / A moves to Address, Enter / A again saves, Esc / B cancels
// (Tab / Shift+Tab switch fields). Only https:// addresses with a plain
// host name are accepted (WebAppRegistry::isValidUrl); the tile is saved to
// ~/.config/mightyos/webapps.d/user-<name>.json (0600). No on-screen
// keyboard before M7: a gamepad alone can't type yet. Kids profiles can't
// open this screen; the TV remote can only leave it.
NavScreen {
    id: settings

    valid: !Library.restricted
    title: qsTr("Media")
    acceptsRemote: false

    property bool editing: false

    prompts: editing
        ? [{ action: InputRouter.Accept, label: addressField.activeFocus ? qsTr("Save") : qsTr("Next") },
           { action: InputRouter.Back, label: qsTr("Cancel") }]
        : [{ action: InputRouter.Accept, label: focusedAcceptLabel },
           { action: InputRouter.Back, label: qsTr("Back") }]

    function acceptLabel(row: var): string {
        if (!row)
            return qsTr("Select")
        return row.kind === "addwebapp" ? qsTr("Add") : qsTr("Manage")
    }

    function _updateFocused() {
        focusedItem = row.currentEntry()
    }

    function _startEditing() {
        nameField.text = ""
        addressField.text = "https://"
        editing = true
        nameField.forceActiveFocus()
    }

    function _stopEditing() {
        nameField.text = ""
        addressField.text = ""
        editing = false
        focusSink.forceActiveFocus()   // keys go back to InputRouter
    }

    function _next() {
        if (nameField.activeFocus) {
            addressField.forceActiveFocus()
            addressField.cursorPosition = addressField.text.length
            return
        }
        const result = Media.addWebApp(nameField.text, addressField.text)
        if (!result.ok) {
            SoundFx.play(SoundFx.Bump)
            toastRequested(result.message, true)
            return
        }
        _stopEditing()
        row.restoreFocus(Media.manage.indexOfKey(result.key), result.key)
        toastRequested(result.message, false)
    }

    function _manage(item: var) {
        const info = Media.webAppInfo(item.key)
        if (!info.key)
            return
        const buttons = [{ id: "close", label: qsTr("Close") }]
        if (info.hidden)
            buttons.push({ id: "show", label: qsTr("Show it again") })
        else if (info.user)
            buttons.push({ id: "remove", label: qsTr("Remove"), destructive: true })
        else
            buttons.push({ id: "hide", label: qsTr("Hide"), destructive: true })
        const browser = info.browser.length > 0 ? qsTr("Opens in %1.").arg(info.browser)
                      : info.drm ? qsTr("Needs Google Chrome (this service uses DRM).")
                                 : qsTr("Needs a web browser (Google Chrome or Chromium).")
        dialogRequested({
            title: info.title,
            message: qsTr("Address: %1").arg(info.host) + "\n" + browser
                     + (info.hidden ? "\n" + qsTr("Hidden from Home and Media.") : ""),
            acceptsRemote: false,
            buttons: buttons,
            onFinished: (id) => {
                const key = info.key
                if (id === "hide" || id === "remove") {
                    if (Media.removeWebApp(key))
                        settings.toastRequested(id === "hide" ? qsTr("%1 is hidden").arg(info.title)
                                                              : qsTr("Removed %1").arg(info.title), false)
                    else
                        settings.toastRequested(qsTr("Could not change %1").arg(info.title), true)
                } else if (id === "show") {
                    if (Media.showWebApp(key))
                        settings.toastRequested(qsTr("%1 is back").arg(info.title), false)
                    else
                        settings.toastRequested(qsTr("Could not change %1").arg(info.title), true)
                }
            }
        })
    }

    function _activate(index: int) {
        const item = Media.manage.get(index)
        if (item.kind === "addwebapp")
            _startEditing()
        else if (item.kind === "managewebapp")
            _manage(item)
    }

    function handleAction(action: int, arg: string): bool {
        if (editing) {
            if (action === InputRouter.Accept)
                _next()
            else if (action === InputRouter.Back)
                _stopEditing()
            return true   // nothing else while a field is open
        }
        switch (action) {
        case InputRouter.Navigate:
            row.moveFocus(arg)
            return true
        case InputRouter.Accept:
            row.activateCurrent()
            return true
        }
        return false
    }

    function optionsFor(): var {
        return []
    }

    function saveState(): var {
        return { row: 0, index: row.savedIndex(), key: row.savedKey(), extra: {} }
    }

    function restoreState(state: var) {
        row.restoreFocus(state.index, state.key)
    }

    function wheel(steps: int) {
        row.wheelStep(steps)
    }

    Component.onCompleted: {
        if (!valid)
            return
        row.currentIndex = 0
        _updateFocused()
    }

    Item { id: focusSink }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        InfoHeader {
            width: parent.width
            heading: settings.focusedItem && !settings.editing ? settings.focusedItem.title : qsTr("Streaming tiles")
            detail: Media.browserLabel.length > 0
                ? qsTr("Streaming tiles open full screen in %1, with separate logins for each profile.").arg(Media.browserLabel)
                : qsTr("No supported browser is installed. Netflix, Disney+, Prime Video and Spotify need Google Chrome: "
                       + "flatpak install --user flathub com.google.Chrome")
        }

        Item {
            width: parent.width
            height: row.height + 2 * Theme.overflowFor(row.tileHeight)

            TileRow {
                id: row
                objectName: "manageRow"
                anchors.verticalCenter: parent.verticalCenter
                model: Media.manage
                showFavorites: false
                tileWidth: Theme.recentTileSize
                tileHeight: Theme.recentTileSize
                active: settings.focusShown && !settings.editing
                onCurrentIndexChanged: {
                    settings._updateFocused()
                    settings.navStateChanged()
                }
                onCurrentItemChanged: settings._updateFocused()
                onCurrentDataChanged: settings._updateFocused()
                onActivated: (index) => settings._activate(index)
            }
        }

        Grid {
            visible: settings.editing
            columns: 2
            columnSpacing: Theme.spacingM
            rowSpacing: Theme.spacingS
            verticalItemAlignment: Grid.AlignVCenter

            FieldLabel { text: qsTr("Name") }
            FieldBox {
                field: nameField
                TextInput {
                    id: nameField
                    objectName: "webAppName"
                    anchors.fill: parent
                    anchors.margins: Theme.spacingS
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    maximumLength: 32
                    clip: true
                    KeyNavigation.tab: addressField
                    onAccepted: settings._next()
                    Keys.onEscapePressed: settings._stopEditing()
                }
            }
            FieldLabel { text: qsTr("Address") }
            FieldBox {
                field: addressField
                TextInput {
                    id: addressField
                    objectName: "webAppAddress"
                    anchors.fill: parent
                    anchors.margins: Theme.spacingS
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    maximumLength: 2048
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    clip: true
                    KeyNavigation.backtab: nameField
                    onAccepted: settings._next()
                    Keys.onEscapePressed: settings._stopEditing()
                }
            }
        }

        Text {
            visible: settings.editing
            textFormat: Text.PlainText
            width: parent.width
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSmall
            text: qsTr("Only secure https:// addresses. The tile opens that page full screen; hold the Guide button to close it.")
        }
    }

    component FieldLabel: Text {
        textFormat: Text.PlainText
        color: Theme.textDim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
    }

    component FieldBox: Rectangle {
        property Item field
        width: Math.min(settings.width * 0.7, Theme.posterWidth * 5)
        height: Theme.fontBody + 3 * Theme.spacingS
        radius: Theme.radius / 2
        color: Theme.surface
        border.color: field && field.activeFocus ? Theme.accent : Theme.textDim
        border.width: Math.max(2, Theme.spacingS / 4)
    }
}
