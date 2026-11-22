pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// Modal confirmation dialog. Focus-trapped like ContextMenu (Main routes
// every action here first). Focus starts on the first non-destructive
// button; B / Esc answers with that same "cancel" button, so the safe
// choice is always the default and the back action.
//
// show({ title, message, buttons: [{ id, label, destructive? }],
//        onFinished: function(id) {} })
// The callback (and the finished signal) receive the chosen button id.
Item {
    id: dialog

    property bool open: false
    property string title
    property string message
    property var buttons: []
    property int currentIndex: 0
    property real bumpX: 0

    property var _onFinished: null

    signal finished(string id)

    readonly property int cancelIndex: {
        for (let i = 0; i < buttons.length; ++i) {
            if (!buttons[i].destructive)
                return i
        }
        return -1
    }

    readonly property var prompts: [
        { action: InputRouter.Accept, label: qsTr("Select") },
        { action: InputRouter.Back, label: qsTr("Cancel") }
    ]

    function show(spec: var) {
        title = spec.title ?? ""
        message = spec.message ?? ""
        buttons = spec.buttons && spec.buttons.length > 0
            ? spec.buttons : [{ id: "ok", label: qsTr("OK") }]
        _onFinished = spec.onFinished ?? null
        currentIndex = Math.max(0, cancelIndex)
        open = true
    }

    function _finish(index: int) {
        const id = index >= 0 && index < buttons.length ? buttons[index].id : ""
        const callback = _onFinished
        _onFinished = null
        open = false
        finished(id)
        if (callback)
            callback(id)
    }

    function handleAction(action: int, arg: string): bool {
        switch (action) {
        case InputRouter.Navigate:
            if (arg === "left" || arg === "right") {
                const next = currentIndex + (arg === "left" ? -1 : 1)
                if (next < 0 || next >= buttons.length) {
                    if (!_bump.running) {
                        _bump.dir = arg === "left" ? -1 : 1
                        _bump.restart()
                        SoundFx.play(SoundFx.Bump)
                    }
                } else {
                    currentIndex = next
                    SoundFx.play(SoundFx.Move)
                }
            }
            break
        case InputRouter.Accept:
            SoundFx.play(SoundFx.Accept)
            _finish(currentIndex)
            break
        case InputRouter.Back:
            SoundFx.play(SoundFx.Back)
            _finish(cancelIndex)
            break
        }
        return true
    }

    visible: opacity > 0
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.animFast } }

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
    }

    // Modal: a click outside does nothing (no accidental confirm/cancel).
    MouseArea {
        anchors.fill: parent
        enabled: dialog.open
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(Theme.panelWidth * 1.3, dialog.width - 2 * Theme.spacingL)
        height: content.implicitHeight + 2 * Theme.spacingL
        radius: Theme.radius
        color: Theme.surface
        border.width: Math.max(1, Theme.px(2))
        border.color: Theme.surfaceHigh

        Column {
            id: content
            x: Theme.spacingL
            y: Theme.spacingL
            width: parent.width - 2 * Theme.spacingL
            spacing: Theme.spacingM

            Text {
                width: parent.width
                text: dialog.title
                wrapMode: Text.Wrap
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontTitle * 0.7
                font.weight: Font.Bold
            }
            Text {
                width: parent.width
                text: dialog.message
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
            }

            Row {
                anchors.right: parent.right
                spacing: Theme.spacingM
                topPadding: Theme.spacingS

                Repeater {
                    model: dialog.buttons
                    delegate: Rectangle {
                        id: button
                        required property int index
                        required property var modelData
                        readonly property bool current: index === dialog.currentIndex

                        width: Math.max(Theme.px(200), label.implicitWidth + 2 * Theme.spacingL)
                        height: Theme.menuItemHeight
                        radius: height / 2
                        color: current ? Theme.surfaceHigh : Theme.backgroundAlt
                        border.width: current ? Theme.focusBorder : Math.max(1, Theme.px(2))
                        border.color: current ? Theme.accent : Theme.surfaceHigh
                        transform: Translate { x: button.current ? dialog.bumpX : 0 }

                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: button.modelData.label
                            color: button.modelData.destructive ? Theme.danger : Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            font.weight: button.current ? Font.DemiBold : Font.Normal
                        }

                        PointerArea {
                            anchors.fill: parent
                            onHovered: dialog.currentIndex = button.index
                            onTapped: (mouseButton) => {
                                if (mouseButton === Qt.LeftButton && InputRouter.pointerAction())
                                    dialog._finish(button.index)
                            }
                        }
                    }
                }
            }
        }
    }

    SequentialAnimation {
        id: _bump
        property int dir: 1
        NumberAnimation {
            target: dialog; property: "bumpX"; to: _bump.dir * Theme.bumpDistance / 2
            duration: Theme.animFast / 2; easing.type: Easing.OutQuad
        }
        NumberAnimation {
            target: dialog; property: "bumpX"; to: 0
            duration: Theme.animFast; easing.type: Easing.OutBack
        }
    }
}
