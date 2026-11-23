pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// Create / edit a profile without a keyboard (the on-screen keyboard
// arrives later): pick a name from generated suggestions, an avatar, a
// colour and the kind. Modal and focus-trapped; ProfilePicker routes every
// action here while it is open.
//
//   Name     (Comet) (Pixel) (Nova) ...       row 0   Left/Right = choose
//   Avatar   [bot] [cat] [ghost] ...          row 1
//   Colour   (o) (o) (o) ...                  row 2
//   Type     (Standard) (Kids)                row 3
//                       (Cancel) (#Create#)   row 4   Accept = press
//
// Up/Down move between rows (bump at the ends), Accept on rows 0-3 moves
// down, Back cancels. Writes go through ProfileService.
Item {
    id: editor

    property bool open: false
    property string mode: "create"        // "create" | "edit"
    property string profileId
    property int focusRow: 0
    property int buttonIndex: 1           // 0 Cancel, 1 Create/Save

    // profileId of the created/edited profile, "" if cancelled.
    signal finished(string profileId)
    signal toastRequested(string message, bool isError)

    readonly property var prompts: [
        { action: InputRouter.Accept, label: focusRow === 4 ? qsTr("Select") : qsTr("Next") },
        { action: InputRouter.Back, label: qsTr("Cancel") }
    ]

    readonly property var _rows: [nameRow, avatarRow, colorRow, kindRow]

    function show(editMode: string, id: string) {
        mode = editMode
        profileId = id
        const p = editMode === "edit" ? ProfileService.profile(id) : null
        let names = ProfileService.suggestedNames(editMode === "edit" ? 5 : 6)
        if (p && p.name)
            names = [p.name].concat(names)
        nameRow.choices = names.map(n => ({ id: n, label: n }))
        nameRow.currentIndex = 0
        avatarRow.selectId(p ? p.avatar : ProfileService.avatars[ProfileService.count % ProfileService.avatars.length])
        colorRow.selectId(p ? p.color : ProfileService.colors[ProfileService.count % ProfileService.colors.length])
        kindRow.selectId(p ? p.kind : "standard")
        focusRow = 0
        buttonIndex = 1
        open = true
    }

    function close() {
        open = false
    }

    function _submit() {
        const name = nameRow.currentId
        const avatar = avatarRow.currentId
        const color = colorRow.currentId
        const kind = kindRow.currentId
        let id = profileId
        if (mode === "create") {
            id = ProfileService.create(name, avatar, color, kind)
            if (id.length === 0) {
                toastRequested(qsTr("Could not create the profile"), true)
                return
            }
        } else if (!ProfileService.update(id, name, avatar, color, kind)) {
            toastRequested(qsTr("Could not save the profile"), true)
            return
        }
        SoundFx.play(SoundFx.Accept)
        close()
        finished(id)
    }

    function _cancel() {
        SoundFx.play(SoundFx.Back)
        close()
        finished("")
    }

    function _moveRow(delta: int) {
        const next = focusRow + delta
        if (next < 0 || next > 4) {
            SoundFx.play(SoundFx.Bump)
            return
        }
        focusRow = next
        SoundFx.play(SoundFx.Move)
    }

    function handleAction(action: int, arg: string): bool {
        switch (action) {
        case InputRouter.Navigate:
            if (arg === "up" || arg === "down") {
                _moveRow(arg === "up" ? -1 : 1)
            } else if (focusRow < 4) {
                _rows[focusRow].moveFocus(arg)
            } else {
                const next = buttonIndex + (arg === "left" ? -1 : 1)
                if (next < 0 || next > 1) {
                    SoundFx.play(SoundFx.Bump)
                } else {
                    buttonIndex = next
                    SoundFx.play(SoundFx.Move)
                }
            }
            break
        case InputRouter.Accept:
            if (focusRow < 4)
                _moveRow(1)
            else if (buttonIndex === 1)
                _submit()
            else
                _cancel()
            break
        case InputRouter.Back:
            _cancel()
            break
        }
        return true   // focus trap
    }

    visible: opacity > 0
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.animFast } }

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
    }
    MouseArea {
        anchors.fill: parent
        enabled: editor.open
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(Theme.px(1300), editor.width - 2 * Theme.spacingM)
        height: column.implicitHeight + 2 * Theme.spacingL
        radius: Theme.radius
        color: Theme.surface
        border.width: Math.max(1, Theme.px(2))
        border.color: Theme.surfaceHigh

        Column {
            id: column
            x: Theme.spacingL
            y: Theme.spacingL
            width: parent.width - 2 * Theme.spacingL
            spacing: Theme.spacingS

            Row {
                spacing: Theme.spacingM
                Rectangle {
                    width: Theme.px(96)
                    height: width
                    radius: width / 2
                    color: Theme.backgroundAlt
                    border.width: Math.max(2, Theme.px(4))
                    border.color: colorRow.currentId || Theme.accent
                    Icon {
                        anchors.centerIn: parent
                        width: parent.width * 0.62
                        height: width
                        name: "avatar-" + avatarRow.currentId
                        color: colorRow.currentId || Theme.accent
                    }
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        textFormat: Text.PlainText
                        text: editor.mode === "create" ? qsTr("New profile") : qsTr("Edit profile")
                        color: Theme.textDim
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSmall
                    }
                    Text {
                        textFormat: Text.PlainText
                        text: nameRow.currentId + (kindRow.currentId === "kids" ? "  ·  " + qsTr("Kids") : "")
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontTitle * 0.7
                        font.weight: Font.Bold
                    }
                }
            }

            ChoiceRow {
                id: nameRow
                objectName: "editorName"
                label: qsTr("Name")
                active: editor.open && editor.focusRow === 0
                onFocusRequested: editor.focusRow = 0
            }
            ChoiceRow {
                id: avatarRow
                objectName: "editorAvatar"
                label: qsTr("Avatar")
                style: "icon"
                iconColor: colorRow.currentId || Theme.text
                choices: ProfileService.avatars.map(a => ({ id: a, icon: "avatar-" + a }))
                active: editor.open && editor.focusRow === 1
                onFocusRequested: editor.focusRow = 1
            }
            ChoiceRow {
                id: colorRow
                objectName: "editorColor"
                label: qsTr("Colour")
                style: "swatch"
                choices: ProfileService.colors.map(c => ({ id: c, color: c }))
                active: editor.open && editor.focusRow === 2
                onFocusRequested: editor.focusRow = 2
            }
            ChoiceRow {
                id: kindRow
                objectName: "editorKind"
                label: qsTr("Type")
                choices: [{ id: "standard", label: qsTr("Standard") }, { id: "kids", label: qsTr("Kids") }]
                active: editor.open && editor.focusRow === 3
                onFocusRequested: editor.focusRow = 3
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: kindRow.currentId === "kids"
                wrapMode: Text.Wrap
                text: qsTr("Kids profiles are marked with a badge. Restrictions (allowed games, "
                           + "no store or settings, PIN to leave) arrive in M8.")
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSmall
            }

            Row {
                anchors.right: parent.right
                spacing: Theme.spacingM
                topPadding: Theme.spacingS

                Repeater {
                    model: [qsTr("Cancel"), editor.mode === "create" ? qsTr("Create") : qsTr("Save")]
                    delegate: Rectangle {
                        id: button
                        required property int index
                        required property string modelData
                        readonly property bool focused: editor.focusRow === 4 && editor.buttonIndex === index

                        width: Math.max(Theme.px(200), buttonLabel.implicitWidth + 2 * Theme.spacingL)
                        height: Theme.menuItemHeight
                        radius: height / 2
                        color: focused ? Theme.surfaceHigh : Theme.backgroundAlt
                        border.width: focused ? Theme.focusBorder : Math.max(1, Theme.px(2))
                        border.color: focused ? Theme.accent : Theme.surfaceHigh

                        Text {
                            textFormat: Text.PlainText
                            id: buttonLabel
                            anchors.centerIn: parent
                            text: button.modelData
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            font.weight: button.focused ? Font.DemiBold : Font.Normal
                        }
                        PointerArea {
                            anchors.fill: parent
                            onHovered: {
                                editor.focusRow = 4
                                editor.buttonIndex = button.index
                            }
                            onTapped: (mouseButton) => {
                                if (mouseButton !== Qt.LeftButton || !InputRouter.pointerAction())
                                    return
                                if (button.index === 1)
                                    editor._submit()
                                else
                                    editor._cancel()
                            }
                        }
                    }
                }
            }
        }
    }
}
