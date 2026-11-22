.pragma library

// Mock data for M1. Shapes mirror the real models that replace it:
//   categories  fixed Home categories (stay in QML)
//   systems     system definitions (M4, systems.d/*.json). Steam is a system
//               like an emulator (PLAN D12): Games -> [Steam] -> its games.
//   games       LibraryModel rows (M4). Ids follow PLAN section 2:
//               rom:<system>:<sha1(relpath)>, steam:<appid>.
//   apps        AppRegistry rows (M2), flatpak:<appid>.
//
// Model rows are flat (scalars only) so they can go into a ListModel and,
// later, map 1:1 to C++ model roles: itemId, label, icon, tint, subtitle,
// kind, sortTitle. Anything nested (where a category opens) is looked up by
// itemId with the functions below.

const categories = [
    { itemId: "favorites", label: qsTr("Favorites"), icon: "favorites", tint: "#ffc94d",
      blurb: qsTr("Your pinned games and apps"),
      open: { screen: "games", params: { collection: "favorites" } } },
    { itemId: "emulators", label: qsTr("Emulators"), icon: "emulators", tint: "#ff6b8b",
      blurb: qsTr("Retro and modern consoles"),
      open: { screen: "systems", params: { category: "emulators" } } },
    { itemId: "games", label: qsTr("Games"), icon: "games", tint: "#7bd88f",
      blurb: qsTr("PC game libraries"),
      open: { screen: "systems", params: { category: "games" } } },
    { itemId: "media", label: qsTr("Media"), icon: "media", tint: "#4da3ff",
      blurb: qsTr("Movies, music and streaming"),
      open: { screen: "category", params: { category: "media" } } },
    { itemId: "store", label: qsTr("App Store"), icon: "store", tint: "#b48cff",
      blurb: qsTr("Install apps from Flathub"),
      open: { screen: "category", params: { category: "store" } } },
    { itemId: "settings", label: qsTr("Settings"), icon: "settings", tint: "#a0aec0",
      blurb: qsTr("Display, audio, network and controllers"),
      open: { screen: "category", params: { category: "settings" } } }
]

const systems = [
    { itemId: "nes",      label: "NES",      fullName: "Nintendo Entertainment System", category: "emulators", icon: "cartridge", tint: "#e05a5a" },
    { itemId: "snes",     label: "SNES",     fullName: "Super Nintendo",               category: "emulators", icon: "cartridge", tint: "#9b8cff" },
    { itemId: "n64",      label: "N64",      fullName: "Nintendo 64",                  category: "emulators", icon: "cartridge", tint: "#4fc06a" },
    { itemId: "ps1",      label: "PS1",      fullName: "PlayStation",                  category: "emulators", icon: "disc",      tint: "#b0b8c8" },
    { itemId: "ps2",      label: "PS2",      fullName: "PlayStation 2",                category: "emulators", icon: "disc",      tint: "#5a7dff" },
    { itemId: "psp",      label: "PSP",      fullName: "PlayStation Portable",         category: "emulators", icon: "handheld",  tint: "#8fa3bf" },
    { itemId: "gamecube", label: "GameCube", fullName: "Nintendo GameCube",            category: "emulators", icon: "disc",      tint: "#8a6bd1" },
    { itemId: "genesis",  label: "Genesis",  fullName: "Sega Genesis / Mega Drive",    category: "emulators", icon: "cartridge", tint: "#3d8bff" },
    { itemId: "arcade",   label: "Arcade",   fullName: "Arcade (MAME / FBNeo)",        category: "emulators", icon: "arcade",    tint: "#ffb347" },
    { itemId: "steam",    label: "Steam",    fullName: "Steam library",                category: "games",     icon: "steam",     tint: "#66c0f4" }
]

// Fictional titles (no trademarks); sorted per system below.
const titlePool = [
    "Aero Blaster Zero", "Alpine Rally", "Astro Miner", "Battle Kart Deluxe", "Blade of Dawn",
    "Bubble Harbor", "Castle Climber", "Chrono Pilot", "Cosmic Courier", "Crystal Caverns",
    "Dragon Valley", "Dune Runner", "Echo Knight", "Fable Forest", "Galaxy Golf",
    "Gear Hunters", "Haunted Hallways", "Hyper Striker", "Iron Orbit", "Jungle Jam",
    "Kingdom of Keys", "Lava Leap", "Lunar Lancer", "Metro Mayhem", "Neon Ninja",
    "Ocean Odyssey", "Pixel Paladin", "Puzzle Pagoda", "Quantum Quest", "Retro Rumble",
    "Robo Rescue", "Sky Fortress", "Star Sprinter", "Super Snowball", "Thunder Track",
    "Tiny Tactics", "Ultra Uprising", "Volcano Vault", "Wild West Wheels", "Zephyr Zone"
]

const apps = [
    { itemId: "flatpak:tv.kodi.Kodi",                label: "Kodi",      icon: "media",   tint: "#17b2e7", subtitle: qsTr("Local media") },
    { itemId: "flatpak:com.github.iwalton3.jellyfin-media-player", label: "Jellyfin", icon: "media", tint: "#aa5cc3", subtitle: qsTr("Media server") },
    { itemId: "flatpak:tv.plex.PlexDesktop",         label: "Plex",      icon: "media",   tint: "#e5a00d", subtitle: qsTr("Media server") },
    { itemId: "flatpak:com.moonlight_stream.Moonlight", label: "Moonlight", icon: "moon", tint: "#7a8cff", subtitle: qsTr("Game streaming") },
    { itemId: "webapp:youtube",                      label: "YouTube",   icon: "display", tint: "#ff4e45", subtitle: qsTr("Web app") }
]

