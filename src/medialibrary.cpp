#include "medialibrary.h"

#include "appregistry.h"
#include "ids.h"
#include "jsonfile.h"
#include "launcher.h"
#include "library.h"
#include "profileservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QSet>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

Q_LOGGING_CATEGORY(lcMedia, "mightyos.media", QtWarningMsg)

namespace {

const QString kWebAppPrefix = QStringLiteral("webapp:");
const QString kGetAppPrefix = QStringLiteral("getapp:");

} // namespace

MediaLibrary::Config MediaLibrary::Config::fromEnvironment()
{
    Config c;
    c.webAppDirs = WebAppRegistry::defaultDirs();
    c.configHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    c.env = LaunchEnv::fromEnvironment();
    c.moonlightFiles = Moonlight::configFiles(c.env.home, c.configHome);
    return c;
}

QString MediaLibrary::Config::userWebAppDir() const
{
    return configHome.isEmpty() ? QString() : configHome + QStringLiteral("/mightyos/webapps.d");
}

const QList<MediaLibrary::KnownApp> &MediaLibrary::knownApps()
{
    // Flathub ids checked 2026-11 (flathub.org/apps/<id>).
    static const QList<KnownApp> apps {
        { QStringLiteral("Kodi"), { QStringLiteral("tv.kodi.Kodi") }, { QStringLiteral("kodi") }, true },
        { QStringLiteral("Jellyfin"),
          { QStringLiteral("org.jellyfin.JellyfinDesktop"), QStringLiteral("com.github.iwalton3.jellyfin-media-player") },
          { QStringLiteral("org.jellyfin.JellyfinDesktop"), QStringLiteral("com.github.iwalton3.jellyfin-media-player"),
            QStringLiteral("jellyfinmediaplayer") }, true },
        { QStringLiteral("Plex HTPC"), { QStringLiteral("tv.plex.PlexHTPC") }, { QStringLiteral("plex-htpc") }, true },
        { QStringLiteral("Plexamp"), { QStringLiteral("com.plexamp.Plexamp") }, { QStringLiteral("plexamp") }, false },
        { QStringLiteral("Moonlight"), { Moonlight::kFlatpakId }, { Moonlight::kFlatpakId, QStringLiteral("moonlight") }, true },
    };
    return apps;
}

MediaLibrary::MediaLibrary(const Config &config, AppRegistry *registry, Library *library, ProfileService *profiles,
                           Launcher *launcher, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_registryApps(registry)
    , m_library(library)
    , m_profiles(profiles)
    , m_launcher(launcher)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, [this] { reloadIfChanged(false); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_future, &QFutureWatcher<QList<MoonlightHost>>::finished, this, [this] {
        applyHosts(m_future.result());
        if (m_readAgain) {
            m_readAgain = false;
            startMoonlightRead();
        }
    });
    if (registry) {
        connect(registry, &AppRegistry::appsChanged, this, &MediaLibrary::rebuild);
        connect(registry, &AppRegistry::readyChanged, this, &MediaLibrary::rebuild);
    }
    if (library)
        connect(library, &Library::allowListChanged, this, &MediaLibrary::rebuild);
    if (launcher)
        connect(launcher, &Launcher::exited, this, [this] { m_debounce.start(); });
    m_webSignature = sourceSignature(false);
    m_moonSignature = sourceSignature(true);
    loadWebApps();
    rebuild();
    startMoonlightRead();
    updateWatcher();
}

MediaLibrary::~MediaLibrary()
{
    m_future.disconnect(this);
    m_future.waitForFinished();
}

MediaLibrary *MediaLibrary::create(QQmlEngine *qmlEngine, QJSEngine *)
{
    auto *registry = qmlEngine->singletonInstance<AppRegistry *>("MightyOS.Core", "AppRegistry");
    auto *library = qmlEngine->singletonInstance<Library *>("MightyOS.Core", "Library");
    auto *profiles = qmlEngine->singletonInstance<ProfileService *>("MightyOS.Core", "ProfileService");
    auto *launcher = qmlEngine->singletonInstance<Launcher *>("MightyOS.Core", "Launcher");
    return new MediaLibrary(Config::fromEnvironment(), registry, library, profiles, launcher);
}

// --- sources -------------------------------------------------------------------

