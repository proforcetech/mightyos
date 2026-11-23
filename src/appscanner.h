#pragma once

#include "desktopentry.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <atomic>
#include <optional>

class IconLocator;

// One launchable application, from a .desktop file or a MightyOS manifest.
struct AppInfo {
    QString id;          // "desktop:<id>", "flatpak:<appid>", "app:<id>"
    QString title;
    QString subtitle;
    QString category;    // "media" | "games" | "apps"
    QString artwork;     // "theme:<name>" | "file:<abs path>" | ""
    QStringList argv;    // never logged
    QString source;      // file it came from

    bool operator==(const AppInfo &) const = default;
};

// Where and how to scan. Everything the scan reads from the environment is
// captured here on the GUI thread, so the scan itself is a pure function
// that can run on a worker thread and in tests with temp dirs.
struct AppScanConfig {
    // ".../applications" dirs, highest precedence first.
    QStringList applicationDirs;
    // Subset of applicationDirs that are Flatpak export dirs.
    QStringList flatpakApplicationDirs;
    // Manifest dirs, highest precedence first.
    QStringList manifestDirs;
    // For OnlyShowIn= / NotShowIn=.
    QStringList currentDesktops;
    // Sanitised PATH used to check TryExec= and the program.
    QString path;
    QString locale;
    // Optional: only icons that resolve get artwork. Not owned.
    const IconLocator *icons = nullptr;

    // $XDG_DATA_HOME, ~/.local/share/flatpak/exports/share,
    // /var/lib/flatpak/exports/share, $XDG_DATA_DIRS (in that order, like
    // flatpak's profile script); manifests from $XDG_DATA_HOME/mightyos/apps
    // and `systemManifestDir`. Desktops from $XDG_CURRENT_DESKTOP, else
    // "MightyOS".
    static AppScanConfig fromEnvironment(
        const QString &systemManifestDir = QStringLiteral("/usr/share/mightyos/apps"));
};

struct AppScanResult {
    QList<AppInfo> apps;       // sorted by title
    QStringList watchPaths;    // directories to watch for changes
    int desktopFiles = 0;      // files looked at
    int manifests = 0;
    int skipped = 0;           // hidden, filtered or invalid
};

namespace AppScanner {

constexpr int kMaxFilesPerDir = 5000;
constexpr int kMaxDepth = 4;
constexpr int kMaxWatchPaths = 128;
constexpr qint64 kMaxManifestBytes = 64 * 1024;
constexpr int kMaxManifestsPerDir = 512;
constexpr qint64 kMaxIconFileBytes = 8 * 1024 * 1024;

// `cancel` (optional) is polled between files; a cancelled scan returns a
// partial result that the caller must discard.
AppScanResult scan(const AppScanConfig &config, const std::atomic_bool *cancel = nullptr);

// Exposed for tests. `why` gets a short reason (never argv) when nullopt.
std::optional<AppInfo> fromDesktopEntry(const DesktopEntry::Entry &entry, const QString &desktopFileId,
                                        const QString &path, bool inFlatpakDir,
                                        const AppScanConfig &config, QString *why = nullptr);
std::optional<AppInfo> fromManifest(const QByteArray &data, const QString &path,
                                    const AppScanConfig &config, QString *why = nullptr);

// "media" for AudioVideo/Audio/Video/TV, "games" for Game, else "apps".
QString categoryFor(const QStringList &categories);

// "theme:<name>" / "file:<path>" for a validated Icon value, or "".
QString artworkFor(const QString &icon, const IconLocator *icons);

} // namespace AppScanner
