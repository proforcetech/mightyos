#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <atomic>
#include <optional>

// Installed Steam games (PLAN D12: Steam is a system).
//
// Roots (deduplicated by realpath; a root needs a steamapps/ dir):
//   ~/.steam/steam, ~/.steam/root, ~/.local/share/Steam (native) and
//   ~/.var/app/com.valvesoftware.Steam/.local/share/Steam (Flatpak).
// Library folders come from <root>/steamapps/libraryfolders.vdf (or
// config/libraryfolders.vdf); the root itself always counts. For the
// Flatpak root, a library path under $HOME that only exists inside the
// sandbox's persisted home is mapped to ~/.var/app/com.valvesoftware.Steam/.
// Games come from <library>/steamapps/appmanifest_<appid>.acf.
//
// Everything there is untrusted (KeyValues limits, fd-checked capped
// reads, clean absolute paths only, appids digits only and equal to the
// file name's, names sanitised). Tools and runtimes are skipped: Proton,
// Steam Linux Runtime, Steamworks Common Redistributables, anti-cheat
// runtimes, SteamVR (known appids plus name prefixes). Only fully
// installed apps (StateFlags bit 4) count.
//
// Artwork: Steam's own library cache first, <root>/appcache/librarycache/
// <appid>_library_600x900.jpg (old layout) or <appid>/library_600x900.jpg
// (also one hashed subdirectory down, newer clients), then header.jpg.
struct SteamGame {
    QString appid;      // digits only
    QString name;
    QString root;       // the Steam root it belongs to
    bool flatpak = false;
    QString artwork;    // absolute image path or ""
    qint64 lastPlayed = 0;   // seconds since the epoch, 0 = never

    bool operator==(const SteamGame &) const = default;
};

struct SteamRoot {
    QString path;       // realpath
    bool flatpak = false;
};

namespace SteamLibrary {

constexpr qint64 kMaxVdfBytes = 1024 * 1024;
constexpr qint64 kMaxManifestBytes = 256 * 1024;
constexpr int kMaxLibraries = 32;
constexpr int kMaxManifests = 10000;
static inline const QString kFlatpakId = QStringLiteral("com.valvesoftware.Steam");

bool isValidAppId(const QString &appid);
// Proton, runtimes, redistributables, ...
bool isTool(const QString &appid, const QString &name);

QList<SteamRoot> findRoots(const QString &home);
// Library dirs (realpaths) of one root, the root first.
QStringList libraryFolders(const SteamRoot &root, const QString &home);
// One appmanifest; nullopt if malformed, not installed or a tool.
// `expectedAppId` (from the file name) must match if non-empty.
std::optional<SteamGame> parseManifest(const QByteArray &data, const QString &expectedAppId = QString());
QString artworkFor(const QString &root, const QString &appid);

// All installed games of all roots, deduplicated by appid (first root
// wins), sorted by nothing in particular. `cancel` may be null.
QList<SteamGame> scan(const QString &home, const std::atomic_bool *cancel = nullptr);

// registry.vdf files that may say which app is running.
QStringList registryFiles(const QString &home);
// True if a registry.vdf says `appid` runs (RunningAppID or
// Apps/<appid>/Running).
bool registrySaysRunning(const QByteArray &registryVdf, const QString &appid);
// steam.pid files of the native and Flatpak clients.
QStringList steamPidFiles(const QString &home);
// True if one of `pidFiles` names a live process of ours under `procRoot`
// whose comm starts with "steam".
bool steamClientAlive(const QString &procRoot, const QStringList &pidFiles);
// True if a process of ours under `procRoot` (normally /proc) has
// SteamAppId=<appid> or SteamGameId=<appid> in its environment.
bool processRunning(const QString &procRoot, const QString &appid);

} // namespace SteamLibrary