void MediaLibrary::loadWebApps()
{
    m_registry.load(m_config.webAppDirs, m_config.userWebAppDir());
    m_browserFor.clear();
    for (const WebAppDef &def : m_registry.webApps())
        m_browserFor.insert(def.id, WebAppLaunch::choose(def, m_config.env).ok());
    WebAppDef drm;
    drm.drm = true;
    const BrowserChoice choice = WebAppLaunch::choose(drm, m_config.env);
    m_browserLabel = !choice.ok() ? QString()
                   : choice.flatpak ? tr("%1 (Flatpak)").arg(choice.browser->label) : choice.browser->label;
}

void MediaLibrary::startMoonlightRead()
{
    m_moonlightInstalled = Moonlight::installed(m_config.env);
    const QList<Moonlight::ConfigFile> files = m_config.moonlightFiles;
    const LaunchEnv env = m_config.env;
    if (!m_config.async) {
        applyHosts(Moonlight::readHosts(files, env));
        return;
    }
    if (m_future.isRunning()) {
        m_readAgain = true;
        return;
    }
    m_future.setFuture(QtConcurrent::run([files, env] { return Moonlight::readHosts(files, env); }));
}

void MediaLibrary::applyHosts(const QList<MoonlightHost> &hosts)
{
    m_moonlight = hosts;
    qCInfo(lcMedia) << "Moonlight:" << hosts.size() << "paired hosts";
    rebuild();
    if (!m_ready) {
        m_ready = true;
        emit readyChanged();
    }
}

void MediaLibrary::waitForMoonlight()
{
    while (m_future.isRunning() || m_readAgain) {
        m_future.waitForFinished();
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();
}

void MediaLibrary::reload()
{
    reloadIfChanged(true);
}

void MediaLibrary::unloadMoonlight()
{
    m_moonlight.clear();
    m_moonSignature.clear();
    rebuild();
    if (m_ready) {
        m_ready = false;
        emit readyChanged();
    }
}

QString MediaLibrary::sourceSignature(bool moonlightOnly) const
{
    // Only what we read counts: *.json in the user layer, the Moonlight.conf
    // files. Lock and temp files written next to them don't.
    QStringList parts;
    for (const Moonlight::ConfigFile &file : m_config.moonlightFiles) {
        const QFileInfo info(file.path);
        parts.append(file.path + QLatin1Char(':') + QString::number(info.size()) + QLatin1Char(':')
                     + QString::number(info.lastModified().toMSecsSinceEpoch()));
    }
    if (!moonlightOnly && !m_config.userWebAppDir().isEmpty()) {
        const QDir dir(m_config.userWebAppDir());
        const QFileInfoList files = dir.entryInfoList({ QStringLiteral("*.json") }, QDir::Files | QDir::System, QDir::Name);
        for (const QFileInfo &info : files)
            parts.append(info.fileName() + QLatin1Char(':') + QString::number(info.size()) + QLatin1Char(':')
                         + QString::number(info.lastModified().toMSecsSinceEpoch()));
    }
    return parts.join(QLatin1Char('|'));
}

void MediaLibrary::reloadWebApps()
{
    m_webSignature = sourceSignature(false);
    loadWebApps();
    rebuild();
    updateWatcher();
}

void MediaLibrary::reloadIfChanged(bool force)
{
    m_debounce.stop();
    const QString webSignature = sourceSignature(false);
    const QString moonSignature = sourceSignature(true);
    const bool webChanged = force || webSignature != m_webSignature;
    const bool moonChanged = force || moonSignature != m_moonSignature;
    m_webSignature = webSignature;
    m_moonSignature = moonSignature;
    if (webChanged)
        loadWebApps();
    // One rebuild per reload: a Moonlight read rebuilds when it lands.
    if (moonChanged)
        startMoonlightRead();
    else if (webChanged)
        rebuild();
    updateWatcher();
}

void MediaLibrary::updateWatcher()
{
    if (!m_config.watch)
        return;
    // Exactly the dirs we read, once they exist (a dir created later is
    // picked up by the next reload: after a launch, Options > Look again,
    // or adding a tile, which creates the user layer).
    QStringList paths;
    if (const QString dir = m_config.userWebAppDir(); !dir.isEmpty() && QFileInfo(dir).isDir())
        paths.append(dir);
    for (const Moonlight::ConfigFile &file : std::as_const(m_config.moonlightFiles)) {
        // Moonlight rewrites its file by renaming: watch the directory.
        const QString dir = QFileInfo(file.path).absolutePath();
        if (QFileInfo(dir).isDir())
            paths.append(dir);
    }
    paths.removeDuplicates();
    const QStringList old = m_watcher.directories();
    if (QSet<QString>(old.cbegin(), old.cend()) == QSet<QString>(paths.cbegin(), paths.cend()))
        return;
    if (!old.isEmpty())
        m_watcher.removePaths(old);
    if (!paths.isEmpty())
        m_watcher.addPaths(paths);
}

// --- models --------------------------------------------------------------------

bool MediaLibrary::restricted() const
{
    return m_library && m_library->isRestricted();
}

bool MediaLibrary::allowed(const QString &key) const
{
    return !m_library || m_library->itemAllowed(key);
}

bool MediaLibrary::hostAllowed(const MoonlightHost &host) const
{
    if (!restricted())
        return true;   // kids: a host shows if one of its apps is allowed
    for (const MoonlightApp &app : host.apps) {
        if (!app.hidden && allowed(Moonlight::appKey(host.id, app.name)))
            return true;
    }
    return false;
}

const MoonlightHost *MediaLibrary::findHost(const QString &hostId) const
{
    for (const MoonlightHost &host : m_moonlight) {
        if (host.id.compare(hostId, Qt::CaseInsensitive) == 0)
            return &host;
    }
    return nullptr;
}

TileItem MediaLibrary::webAppTile(const WebAppDef &def) const
{
    TileItem item;
    item.key = kWebAppPrefix + def.id;
    item.title = def.label;
    item.subtitle = !m_browserFor.value(def.id)
        ? (def.drm ? tr("Needs Google Chrome") : tr("Needs a web browser"))
        : !def.subtitle.isEmpty() ? def.subtitle : WebAppRegistry::hostOf(def.url);
    item.icon = def.icon;
    item.tint = def.tint;
    item.kind = QStringLiteral("webapp");
    return item;
}

TileItem MediaLibrary::hostTile(const MoonlightHost &host) const
{
    const int visible = static_cast<int>(streamItems(host).size());
    TileItem item;
    item.key = Moonlight::hostKey(host.id);
    item.title = host.name;
    item.subtitle = visible == 0 ? tr("Game streaming · open Moonlight to load its apps")
                                 : tr("Game streaming · %n apps", nullptr, visible);
    item.icon = QStringLiteral("stream");
    item.tint = QStringLiteral("#7c5cff");
    item.kind = QStringLiteral("streamhost");
    return item;
}

TileItem MediaLibrary::streamTile(const MoonlightHost &host, const MoonlightApp &app) const
{
    TileItem item;
    item.key = Moonlight::appKey(host.id, app.name);
    item.title = app.name;
    item.subtitle = host.name;
    item.icon = QStringLiteral("stream");
    item.tint = QStringLiteral("#7c5cff");
    item.kind = QStringLiteral("stream");
    return item;
}

QList<TileItem> MediaLibrary::streamItems(const MoonlightHost &host) const
{
    QList<TileItem> items;
    for (const MoonlightApp &app : host.apps) {
        if (app.hidden)
            continue;
        TileItem tile = streamTile(host, app);
        if (allowed(tile.key))
            items.append(tile);
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const TileItem &a, const TileItem &b) { return TileListModel::sortLess(a.title, b.title); });
    return items;
}

