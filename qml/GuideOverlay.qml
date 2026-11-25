import QtQuick
import QtQuick.Window
import MightyOS.Core

// The Guide overlay window (GuideOverlayHost): a transparent, full-output
// surface on the wlr-layer-shell overlay layer above the running app. It is
// created when the Guide menu opens and destroyed when it closes. All state
// lives in the main window (`host`); input reaches it through InputRouter,
// which routes to the menu registered here (host.overlayMenu).
Window {
    id: overlay

    required property Main host

    visible: false   // GuideOverlayHost sets up layer shell first, then shows it
    color: "transparent"
    title: qsTr("MightyOS Guide")
    width: host.width
    height: host.height

    GuideMenu {
        id: menu
        anchors.fill: parent
        open: overlay.host.guideOpen
        title: overlay.host.launchTitle
        iconName: overlay.host.launchIcon
        closable: overlay.host.launchClosable
        tracked: Launcher.tracked
        waiting: Launcher.waiting
        remoteCloseArmed: overlay.host.remoteCloseArmed
        startedAt: overlay.host.launchStartedAt
        onResumeRequested: overlay.host.closeGuide()
        onCloseRequested: overlay.host.guideCloseApp()
        onStopTrackingRequested: overlay.host.guideStopTracking()
    }

    Component.onCompleted: host.overlayMenu = menu
    Component.onDestruction: {
        if (host.overlayMenu === menu)
            host.overlayMenu = null
    }
}
