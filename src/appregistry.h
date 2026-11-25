#pragma once

#include "appscanner.h"
#include "tilelistmodel.h"

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <atomic>
#include <memory>
#include <optional>

// Installed applications (PLAN section 2): .desktop files from the XDG and
// Flatpak export application dirs plus MightyOS JSON manifests. See
// AppScanner for the parsing and precedence rules.
//
// Scans run on a worker thread (QtConcurrent); results are applied on the
// GUI thread. A QFileSystemWatcher on the scanned dirs (and the nearest
// existing parent of missing ones) triggers a debounced re-scan, so
// installing or removing an app updates the tiles without a restart.
//
// Models: `apps` (everything, sorted by title) and `media` (category
// media). Both keep their identity across re-scans; rows are only reset
// when the set of apps changed.
//
// Duplicates (M6): an app installed both natively and as a Flatpak (Kodi:
// desktop:kodi and flatpak:tv.kodi.Kodi) gets one tile, the Flatpak's.
// A native desktop:<id> is shadowed when <id> is the Flatpak's whole app
// id (case-insensitive: org.mozilla.firefox.desktop installed natively) or
// its Name is exactly the Flatpak's (Kodi / Kodi). Never by a part of an
// id (shadowedApps()). Shadowed apps stay known:
// favorites, history and command() still resolve them.
//
// Startup cache: the last scan result is kept in
// $XDG_CACHE_HOME/mightyos/apps.json and loaded synchronously when the
// registry is created, so app tiles, favorites and saved focus positions
// resolve on the first frame. The real scan then runs in the background
// and replaces it; `ready` only turns true after a real scan. The cache is
// validated like any other untrusted input (ids, argv shape, sizes).
//
// Logging: ids and counts only, never argv (PLAN section 4).
class AppRegistry : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(TileListModel *apps READ apps CONSTANT FINAL)
    Q_PROPERTY(TileListModel *media READ media CONSTANT FINAL)
    Q_PROPERTY(int count READ count NOTIFY appsChanged FINAL)
    Q_PROPERTY(bool scanning READ isScanning NOTIFY scanningChanged FINAL)
    // True after the first real scan (not just the startup cache).
    Q_PROPERTY(bool ready READ isReady NOTIFY readyChanged FINAL)

public:
    static constexpr int kDebounceMs = 500;
    static constexpr qint64 kMaxCacheBytes = 2 * 1024 * 1024;
    static constexpr int kMaxCachedApps = 5000;

    // From the environment; starts the first scan immediately.
    explicit AppRegistry(QObject *parent = nullptr);
    // Tests: explicit dirs; `autoScan` false waits for rescan(). With a
    // non-empty `cacheFile` the cache is loaded first and written after
    // every scan.
    AppRegistry(const AppScanConfig &config, bool autoScan, QObject *parent = nullptr,
                const QString &cacheFile = QString());
    ~AppRegistry() override;

    TileListModel *apps() { return &m_apps; }
    TileListModel *media() { return &m_media; }
    int count() const { return static_cast<int>(m_byId.size()); }
    bool isScanning() const { return m_future.isRunning(); }
    bool isReady() const { return m_ready; }

    // Starts a scan now (or right after the running one).
    Q_INVOKABLE void rescan();
    Q_INVOKABLE bool contains(const QString &id) const { return m_byId.contains(id); }
    // The argv to launch `id` with, or empty if unknown.
    Q_INVOKABLE QStringList command(const QString &id) const;

    std::optional<TileItem> resolve(const QString &id) const;
    std::optional<AppInfo> app(const QString &id) const;
    QStringList watchedPaths() const;
    const AppScanConfig &config() const { return m_config; }

    static TileItem tileFor(const AppInfo &app);
    // The native ids that a Flatpak in `apps` shadows (see above).
    static QSet<QString> shadowedApps(const QList<AppInfo> &apps);
    Q_INVOKABLE bool isShadowed(const QString &id) const { return m_shadowed.contains(id); }
    static QString defaultCacheFile();
    QString cacheFile() const { return m_cacheFile; }
    // True while the content came from the startup cache only.
    bool isFromCache() const { return m_fromCache; }
    // Validated cache content; nullopt if missing or unusable.
    static std::optional<QList<AppInfo>> readCache(const QString &path);
    static bool writeCache(const QString &path, const QList<AppInfo> &apps);

signals:
    void appsChanged();
    void scanningChanged();
    void readyChanged();
    void scanFinished();

private:
    void startScan();
    void applyResult(const AppScanResult &result, bool fromScan);

    AppScanConfig m_config;
    TileListModel m_apps;
    TileListModel m_media;
    QHash<QString, AppInfo> m_byId;
    QSet<QString> m_shadowed;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QFutureWatcher<AppScanResult> m_future;
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
    QString m_cacheFile;
    bool m_rescanPending = false;
    bool m_ready = false;
    bool m_fromCache = false;
};