void MediaLibrary::rebuild()
{
    const bool kids = restricted();
    QList<TileItem> apps, getApps;
    QSet<QString> used;
    if (m_registryApps) {
        for (const KnownApp &known : knownApps()) {
            std::optional<TileItem> found;
            for (const QString &id : known.flatpaks) {
                if (!found && !used.contains(QStringLiteral("flatpak:") + id))
                    found = m_registryApps->resolve(QStringLiteral("flatpak:") + id);
            }
            for (const QString &id : known.desktops) {
                if (!found && !used.contains(QStringLiteral("desktop:") + id))
                    found = m_registryApps->resolve(QStringLiteral("desktop:") + id);
            }
            if (found) {
                used.insert(found->key);
                found->tint = QStringLiteral("#4da3ff");
                if (allowed(found->key))
                    apps.append(*found);
            } else if (known.offerInstall && !kids && m_registryApps->isReady()) {
                TileItem get;
                get.key = kGetAppPrefix + known.flatpaks.constFirst();
                get.title = known.label;
                get.subtitle = tr("Get it in the App Store");
                get.icon = QStringLiteral("store");
                get.tint = QStringLiteral("#5a6274");
                get.kind = QStringLiteral("getapp");
                getApps.append(get);
            }
        }
        for (const TileItem &tile : m_registryApps->media()->items()) {
            if (!used.contains(tile.key) && allowed(tile.key)) {
                used.insert(tile.key);
                apps.append(tile);
            }
        }
    }
    QList<TileItem> web, manage;
    TileItem add;
    add.key = QStringLiteral("addwebapp");
    add.title = tr("Add a web app");
    add.subtitle = tr("Any https:// site as a tile");
    add.icon = QStringLiteral("add");
    add.tint = QStringLiteral("#5a8dee");
    add.kind = QStringLiteral("addwebapp");
    manage.append(add);
    for (const WebAppDef &def : m_registry.webApps()) {
        TileItem tile = webAppTile(def);
        if (!def.hidden && allowed(tile.key))
            web.append(tile);
        TileItem m = tile;
        m.kind = QStringLiteral("managewebapp");
        m.subtitle = def.hidden ? tr("Hidden") : def.user ? tr("Added by you · %1").arg(WebAppRegistry::hostOf(def.url))
                                                         : WebAppRegistry::hostOf(def.url);
        m.available = !def.hidden;
        manage.append(m);
    }
    QList<TileItem> hosts;
    for (const MoonlightHost &host : std::as_const(m_moonlight)) {
        if (hostAllowed(host))
            hosts.append(hostTile(host));
    }
    m_apps.setItems(apps + getApps);
    m_webApps.setItems(web);
    m_hosts.setItems(hosts);
    m_home.setItems(apps + web + hosts);
    m_manage.setItems(manage);
    for (auto it = m_streams.begin(); it != m_streams.end(); ++it) {
        const MoonlightHost *host = findHost(it.key());
        it.value()->setItems(host && hostAllowed(*host) ? streamItems(*host) : QList<TileItem>());
    }
    emit changed();
}

