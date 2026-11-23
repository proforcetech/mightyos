pragma ComponentBehavior: Bound
import QtQuick
import MightyOS.Core

// Options menu (Start / M / right click). Modal and focus-trapped: while
// open, Main routes every action here first and handleAction() consumes
// all of them. B / Esc / Start closes it, as does a click outside the panel.
//
// items: [{ id, label, destructive? }]. Emits chosen(id) after closing.
Item {
    id: menu

    property bool open: false
    property string title
    property var items: []
    property int currentIndex: 0
    property real bumpY: 0

    signal chosen(string id)

    readonly property var prompts: [
        { action: InputRouter.Accept, label: qsTr("Select") },
        { action: InputRouter.Back, label: qsTr("Close") }
    ]

    function show(menuTitle: string, menuItems: var) {
        title = menuTitle
        items = menuItems
        currentIndex = 0
        open = true
    }

    function close() {
        open = false
    }

    function _choose(index: int) {
        if (index < 0 || index >= items.length)
            return
        const id = items[index].id
        close()
        SoundFx.play(SoundFx.Accept)
        chosen(id)
    }

    // Consumes every action (focus trap).
    function handleAction(action: int, arg: string): bool {
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
            }
            break
        case InputRouter.Accept:
            _choose(currentIndex)
            break
        case InputRouter.Back:
        case InputRouter.Options:
            close()
            SoundFx.play(SoundFx.Back)
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

    // Clicks outside the panel close the menu; nothing leaks through.
    MouseArea {
        anchors.fill: parent
        enabled: menu.open
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onClicked: menu.close()
        onWheel: (wheel) => wheel.accepted = true
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(Theme.panelWidth, menu.width - 2 * Theme.spacingL)
        height: column.implicitHeight + 2 * Theme.spacingM
        radius: Theme.radius
        color: Theme.surface
        border.width: Math.max(1, Theme.px(2))
        border.color: Theme.surfaceHigh

        // Swallow clicks on the panel background.
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Column {
            id: column
            x: Theme.spacingM
            y: Theme.spacingM
            width: parent.width - 2 * Theme.spacingM
            spacing: Theme.px(4)

            Text {
                textFormat: Text.PlainText
                width: parent.width
                text: menu.title
                visible: text.length > 0
                color: Theme.textDim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
                bottomPadding: Theme.spacingS
                leftPadding: Theme.spacingM
            }

            Repeater {
                model: menu.items
                delegate: Rectangle {
                    id: entry
                    required property int index
                    required property var modelData
                    readonly property bool current: index === menu.currentIndex

                    width: column.width
                    height: Theme.menuItemHeight
                    radius: Theme.radius / 2
                    color: current ? Theme.surfaceHigh : "transparent"
                    border.width: current ? Theme.focusBorder : 0
                    border.color: Theme.accent
                    transform: Translate { y: entry.current ? menu.bumpY : 0 }

                    Text {
                        textFormat: Text.PlainText
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.spacingM
                        width: parent.width - 2 * Theme.spacingM
                        text: entry.modelData.label
                        elide: Text.ElideRight
                        color: entry.modelData.destructive ? Theme.danger : Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                        font.weight: entry.current ? Font.DemiBold : Font.Normal
                    }

                    PointerArea {
                        anchors.fill: parent
                        onHovered: menu.currentIndex = entry.index
                        onTapped: (button) => {
                            if (button === Qt.LeftButton && InputRouter.pointerAction())
                                menu._choose(entry.index)
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
            target: menu; property: "bumpY"; to: _bump.dir * Theme.bumpDistance / 2
            duration: Theme.animFast / 2; easing.type: Easing.OutQuad
        }
        NumberAnimation {
            target: menu; property: "bumpY"; to: 0
            duration: Theme.animFast; easing.type: Easing.OutBack
        }
    }
}
