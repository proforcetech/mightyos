import QtQuick
import MightyOS.Core

// Active profile in the top bar: avatar, name and a "Kids" badge.
// Clicking it opens the profile picker (same as Home > Options > Switch
// profile); it is not focusable, so D-pad focus never lands up here.
Item {
    id: chip

    signal clicked()

    readonly property var _profile: ProfileService.active

    implicitWidth: content.implicitWidth
    implicitHeight: content.implicitHeight

    Row {
        id: content
        spacing: Theme.spacingS

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.px(44)
            height: width
            radius: width / 2
            color: Theme.surface
            border.width: Math.max(1, Theme.px(2))
            border.color: chip._profile.color ?? Theme.accent
            Icon {
                anchors.centerIn: parent
                width: parent.width * 0.66
                height: width
                name: "avatar-" + (chip._profile.avatar ?? "")
                color: chip._profile.color ?? Theme.accent
            }
        }
        Text {
            textFormat: Text.PlainText
            anchors.verticalCenter: parent.verticalCenter
            text: chip._profile.name ?? ""
            color: Theme.textDim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSmall
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            visible: chip._profile.kind === "kids"
            width: kidsText.implicitWidth + 2 * Theme.spacingS
            height: kidsText.implicitHeight + Theme.px(4)
            radius: height / 2
            color: chip._profile.color ?? Theme.accent
            Text {
                textFormat: Text.PlainText
                id: kidsText
                anchors.centerIn: parent
                text: qsTr("Kids")
                color: Theme.accentText
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Bold
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        anchors.margins: -Theme.spacingS
        onClicked: {
            if (InputRouter.pointerAction())
                chip.clicked()
        }
    }
}
