.pragma library

// Static UI structure: the fixed Home categories and the sections of the
// App Store / Settings placeholder screens. Content (systems, games, apps,
// favorites, profiles) comes from the C++ models (Library, AppRegistry,
// Favorites, History, ProfileService); this file holds no mock data.
//
// Rows use the TileListModel roles: key, title, subtitle, icon, tint,
// kind (+ optional sortTitle, badge, artwork, available).

const categories = [
    { key: "favorites", title: qsTr("Favorites"), icon: "favorites", tint: "#ffc94d",
      blurb: qsTr("Your pinned games, apps and systems"),
      open: { screen: "games", params: { collection: "favorites" } } },
    { key: "emulators", title: qsTr("Emulators"), icon: "emulators", tint: "#ff6b8b",
      blurb: qsTr("Retro and modern consoles"),
      open: { screen: "systems", params: { category: "emulators" } } },
    { key: "games", title: qsTr("Games"), icon: "games", tint: "#7bd88f",
      blurb: qsTr("PC game libraries"),
      open: { screen: "systems", params: { category: "games" } } },
    { key: "media", title: qsTr("Media"), icon: "media", tint: "#4da3ff",
      blurb: qsTr("Movies, music and streaming"),
      open: { screen: "media", params: {} } },
    { key: "store", title: qsTr("App Store"), icon: "store", tint: "#b48cff",
      blurb: qsTr("Install apps from Flathub"),
      open: { screen: "category", params: { category: "store" } } },
    { key: "settings", title: qsTr("Settings"), icon: "settings", tint: "#a0aec0",
      blurb: qsTr("Profiles, display, audio, network and controllers"),
      open: { screen: "category", params: { category: "settings" } } }
]

const sections = {
    store: [
        { key: "store:featured",  title: qsTr("Featured"),  icon: "favorites", tint: "#b48cff" },
        { key: "store:games",     title: qsTr("Games"),     icon: "games",     tint: "#7bd88f" },
        { key: "store:emulators", title: qsTr("Emulators"), icon: "emulators", tint: "#ff6b8b" },
        { key: "store:media",     title: qsTr("Media"),     icon: "media",     tint: "#4da3ff" },
        { key: "store:updates",   title: qsTr("Updates"),   icon: "store",     tint: "#a0aec0" }
    ],
    settings: [
        { key: "settings:profiles",    title: qsTr("Profiles"),    icon: "profile",   tint: "#ffc94d", kind: "profiles" },
        { key: "settings:apps",        title: qsTr("All apps"),    icon: "apps",      tint: "#4da3ff", kind: "allapps" },
        { key: "settings:media",       title: qsTr("Media"),       icon: "media",     tint: "#4da3ff", kind: "mediasettings" },
        { key: "settings:display",     title: qsTr("Display"),     icon: "display",   tint: "#a0aec0" },
        { key: "settings:audio",       title: qsTr("Audio"),       icon: "audio",     tint: "#a0aec0" },
        { key: "settings:network",     title: qsTr("Network"),     icon: "network",   tint: "#a0aec0" },
        { key: "settings:controllers", title: qsTr("Controllers"), icon: "emulators", tint: "#a0aec0", kind: "controllers" },
        { key: "settings:artwork",     title: qsTr("Artwork"),     icon: "display",   tint: "#66c0f4", kind: "artwork" },
        { key: "settings:storage",     title: qsTr("Storage"),     icon: "folder",    tint: "#a0aec0" },
        { key: "settings:power",       title: qsTr("Power"),       icon: "power",     tint: "#ff6b6b", kind: "power" }
    ]
}

// Milestone that brings each placeholder section to life.
const sectionMilestone = { store: "M7", settings: "M8" }

function category(id) {
    return categories.find(c => c.key === id) ?? null
}

// `restricted` (a kids profile, PLAN D13): no App Store, no Settings.
function categoryRows(restricted) {
    return categories.filter(c => !restricted || (c.key !== "store" && c.key !== "settings")).map(c => ({ key: c.key, title: c.title, icon: c.icon, tint: c.tint,
                                  subtitle: "", kind: "category" }))
}

function sectionRows(categoryId) {
    return (sections[categoryId] ?? []).map(s => ({
        key: s.key, title: s.title, icon: s.icon, tint: s.tint, subtitle: "",
        kind: s.kind ?? "section" }))
}
