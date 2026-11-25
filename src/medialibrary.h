#pragma once

#include "emulatorlaunch.h"
#include "moonlight.h"
#include "tilelistmodel.h"
#include "webapps.h"

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <optional>

class AppRegistry;
class Launcher;
class Library;
class ProfileService;
class QJSEngine;
class QQmlEngine;

// Home > Media (PLAN D10, M5): installed media apps, streaming web apps and
// Moonlight hosts.
//
//   apps      well-known media apps first (Kodi, Jellyfin, Plex HTPC,
//             Plexamp, Moonlight; Flatpak or native .desktop, found through
//             AppRegistry), then every other app in the media category.
//             Standard profiles also get a "Get it in the App Store" tile
//             (kind "getapp") for each missing well-known app; nothing is
//             installed automatically.
//   webApps   WebAppRegistry tiles (kind "webapp", key webapp:<id>).
//   hosts     Moonlight hosts (kind "streamhost", key moonlight:<host>);
//             streams(host) lists that host's apps (kind "stream", key
//             moonlight:<host>/<app>).
//   home      apps (without "getapp") + webApps + hosts: the Home row.
//   manage    Settings > Media: "Add a web app" + every web app, hidden
//             built-ins included (kinds "addwebapp", "managewebapp").
//
// Kids profiles (PLAN D13): only what allowed.json's "apps" array lists
// (Library::itemAllowed), default deny, every Moonlight app by name (a host
// is shown when one of its apps is allowed); no "getapp" tiles, no adding
// or hiding web apps; web apps launch with network limits (WebAppDef).
//
// Reloads: web-app layers synchronously (a few small files), Moonlight's
// settings on a worker thread. A change in the user's webapps.d or
// Moonlight's settings dir (watched once they exist; debounced) or the end
// of a launch (Moonlight may have refreshed its app list) re-reads only the
// source whose files really changed (*.json / Moonlight.conf size+mtime;
// lock and temp files are ignored); reload() re-reads both. Each reload
// rebuilds the models once.
// Models keep their identity (TileListModel::setItems diffs).
class MediaLibrary : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Media)
    QML_SINGLETON

    Q_PROPERTY(TileListModel *home READ home CONSTANT FINAL)
    Q_PROPERTY(TileListModel *apps READ apps CONSTANT FINAL)
    Q_PROPERTY(TileListModel *webApps READ webApps CONSTANT FINAL)
    Q_PROPERTY(TileListModel *hosts READ hosts CONSTANT FINAL)
    Q_PROPERTY(TileListModel *manage READ manage CONSTANT FINAL)
    // Moonlight's settings were read at least once.
    Q_PROPERTY(bool ready READ isReady NOTIFY readyChanged FINAL)
    // "Google Chrome (Flatpak)" / "" (none): the browser DRM services use.
    Q_PROPERTY(QString browserLabel READ browserLabel NOTIFY changed FINAL)
    Q_PROPERTY(bool moonlightInstalled READ moonlightInstalled NOTIFY changed FINAL)

