pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// "Who's playing?" — choose, create, edit and delete profiles.
//
// Shown at startup when there are 2+ profiles (last used preselected), and
// from Home's options menu / Settings > Profiles. It sits above the
// NavStack, outside of any profile's navigation, and is modal: Main routes
// every action here while it is open (after Dialog and ContextMenu).
//
//   [#Player 1#] [Comet] [Pixel·Kids] [+ Add profile]
//
// Left/Right move (bump), Accept chooses (or opens the editor on "Add"),
// Options: Choose / Rename & avatar / Delete, Back closes (not at startup,
// where a profile must be chosen). Delete asks first; the last profile
// can't be deleted.
Item {
    id: picker

    property bool open: false
    // At startup there is nothing to go back to.
    property bool startup: false

    signal chosen(string profileId)
    // Emitted right before ProfileService.remove(), so Main can drop the
    // screens of a profile that is about to disappear.
    signal removing(string profileId)
    signal closed()
    signal toastRequested(string message, bool isError)
    signal dialogRequested(var spec)
    signal optionsRequested(string title, var items)

    readonly property var focusedRow: row.currentEntry()

    readonly property var prompts: editor.open ? editor.prompts : _prompts
    readonly property var _prompts: {
        const list = [{ action: InputRouter.Accept,
                        label: focusedRow && focusedRow.kind === "add" ? qsTr("Create") : qsTr("Choose") }]
        if (focusedRow && focusedRow.kind === "profile")
            list.push({ action: InputRouter.Options, label: qsTr("Options") })
        if (!startup)
            list.push({ action: InputRouter.Back, label: qsTr("Back") })
        return list
    }

    function show(atStartup: bool) {
        startup = atStartup
        _rebuild()
        row.restoreFocus(0, ProfileService.activeId)
        open = true
    }

    function close() {
        editor.close()
        open = false
    }

    function _rebuild() {
        const rows = ProfileService.profiles.map(p => ({
            key: p.id, title: p.name, icon: "avatar-" + p.avatar, tint: p.color,
            kind: "profile", badge: p.kind === "kids" ? qsTr("Kids") : "",
            subtitle: p.id === ProfileService.activeId && !picker.startup ? qsTr("Current") : ""
        }))
        if (rows.length < ProfileService.maxProfiles)
            rows.push({ key: "add", title: qsTr("Add profile"), icon: "add", tint: "#a0aec0", kind: "add" })
        pickerModel.setRows(rows)
    }

    Connections {
        target: ProfileService
        function onProfilesChanged() {
            if (picker.open)
                picker._rebuild()
        }
    }

    function _activate(item: var) {
        if (!item)
            return
        if (item.kind === "add") {
            editor.show("create", "")
            return
        }
        chosen(item.key)
    }

    function _confirmDelete(item: var) {
        if (ProfileService.count <= 1) {
            toastRequested(qsTr("The last profile can't be deleted"), true)
            return
        }
        const id = item.key
        const name = item.title
        dialogRequested({
            title: qsTr("Delete %1?").arg(name),
            message: qsTr("Its favorites, history and saved position are deleted too. This can't be undone."),
            buttons: [
                { id: "keep", label: qsTr("Keep") },
                { id: "delete", label: qsTr("Delete"), destructive: true }
            ],
            onFinished: (answer) => {
                if (answer !== "delete")
                    return
                picker.removing(id)
                if (ProfileService.remove(id)) {
                    row.restoreFocus(0, ProfileService.activeId)   // land on a profile, not "Add"
                    picker.toastRequested(qsTr("Deleted %1").arg(name), false)
                }
                else
                    picker.toastRequested(qsTr("Could not delete %1").arg(name), true)
            }
        })
    }

    // Called by Main when the context menu requested above is answered.
    function optionChosen(id: string) {
        const item = row.currentEntry()
        if (!item || item.kind !== "profile")
            return
        switch (id) {
        case "choose": chosen(item.key); break
        case "edit":   editor.show("edit", item.key); break
        case "delete": _confirmDelete(item); break
        }
    }

    function handleAction(action: int, arg: string): bool {
        if (editor.open)
            return editor.handleAction(action, arg)
        switch (action) {
        case InputRouter.Navigate:
            if (arg === "left" || arg === "right")
                row.moveFocus(arg)
            else
                SoundFx.play(SoundFx.Bump)
            break
        case InputRouter.Accept:
            row.activateCurrent()
            break
        case InputRouter.Options: {
            const item = row.currentEntry()
            if (item && item.kind === "profile")
                optionsRequested(item.title, [
                    { id: "choose", label: qsTr("Choose") },
                    { id: "edit", label: qsTr("Rename / change avatar") },
                    { id: "delete", label: qsTr("Delete"), destructive: true }
                ])
            else
                SoundFx.play(SoundFx.Bump)
            break
        }
        case InputRouter.Back:
            if (startup) {
                SoundFx.play(SoundFx.Bump)
            } else {
                SoundFx.play(SoundFx.Back)
                close()
                closed()
            }
            break
        default:
            SoundFx.play(SoundFx.Bump)
            break
        }
        return true   // modal
    }

    visible: open
    enabled: open

    // Opaque: nothing of the profile behind it shows through.
    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => {
            wheel.accepted = true
            if (!editor.open && InputRouter.pointerAction())
                row.wheelStep(wheel.angleDelta.y < 0 ? 1 : -1)
        }
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton && InputRouter.pointerAction())
                InputRouter.dispatch(InputRouter.Options, "", InputRouter.Mouse)
        }
    }

    TileListModel { id: pickerModel }

    Column {
        anchors.centerIn: parent
        width: parent.width
        spacing: Theme.spacingL

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Who's playing?")
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontHuge
            font.weight: Font.Bold
        }

        Item {
            width: parent.width
            height: row.height + 2 * Theme.focusOverflow

            TileRow {
                id: row
                objectName: "profileRow"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                model: pickerModel
                showFavorites: false
                active: picker.open && !editor.open
                onActivated: (index) => picker._activate(pickerModel.get(index))
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(implicitWidth, parent.width)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            visible: picker.focusedRow !== null && picker.focusedRow.kind === "profile"
                     && picker.focusedRow.badge.length > 0
            text: qsTr("Kids profiles are not restricted yet (M8).")
            color: Theme.textDim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSmall
        }
    }

    ProfileEditor {
        id: editor
        objectName: "profileEditor"
        anchors.fill: parent
        onToastRequested: (message, isError) => picker.toastRequested(message, isError)
        onFinished: (id) => {
            if (id.length > 0) {
                picker._rebuild()
                row.restoreFocus(row.currentIndex, id)
            }
        }
    }
}
