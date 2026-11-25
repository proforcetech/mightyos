pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// The Guide menu over a running app (PLAN section 3, M6): Resume, and Close
// (or, for a Steam launch MightyOS can only stop tracking, "Stop waiting" /
// "Back to MightyOS"). Drawn in the layer-shell overlay window
// (GuideOverlay) or, without layer shell, inside the main window.
//
//   ┌──────────────────────────────────────────────────────────┐
//   │ MightyOS                                          21:42  │
//   │                                                          │
//   │                       [ icon ]                           │
//   │                  Super Mario Bros.                       │
//   │                 Playing for 12 min                       │
//   │            ┌──────────────────────────────┐              │
//   │            │ Resume                       │ ◀ focus      │
//   │            └──────────────────────────────┘              │
//   │            ┌──────────────────────────────┐              │
//   │            │ Close Super Mario Bros.      │              │
//   │            └──────────────────────────────┘              │
//   │               Press Home again to close                  │
//   │ (A) Select  (B) Resume                                   │
//   └──────────────────────────────────────────────────────────┘
//
// Focus: Resume first. Up/Down move (bump at the ends), A chooses, B and
// Guide resume. Close of a game or app asks first (Dialog, Cancel focused,
// B cancels); a web app or stream closes at once (PLAN D25: no unsaved
// state). Trust (D18): Close is a secure choice, so the menu doesn't accept
// the TV remote (Main binds InputRouter.acceptRemote): the remote can only
// resume (Back / Home); closing from the remote is the D25 double Home of a
// closable launch in "full" mode, whose hint shows here.
Item {
    id: menu

    property bool open: false
    property string title
    property string iconName: "placeholder"
    // Web app / stream: Close without a confirmation.
    property bool closable: false
    // A Steam launch: only tracking can end (the game is Steam's).
    property bool tracked: false
    property bool waiting: false
    property bool remoteCloseArmed: false
    // ms since epoch when the app started (0: unknown).
    property double startedAt: 0
    property int currentIndex: 0
    property real bumpY: 0
    readonly property bool acceptsRemote: false
    readonly property alias confirmOpen: confirm.open

    signal resumeRequested()
    signal closeRequested()
    signal stopTrackingRequested()

    readonly property var items: [
        { id: "resume", label: qsTr("Resume") },
        menu.tracked
            ? { id: "stop", label: menu.waiting ? qsTr("Stop waiting") : qsTr("Back to MightyOS") }
            : { id: "close", label: qsTr("Close %1").arg(menu.title), destructive: true }
    ]

    readonly property var prompts: confirm.open ? confirm.prompts : [
        { action: InputRouter.Accept, label: qsTr("Select") },
        { action: InputRouter.Back, label: qsTr("Resume") }
    ]

    // Fresh every time it opens: Resume focused, no confirmation pending.
    function reset() {
        currentIndex = 0
        if (confirm.open)
            confirm.handleAction(InputRouter.Back, "")
        clock.refresh()
        _minutes = _elapsedMinutes()
    }

    property int _minutes: 0
    function _elapsedMinutes(): int {
        return startedAt > 0 ? Math.max(0, Math.floor((Date.now() - startedAt) / 60000)) : -1
    }

    function _choose(index: int) {
        const id = items[index].id
        SoundFx.play(SoundFx.Accept)
        if (id === "resume") {
            resumeRequested()
        } else if (id === "stop") {
            stopTrackingRequested()
        } else if (closable) {
            closeRequested()
        } else {
            confirm.show({
                title: qsTr("Close %1?").arg(menu.title),
                message: qsTr("Progress that isn't saved in the game is lost."),
                buttons: [{ id: "cancel", label: qsTr("Cancel") },
                          { id: "close", label: qsTr("Close"), destructive: true }],
                acceptsRemote: false,
                onFinished: (answer) => {
                    if (answer === "close")
                        menu.closeRequested()
                }
            })
        }
    }

    // Consumes every action while open (focus trap). Guide is Main's
    // (resume, or the remote's second Home).
    function handleAction(action: int, arg: string): bool {
        if (confirm.open)
            return confirm.handleAction(action, arg)
        switch (action) {
        case InputRouter.Navigate:
            if (arg === "up" || arg === "down") {
                const next = currentIndex + (arg === "up" ? -1 : 1)
                if (next < 0 || next >= items.length) {
                    if (!_bump.running) {
                        _bump.dir = arg === "up" ? -1 : 1
                        _bump.restart()
                        SoundFx.play(SoundFx.Bump)
                    }
                } else {
                    currentIndex = next
                    SoundFx.play(SoundFx.Move)
                }
            } else {
                SoundFx.play(SoundFx.Bump)
            }
            break
        case InputRouter.Accept:
            _choose(currentIndex)
            break
        case InputRouter.Back:
            SoundFx.play(SoundFx.Back)
            resumeRequested()
            break
        default:
            SoundFx.play(SoundFx.Bump)
            break
        }
        return true
    }

    onOpenChanged: {
        if (open)
            reset()
    }

    visible: open
    focus: open

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
    }

    // Modal: clicks outside the items do nothing.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Item {
        id: safeArea
        anchors.fill: parent
        anchors.leftMargin: Math.round(menu.width * Theme.safeAreaRatio)
        anchors.rightMargin: anchors.leftMargin
        anchors.topMargin: Math.round(menu.height * Theme.safeAreaRatio)
        anchors.bottomMargin: anchors.topMargin

        Text {
            textFormat: Text.PlainText
            anchors.left: parent.left
            anchors.verticalCenter: clock.verticalCenter
            text: qsTr("MightyOS")
            color: Theme.textDim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
        }
        Clock {
            id: clock
            anchors.right: parent.right
            anchors.top: parent.top
            active: menu.open
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(Theme.panelWidth, parent.width)
            spacing: Theme.spacingM

            Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Theme.px(96)
                height: width
                name: menu.iconName
                color: Theme.accent
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: menu.title
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: menu.waiting ? qsTr("Waiting for Steam")
                    : menu._minutes < 0 ? qsTr("Running")
                    : menu._minutes < 1 ? qsTr("Started just now")
                    : qsTr("Running for %n min", "", menu._minutes)
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                bottomPadding: Theme.spacingM
            }

            Repeater {
                model: menu.items
                delegate: Rectangle {
                    id: entry
                    required property int index
                    required property var modelData
                    readonly property bool current: index === menu.currentIndex && !confirm.open

                    objectName: "guideItem_" + modelData.id
                    width: parent ? parent.width : 0
                    height: Theme.menuItemHeight
                    radius: Theme.radius
                    color: current ? Theme.surfaceHigh : Theme.surface
                    border.width: current ? Theme.focusBorder : Math.max(1, Theme.px(2))
                    border.color: current ? Theme.accent : Theme.surfaceHigh
                    transform: Translate { y: entry.current ? menu.bumpY : 0 }

                    Text {
                        textFormat: Text.PlainText
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.spacingL
                        width: parent.width - 2 * Theme.spacingL
                        elide: Text.ElideRight
                        text: entry.modelData.label
                        color: entry.modelData.destructive ? Theme.danger : Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontLabel
                        font.weight: entry.current ? Font.DemiBold : Font.Normal
                    }

                    PointerArea {
                        anchors.fill: parent
                        onHovered: menu.currentIndex = entry.index
                        onTapped: (mouseButton) => {
                            if (mouseButton === Qt.LeftButton && InputRouter.pointerAction())
                                menu._choose(entry.index)
                        }
                    }
                }
            }

            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: menu.remoteCloseArmed
                text: qsTr("Press Home again to close")
                color: Theme.accent
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                topPadding: Theme.spacingS
            }
        }

        PromptBar {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            prompts: menu.prompts
        }
    }

    Dialog {
        id: confirm
        objectName: "guideConfirm"
        anchors.fill: parent
    }

    SequentialAnimation {
        id: _bump
        property int dir: 1
        NumberAnimation {
            target: menu; property: "bumpY"; to: _bump.dir * Theme.bumpDistance / 2
            duration: Theme.animFast / 2; easing.type: Easing.OutQuad
        }
        NumberAnimation {
            target: menu; property: "bumpY"; to: 0
            duration: Theme.animFast; easing.type: Easing.OutBack
        }
    }
}