public:
    static constexpr int kDebounceMs = 500;

    struct Config {
        QStringList webAppDirs;          // layers, lowest priority first
        QString configHome;              // $XDG_CONFIG_HOME: the user layer is <it>/mightyos/webapps.d
        QList<Moonlight::ConfigFile> moonlightFiles;
        LaunchEnv env;
        bool watch = true;
        bool async = true;               // Moonlight settings read on a worker
        static Config fromEnvironment();
        QString userWebAppDir() const;
    };

    struct KnownApp {
        QString label;
        QStringList flatpaks;      // Flathub ids, preferred first
        QStringList desktops;      // native desktop file ids (without .desktop)
        bool offerInstall = true;  // a "getapp" tile when missing
    };
    static const QList<KnownApp> &knownApps();

    MediaLibrary(const Config &config, AppRegistry *registry, Library *library, ProfileService *profiles,
                 Launcher *launcher, QObject *parent = nullptr);
    ~MediaLibrary() override;
    static MediaLibrary *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    TileListModel *home() { return &m_home; }
    TileListModel *apps() { return &m_apps; }
    TileListModel *webApps() { return &m_webApps; }
    TileListModel *hosts() { return &m_hosts; }
    TileListModel *manage() { return &m_manage; }
    bool isReady() const { return m_ready; }
    QString browserLabel() const { return m_browserLabel; }
    bool moonlightInstalled() const { return m_moonlightInstalled; }

    // Apps of a Moonlight host (sorted by name); nullptr for unknown or
    // (kids) not allowed hosts.
    Q_INVOKABLE TileListModel *streams(const QString &hostKey);
    Q_INVOKABLE bool hasHost(const QString &hostKey) const;
    Q_INVOKABLE QString hostName(const QString &hostKey) const;

    // webapp: / moonlight: keys. { ok, attempted, message, install } like
    // Library::launch().
    Q_INVOKABLE QVariantMap launch(const QString &key);
    // Settings > Media. { ok, message, key }. Refused for kids profiles.
    Q_INVOKABLE QVariantMap addWebApp(const QString &label, const QString &url);
    // A user tile is deleted; a built-in is hidden (disabled stub in the
    // user layer). False for kids, unknown keys or a failed write.
    Q_INVOKABLE bool removeWebApp(const QString &key);
    // Un-hides a built-in (removes its stub).
    Q_INVOKABLE bool showWebApp(const QString &key);
    // { key, title, host, user, hidden, drm, browser } or empty.
    Q_INVOKABLE QVariantMap webAppInfo(const QString &key) const;
    Q_INVOKABLE void reload();
    // Back to "Moonlight's settings not read yet" (ready false, no hosts)
    // until the next reload(). For tests of the startup path; harmless
    // otherwise (Moonlight keys are pending, not unavailable, meanwhile).
    Q_INVOKABLE void unloadMoonlight();

    std::optional<TileItem> resolve(const QString &key) const;
    // Moonlight keys before the first read of its settings.
    bool isPending(const QString &key) const;
    const WebAppRegistry &webAppRegistry() const { return m_registry; }
    // Tests: blocks until a running Moonlight read is applied.
    void waitForMoonlight();

signals:
    void changed();
    void readyChanged();

private:
    void loadWebApps();
    void reloadIfChanged(bool force);
    void reloadWebApps();   // after our own edits: synchronous
    QString sourceSignature(bool moonlightOnly) const;
    void startMoonlightRead();
    void applyHosts(const QList<MoonlightHost> &hosts);
    void rebuild();
    void updateWatcher();
    bool restricted() const;
    bool allowed(const QString &key) const;
    bool hostAllowed(const MoonlightHost &host) const;
    const MoonlightHost *findHost(const QString &hostId) const;
    TileItem webAppTile(const WebAppDef &def) const;
    TileItem hostTile(const MoonlightHost &host) const;
    TileItem streamTile(const MoonlightHost &host, const MoonlightApp &app) const;
    QList<TileItem> streamItems(const MoonlightHost &host) const;
    QString profileId() const;

    Config m_config;
    QPointer<AppRegistry> m_registryApps;
    QPointer<Library> m_library;
    QPointer<ProfileService> m_profiles;
    QPointer<Launcher> m_launcher;
    WebAppRegistry m_registry;
    QHash<QString, bool> m_browserFor;   // web app id -> a browser is usable
    QList<MoonlightHost> m_moonlight;
    TileListModel m_home, m_apps, m_webApps, m_hosts, m_manage;
    QHash<QString, TileListModel *> m_streams;   // host id -> model
    QString m_browserLabel;
    bool m_moonlightInstalled = false;
    bool m_ready = false;
    bool m_readAgain = false;
    QString m_webSignature, m_moonSignature;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QFutureWatcher<QList<MoonlightHost>> m_future;
};