TileListModel *MediaLibrary::streams(const QString &hostKey)
{
    QString hostId, app;
    if (!Moonlight::parseKey(hostKey, &hostId, &app) || !app.isEmpty())
        return nullptr;
    const MoonlightHost *host = findHost(hostId);
    if (!host || !hostAllowed(*host))
        return nullptr;
    TileListModel *model = m_streams.value(host->id);
    if (!model) {
        model = new TileListModel(this);
        QQmlEngine::setObjectOwnership(model, QQmlEngine::CppOwnership);
        m_streams.insert(host->id, model);
        model->setItems(streamItems(*host));
    }
    return model;
}

bool MediaLibrary::hasHost(const QString &hostKey) const
{
    QString hostId, app;
    if (!Moonlight::parseKey(hostKey, &hostId, &app) || !app.isEmpty())
        return false;
    const MoonlightHost *host = findHost(hostId);
    return host && hostAllowed(*host);
}

QString MediaLibrary::hostName(const QString &hostKey) const
{
    QString hostId, app;
    if (!Moonlight::parseKey(hostKey, &hostId, &app))
        return {};
    const MoonlightHost *host = findHost(hostId);
    return host ? host->name : QString();
}

std::optional<TileItem> MediaLibrary::resolve(const QString &key) const
{
    if (key.startsWith(kWebAppPrefix)) {
        const WebAppDef *def = m_registry.find(key.mid(kWebAppPrefix.size()));
        if (!def || def->hidden || !allowed(key))
            return std::nullopt;
        return webAppTile(*def);
    }
    QString hostId, appName;
    if (!Moonlight::parseKey(key, &hostId, &appName))
        return std::nullopt;
    const MoonlightHost *host = findHost(hostId);
    if (!host)
        return std::nullopt;
    if (appName.isEmpty())
        return hostAllowed(*host) ? std::optional<TileItem>(hostTile(*host)) : std::nullopt;
    for (const MoonlightApp &app : host->apps) {
        if (app.name == appName && !app.hidden && allowed(key))
            return streamTile(*host, app);
    }
    return std::nullopt;
}

bool MediaLibrary::isPending(const QString &key) const
{
    return !m_ready && key.startsWith(QLatin1String("moonlight:"));
}

QString MediaLibrary::profileId() const
{
    return m_profiles ? m_profiles->activeId() : QString();
}

// --- actions -------------------------------------------------------------------

QVariantMap MediaLibrary::launch(const QString &key)
{
    auto result = [](bool attempted, const QString &message, const QString &install = QString()) {
        return QVariantMap { { QStringLiteral("ok"), attempted && message.isEmpty() },
                             { QStringLiteral("attempted"), attempted },
                             { QStringLiteral("message"), message },
                             { QStringLiteral("install"), install } };
    };
    if (!m_launcher)
        return result(false, tr("Launching is not available."));
    if (!resolve(key))
        return result(false, restricted() ? tr("Ask a parent: this isn't available in a kids profile")
                                          : tr("This tile is not available any more."));
    LaunchPlan plan;
    if (key.startsWith(kWebAppPrefix)) {
        plan = WebAppLaunch::plan(*m_registry.find(key.mid(kWebAppPrefix.size())), profileId(), m_config.env,
                                  restricted());
    } else {
        QString hostId, app;
        Moonlight::parseKey(key, &hostId, &app);
        const MoonlightHost *host = findHost(hostId);
        if (app.isEmpty() || !host)
            return result(false, tr("Choose an app of %1 to stream.").arg(host ? host->name : hostId));
        plan = Moonlight::planStream(host->id, app, host->flatpak, m_config.env);
    }
    if (!plan.ok())
        return result(false, plan.error, plan.install);
    qCInfo(lcMedia) << "launching" << key << "with" << plan.emulatorId;   // never argv
    m_launcher->launch(key, plan.argv);
    return result(true, QString());
}