const sections = {
    store: [
        { itemId: "store:featured",  label: qsTr("Featured"),  icon: "favorites", tint: "#b48cff" },
        { itemId: "store:games",     label: qsTr("Games"),     icon: "games",     tint: "#7bd88f" },
        { itemId: "store:emulators", label: qsTr("Emulators"), icon: "emulators", tint: "#ff6b8b" },
        { itemId: "store:media",     label: qsTr("Media"),     icon: "media",     tint: "#4da3ff" },
        { itemId: "store:updates",   label: qsTr("Updates"),   icon: "store",     tint: "#a0aec0" }
    ],
    settings: [
        { itemId: "settings:display",     label: qsTr("Display"),     icon: "display",   tint: "#a0aec0" },
        { itemId: "settings:audio",       label: qsTr("Audio"),       icon: "audio",     tint: "#a0aec0" },
        { itemId: "settings:network",     label: qsTr("Network"),     icon: "network",   tint: "#a0aec0" },
        { itemId: "settings:controllers", label: qsTr("Controllers"), icon: "emulators", tint: "#a0aec0" },
        { itemId: "settings:storage",     label: qsTr("Storage"),     icon: "folder",    tint: "#a0aec0" },
        { itemId: "settings:power",       label: qsTr("Power"),       icon: "power",     tint: "#ff6b6b" }
    ]
}

// Milestone that brings each placeholder section to life.
const sectionMilestone = { store: "M7", settings: "M8" }

// 32-bit FNV-1a as hex. Stand-in for sha1(relpath) in mock ids only.
function _fakeHash(text) {
    let h = 0x811c9dc5
    for (let i = 0; i < text.length; ++i) {
        h ^= text.charCodeAt(i)
        h = Math.imul(h, 0x01000193) >>> 0
    }
    return ("00000000" + h.toString(16)).slice(-8)
}

function category(id) {
    return categories.find(c => c.itemId === id) ?? null
}

function system(id) {
    return systems.find(s => s.itemId === id) ?? null
}

// The system `delta` steps away from `id` within the same category
// (SNES +1 -> N64), or null at either end / for unknown ids.
function adjacentSystem(id, delta) {
    const sys = system(id)
    if (!sys)
        return null
    const peers = systems.filter(s => s.category === sys.category)
    const next = peers[peers.findIndex(s => s.itemId === id) + delta]
    return next ? next.itemId : null
}

// --- Flat model rows ---------------------------------------------------------

function categoryRows() {
    return categories.map(c => ({ itemId: c.itemId, label: c.label, icon: c.icon, tint: c.tint,
                                  subtitle: "", kind: "category", sortTitle: c.label }))
}

function systemRows(categoryId) {
    return systems.filter(s => s.category === categoryId)
                  .map(s => ({ itemId: s.itemId, label: s.label, icon: s.icon, tint: s.tint,
                               subtitle: s.fullName, kind: "system", sortTitle: s.label }))
}

function gameRows(systemId) {
    const sys = system(systemId)
    if (!sys)
        return []
    const isSteam = systemId === "steam"
    const titles = isSteam ? titlePool.filter((_, i) => i % 5 === 1 || i % 5 === 3) : titlePool
    return titles.map((title, i) => ({
        itemId: isSteam ? "steam:" + (200000 + i * 10)
                        : "rom:" + systemId + ":" + _fakeHash(systemId + "/" + title),
        label: title,
        icon: sys.icon,
        tint: sys.tint,
        subtitle: String(1986 + (title.length * 7 + i) % 22),
        kind: "game",
        sortTitle: title
    }))
}

function favoriteRows() {
    const picks = [["snes", 3], ["n64", 7], ["steam", 2], ["ps1", 10], ["genesis", 0], ["arcade", 21]]
    const rows = []
    for (const [systemId, index] of picks) {
        const game = gameRows(systemId)[index]
        if (game)
            rows.push(Object.assign({}, game, { subtitle: system(systemId).label }))
    }
    return rows
}

function appRows() {
    return apps.map(a => ({ itemId: a.itemId, label: a.label, icon: a.icon, tint: a.tint,
                            subtitle: a.subtitle, kind: "app", sortTitle: a.label }))
}

function sectionRows(categoryId) {
    return (sections[categoryId] ?? []).map(s => ({
        itemId: s.itemId, label: s.label, icon: s.icon, tint: s.tint, subtitle: "",
        kind: s.itemId === "settings:power" ? "power" : "section", sortTitle: s.label }))
}

// Content of a Home category's row, and of the category's own screen.
function contentRows(categoryId) {
    switch (categoryId) {
    case "favorites": return favoriteRows()
    case "emulators":
    case "games":     return systemRows(categoryId)
    case "media":     return appRows()
    case "store":
    case "settings":  return sectionRows(categoryId)
    }
    return []
}

// Rows for the GameGrid screen; null if `params` no longer resolve.
function gridRows(params) {
    if (params && params.collection === "favorites")
        return favoriteRows()
    if (params && system(params.system))
        return gameRows(params.system)
    return null
}

function gridTitle(params) {
    if (params && params.collection === "favorites")
        return qsTr("Favorites")
    const sys = params ? system(params.system) : null
    return sys ? sys.label : ""
}
