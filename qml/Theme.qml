pragma Singleton
import QtQuick

// Design tokens. All sizes are authored for 1920x1080 and multiplied by
// `scale`, which Main.qml derives from the window size.
QtObject {
    id: theme

    // Set by Main.qml (Binding). Defaults keep the singleton usable alone.
    property real windowWidth: 1920
    property real windowHeight: 1080

    readonly property real scale: Math.max(0.4, Math.min(windowWidth / 1920, windowHeight / 1080))
    function px(v) { return Math.round(v * scale) }

    // --- Colours (dark, high contrast) ------------------------------------
    readonly property color background:  "#0e1117"
    readonly property color backgroundAlt: "#151a23"
    readonly property color surface:     "#1c2230"
    readonly property color surfaceHigh: "#2a3245"
    readonly property color text:        "#f3f5f9"   // ~17:1 on background
    readonly property color textDim:     "#aeb6c6"   // ~9:1 on background
    readonly property color accent:      "#4da3ff"
    readonly property color accentText:  "#0e1117"
    readonly property color danger:      "#ff6b6b"
    readonly property color scrim:       "#d90b0e14"

    // --- Typography (px at 1080p; body never below 24) --------------------
    readonly property string fontFamily: "Ubuntu Sans"
    readonly property int fontSmall:   px(24)
    readonly property int fontBody:    px(28)
    readonly property int fontLabel:   px(30)
    readonly property int fontTitle:   px(56)
    readonly property int fontHuge:    px(72)

    // --- Layout -----------------------------------------------------------
    readonly property real safeAreaRatio: 0.05     // TV overscan margin
    readonly property int spacingS:  px(12)
    readonly property int spacingM:  px(24)
    readonly property int spacingL:  px(48)
    readonly property int radius:    px(20)
    readonly property int tileSize:  px(240)
    readonly property int tileIconSize: px(104)
    // Game covers in grids (3:4 box art; art arrives in M2/M4).
    readonly property int posterWidth:  px(200)
    readonly property int posterHeight: px(266)
    readonly property int posterIconSize: px(72)
    // Small tiles of the "Recent" row on Home > Favorites.
    readonly property int recentTileSize: px(150)
    // Category bar "pills".
    readonly property int pillHeight: px(76)
    readonly property int pillIconSize: px(40)
    readonly property int pillPadding: px(28)
    // Context menu / dialog panels.
    readonly property int panelWidth: px(720)
    readonly property int menuItemHeight: px(76)

    // --- Focus ------------------------------------------------------------
    readonly property real focusScale: 1.08
    readonly property int focusBorder: Math.max(2, px(6))
    readonly property int focusGap:    Math.max(2, px(6))
    readonly property int bumpDistance: px(14)
    // Room a focused tile + ring needs beyond the tile's own bounds.
    function overflowFor(size) { return Math.ceil(size * (focusScale - 1) / 2) + focusGap + focusBorder }
    readonly property int focusOverflow: overflowFor(tileSize)

    // --- Motion (ms) ------------------------------------------------------
    readonly property int animFast:   120
    readonly property int animNormal: 200
    readonly property int animScreen: 140          // NavStack push/pop (<= 150)
    readonly property int screenShift: px(64)      // push/pop slide distance
    readonly property int inputMuteAfterExit: 300
    readonly property int toastDuration: 2500
    readonly property int cursorHideDelay: 3000
}