QVariantMap MediaLibrary::addWebApp(const QString &label, const QString &url)
{
    auto result = [](bool ok, const QString &message, const QString &key = QString()) {
        return QVariantMap { { QStringLiteral("ok"), ok }, { QStringLiteral("message"), message },
                             { QStringLiteral("key"), key } };
    };
    if (restricted())
        return result(false, tr("Ask a parent: kids profiles can't add tiles"));
    if (m_config.configHome.isEmpty() || !QFileInfo(m_config.configHome).isDir())
        return result(false, tr("No settings folder is available."));
    QStringList taken;
    for (const WebAppDef &def : m_registry.webApps())
        taken.append(def.id);
    QString id, error;
    const QByteArray json = WebAppRegistry::userDefinition(label, url, taken, &id, &error);
    if (json.isEmpty())
        return result(false, error);
    QString writeError;
    if (!JsonFile::writePrivate(m_config.configHome, { QStringLiteral("mightyos"), QStringLiteral("webapps.d") },
                                id + QStringLiteral(".json"), json, 0600, &writeError)) {
        qCWarning(lcMedia).noquote() << "could not save web app" << id << ":" << writeError;
        return result(false, tr("Could not save the tile."));
    }
    reloadWebApps();
    if (!m_registry.find(id))
        return result(false, tr("The tile was saved but could not be loaded."));
    return result(true, tr("Added %1").arg(label.trimmed()), kWebAppPrefix + id);
}

bool MediaLibrary::removeWebApp(const QString &key)
{
    if (restricted() || !key.startsWith(kWebAppPrefix))
        return false;
    const QString id = key.mid(kWebAppPrefix.size());
    const WebAppDef *def = m_registry.find(id);
    const QString dir = m_config.userWebAppDir();
    if (!def || def->hidden || dir.isEmpty() || !WebAppRegistry::isValidId(id))
        return false;
    bool ok = false;
    if (def->user && !def->shadowsLower) {
        // Our own file in the user layer (a symlink there would be removed, not followed).
        ok = QFile::remove(dir + QLatin1Char('/') + id + QStringLiteral(".json"));
    } else {
        // A built-in, or a user file replacing one: "remove" means gone, so
        // the user layer gets a stub (replacing the file) instead.
        ok = JsonFile::writePrivate(m_config.configHome, { QStringLiteral("mightyos"), QStringLiteral("webapps.d") },
                                    id + QStringLiteral(".json"), WebAppRegistry::disabledStub(id));
    }
    reloadWebApps();
    return ok;
}

bool MediaLibrary::showWebApp(const QString &key)
{
    if (restricted() || !key.startsWith(kWebAppPrefix))
        return false;
    const QString id = key.mid(kWebAppPrefix.size());
    const WebAppDef *def = m_registry.find(id);
    if (!def || !def->hidden || !WebAppRegistry::isValidId(id))
        return false;
    const bool ok = QFile::remove(m_config.userWebAppDir() + QLatin1Char('/') + id + QStringLiteral(".json"));
    reloadWebApps();
    return ok;
}

QVariantMap MediaLibrary::webAppInfo(const QString &key) const
{
    if (!key.startsWith(kWebAppPrefix))
        return {};
    const WebAppDef *def = m_registry.find(key.mid(kWebAppPrefix.size()));
    if (!def)
        return {};
    const BrowserChoice choice = WebAppLaunch::choose(*def, m_config.env);
    return {
        { QStringLiteral("key"), key }, { QStringLiteral("title"), def->label },
        { QStringLiteral("host"), WebAppRegistry::hostOf(def->url) }, { QStringLiteral("user"), def->user },
        { QStringLiteral("hidden"), def->hidden }, { QStringLiteral("drm"), def->drm },
        { QStringLiteral("browser"), choice.ok() ? choice.browser->label : QString() },
    };
}
