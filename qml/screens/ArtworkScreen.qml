import QtQuick
import MightyOS.Core
import MightyOS.UI

// Settings > Artwork: the SteamGridDB API key (PLAN D11). Scraping is
// opt-in: nothing is downloaded until a key is saved here.
//
//   Artwork
//   SteamGridDB box art · Ready / Off: no API key / ...
//   [Enter key] [Remove key]                     <- TileRow (focus)
//   API key  [ text field, only while editing ]
//   Key: ••••••••1a2b (private file)
//
// Entering: Accept on "Enter key" focuses a plain text field (keyboard
// typing goes straight to it; InputRouter steps aside while a text field
// has focus, except Esc). Enter / A saves, Esc / B cancels. Nothing is
// echoed back after saving: the key is only shown masked. There is no
// on-screen keyboard before M7, so a gamepad alone can't type a key yet.
// The TV remote can't operate this screen (it can only leave it), and
// action logging is off while the field is open.
NavScreen {
    id: artwork

    valid: !Library.restricted   // kids profiles: no app list / settings
    title: qsTr("Artwork")
    acceptsRemote: false
    secureEntry: editing

    property bool editing: false

    prompts: editing
        ? [{ action: InputRouter.Accept, label: qsTr("Save") },
           { action: InputRouter.Back, label: qsTr("Cancel") }]
        : [{ action: InputRouter.Accept, label: qsTr("Select") },
           { action: InputRouter.Back, label: qsTr("Back") }]

    function _rows(): var {
        const rows = [{ key: "enter", title: Scraper.hasKey ? qsTr("Change key") : qsTr("Enter key"),
                        icon: "add", tint: "#66c0f4", kind: "action" }]
        if (Scraper.hasKey)
            rows.push({ key: "remove", title: qsTr("Remove key"), icon: "power", tint: "#ff6b6b", kind: "action" })
        return rows
    }

    function _updateFocused() {
        focusedItem = row.currentEntry()
    }

    function _startEditing() {
        field.text = ""
        editing = true
        field.forceActiveFocus()
    }

    function _stopEditing() {
        field.text = ""
        editing = false
        focusSink.forceActiveFocus()   // keys go back to InputRouter
    }

    function _save() {
        if (Scraper.setApiKey(field.text)) {
            _stopEditing()
            return
        }
        SoundFx.play(SoundFx.Bump)
        toastRequested(qsTr("That doesn't look like a SteamGridDB API key (16-128 letters and digits)"), true)
    }

    function _activate(key: string) {
        if (key === "enter") {
            _startEditing()
        } else if (key === "remove") {
            dialogRequested({
                title: qsTr("Remove the API key?"),
                message: qsTr("Artwork already downloaded stays. No new artwork is fetched."),
                acceptsRemote: false,
                buttons: [{ id: "keep", label: qsTr("Keep") },
                          { id: "remove", label: qsTr("Remove"), destructive: true }],
                onFinished: (id) => {
                    if (id === "remove") {
                        Scraper.clearApiKey()
                        artwork.toastRequested(qsTr("API key removed"), false)
                    }
                }
            })
        }
    }

    function handleAction(action: int, arg: string): bool {
        if (editing) {
            if (action === InputRouter.Accept)
                _save()
            else if (action === InputRouter.Back)
                _stopEditing()
            return true   // nothing else while the field is open
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

    TileListModel { id: actions }

    Connections {
        target: Scraper
        function onKeyChanged() {
            actions.setRows(artwork._rows())
            artwork._updateFocused()
        }
        function onKeySaved(ok, message) {
            artwork.toastRequested(message, !ok)
        }
    }

    Component.onCompleted: {
        actions.setRows(_rows())
        row.currentIndex = 0
        _updateFocused()
    }

    // Takes the keyboard focus back from the text field.
    Item { id: focusSink }

    Column {
        anchors.fill: parent
        spacing: Theme.spacingM

        InfoHeader {
            width: parent.width
            heading: qsTr("SteamGridDB box art")
            detail: Scraper.status
        }

        Item {
            width: parent.width
            height: row.height + 2 * Theme.focusOverflow

            TileRow {
                id: row
                anchors.verticalCenter: parent.verticalCenter
                model: actions
                showFavorites: false
                active: artwork.focusShown && !artwork.editing
                onCurrentIndexChanged: {
                    artwork._updateFocused()
                    artwork.navStateChanged()
                }
                onActivated: (index) => artwork._activate(actions.keyAt(index))
            }
        }

        Rectangle {
            visible: artwork.editing
            width: Math.min(parent.width, Theme.posterWidth * 5)
            height: field.implicitHeight + 2 * Theme.spacingS
            radius: Theme.radius / 2
            color: Theme.surface
            border.color: Theme.accent
            border.width: Math.max(2, Theme.spacingS / 4)

            TextInput {
                id: field
                objectName: "apiKeyField"
                anchors.fill: parent
                anchors.margins: Theme.spacingS
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                maximumLength: 128
                // Shown while typing, dots once the field loses focus.
                echoMode: TextInput.PasswordEchoOnEdit
                persistentSelection: false
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData
                clip: true
                onAccepted: artwork._save()
                Keys.onEscapePressed: artwork._stopEditing()
                // No undo/redo history of a secret (setting text clears it too).
                Keys.onPressed: (event) => {
                    if ((event.modifiers & Qt.ControlModifier)
                        && (event.key === Qt.Key_Z || event.key === Qt.Key_Y))
                        event.accepted = true
                }
            }
        }

        Text {
            textFormat: Text.PlainText
            width: parent.width
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSmall
            text: Scraper.hasKey
                ? qsTr("Key: %1 (stored in a %2). Steam games use Steam's own artwork first.")
                      .arg(Scraper.maskedKey).arg(Scraper.keyStorage)
                : qsTr("Get a free API key at steamgriddb.com (Preferences > API) and enter it here. "
                       + "Until then games show tinted placeholder tiles. Steam games use Steam's own artwork.")
        }
    }
}
