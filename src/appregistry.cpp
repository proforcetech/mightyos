#include "appregistry.h"

#include "artworkprovider.h"
#include "iconlocator.h"

#include "ids.h"
#include "jsonfile.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

Q_DECLARE_LOGGING_CATEGORY(lcAppScan)

namespace {

QString tintFor(const QString &category)
{
    if (category == QLatin1String("media"))
        return QStringLiteral("#4da3ff");
    if (category == QLatin1String("games"))
        return QStringLiteral("#7bd88f");
    return QStringLiteral("#a0aec0");
}

QString vectorIconFor(const QString &category)
{
    if (category == QLatin1String("media"))
        return QStringLiteral("media");
    if (category == QLatin1String("games"))
        return QStringLiteral("games");
    return QStringLiteral("apps");
}

} // namespace

AppRegistry::AppRegistry(QObject *parent)
    : AppRegistry(AppScanConfig::fromEnvironment(), true, parent, defaultCacheFile())
{
    m_config.icons = &IconLocator::shared();
}

AppRegistry::AppRegistry(const AppScanConfig &config, bool autoScan, QObject *parent,
                         const QString &cacheFile)
    : QObject(parent)
    , m_config(config)
    , m_cacheFile(cacheFile)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &AppRegistry::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_future, &QFutureWatcher<AppScanResult>::finished, this, [this] {
        applyResult(m_future.result(), true);
        emit scanningChanged();
        if (m_rescanPending) {
            m_rescanPending = false;
            startScan();
        }
    });
    // Synchronous: a few KB of JSON, so tiles and saved positions resolve
    // before QML builds the first screen.
    if (!m_cacheFile.isEmpty()) {
        if (const auto cached = readCache(m_cacheFile)) {
            AppScanResult result;
            result.apps = *cached;
            applyResult(result, false);
        }
    }
    if (autoScan) {
        // Queued, so the first scan starts after the constructor (and the
        // AppRegistry(parent) delegating constructor) finished setting up.
        QMetaObject::invokeMethod(this, &AppRegistry::rescan, Qt::QueuedConnection);
    }
}

AppRegistry::~AppRegistry()
{
    // Stop a running scan between two files instead of waiting for it.
    m_cancel->store(true);
    m_future.disconnect(this);
    m_future.waitForFinished();
}

void AppRegistry::rescan()
{
    m_debounce.stop();
    if (m_future.isRunning()) {
        m_rescanPending = true;
        return;
    }
    startScan();
}

void AppRegistry::startScan()
{
    const AppScanConfig config = m_config;
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    m_future.setFuture(QtConcurrent::run([config, cancel] { return AppScanner::scan(config, cancel.get()); }));
    emit scanningChanged();
}

TileItem AppRegistry::tileFor(const AppInfo &app)
{
    TileItem item;
    item.key = app.id;
    item.title = app.title;
    item.subtitle = app.subtitle;
    item.icon = vectorIconFor(app.category);
    item.artwork = ArtworkProvider::urlFor(app.artwork);
    item.kind = QStringLiteral("app");
    item.tint = tintFor(app.category);
    return item;
}

void AppRegistry::applyResult(const AppScanResult &result, bool fromScan)
{
    if (fromScan && m_cancel->load())
        return;   // partial result of a cancelled scan
    QHash<QString, AppInfo> byId;
    QList<TileItem> all;
    QList<TileItem> media;
    for (const AppInfo &app : result.apps) {
        byId.insert(app.id, app);
        const TileItem tile = tileFor(app);
        all.append(tile);
        if (app.category == QLatin1String("media"))
            media.append(tile);
    }
    const bool changed = byId != m_byId;
    m_byId = byId;
    m_apps.setItems(all);
    m_media.setItems(media);

    m_fromCache = !fromScan;
    if (!fromScan) {
        qCInfo(lcAppScan) << "apps:" << result.apps.size() << "from the startup cache";
        if (changed)
            emit appsChanged();
        return;
    }
    if (!m_cacheFile.isEmpty() && (changed || !QFileInfo::exists(m_cacheFile)))
        writeCache(m_cacheFile, result.apps);

    const QStringList oldWatch = m_watcher.directories() + m_watcher.files();
    if (oldWatch != result.watchPaths) {
        if (!oldWatch.isEmpty())
            m_watcher.removePaths(oldWatch);
        if (!result.watchPaths.isEmpty())
            m_watcher.addPaths(result.watchPaths);
    }

    qCInfo(lcAppScan).nospace() << "apps: " << result.apps.size() << " usable of "
                                << result.desktopFiles << " desktop files and " << result.manifests
                                << " manifests (" << result.skipped << " skipped)";
    if (lcAppScan().isDebugEnabled()) {
        for (const AppInfo &app : result.apps)
            qCDebug(lcAppScan) << "  " << app.id << app.category;
    }

    if (changed)
        emit appsChanged();
    if (!m_ready) {
        m_ready = true;
        emit readyChanged();
    }
    emit scanFinished();
}

QStringList AppRegistry::command(const QString &id) const
{
    const auto found = m_byId.constFind(id);
    return found == m_byId.cend() ? QStringList() : found->argv;
}

std::optional<TileItem> AppRegistry::resolve(const QString &id) const
{
    const auto found = m_byId.constFind(id);
    if (found == m_byId.cend())
        return std::nullopt;
    return tileFor(*found);
}

std::optional<AppInfo> AppRegistry::app(const QString &id) const
{
    const auto found = m_byId.constFind(id);
    if (found == m_byId.cend())
        return std::nullopt;
    return *found;
}

QStringList AppRegistry::watchedPaths() const
{
    return m_watcher.directories() + m_watcher.files();
}

QString AppRegistry::defaultCacheFile()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/mightyos/apps.json");
}

namespace {

bool validCachedId(const QString &id)
{
    if (id.startsWith(QLatin1String("desktop:")))
        return Ids::isValidDesktopFileId(id.mid(8) + QStringLiteral(".desktop"));
    if (id.startsWith(QLatin1String("flatpak:")))
        return Ids::isValidFlatpakId(id.mid(8));
    if (id.startsWith(QLatin1String("app:")))
        return Ids::isValidManifestId(id.mid(4));
    return false;
}

bool validCachedArtwork(const QString &artwork)
{
    if (artwork.isEmpty())
        return true;
    if (artwork.startsWith(QLatin1String("theme:")))
        return Ids::isValidIconName(artwork.mid(6));
    if (artwork.startsWith(QLatin1String("file:")))
        return Ids::isCleanAbsolutePath(artwork.mid(5));
    return false;
}

} // namespace

std::optional<QList<AppInfo>> AppRegistry::readCache(const QString &path)
{
    const auto root = JsonFile::read(path, kMaxCacheBytes);
    if (!root || root->value(QStringLiteral("version")).toInt(-1) != 1)
        return std::nullopt;
    QList<AppInfo> apps;
    for (const QJsonValue &value : root->value(QStringLiteral("apps")).toArray()) {
        if (apps.size() >= kMaxCachedApps)
            break;
        const QJsonObject o = value.toObject();
        AppInfo app;
        app.id = o.value(QStringLiteral("id")).toString();
        app.title = o.value(QStringLiteral("title")).toString().left(100);
        app.subtitle = o.value(QStringLiteral("subtitle")).toString().left(120);
        app.category = o.value(QStringLiteral("category")).toString();
        app.artwork = o.value(QStringLiteral("artwork")).toString();
        app.source = o.value(QStringLiteral("source")).toString();
        const QJsonArray argv = o.value(QStringLiteral("argv")).toArray();
        bool ok = validCachedId(app.id) && !app.title.isEmpty() && validCachedArtwork(app.artwork)
                  && (app.category == QLatin1String("media") || app.category == QLatin1String("games")
                      || app.category == QLatin1String("apps"))
                  && !argv.isEmpty() && argv.size() <= DesktopEntry::kMaxArgs;
        for (const QJsonValue &arg : argv) {
            ok = ok && arg.isString() && !arg.toString().contains(QChar(0));
            app.argv.append(arg.toString());
        }
        const QString program = app.argv.value(0);
        ok = ok && !program.isEmpty()
             && (!program.contains(QLatin1Char('/')) || Ids::isCleanAbsolutePath(program));
        if (ok)
            apps.append(app);
    }
    return apps;
}

bool AppRegistry::writeCache(const QString &path, const QList<AppInfo> &apps)
{
    QJsonArray array;
    for (const AppInfo &app : apps) {
        array.append(QJsonObject {
            { QStringLiteral("id"), app.id }, { QStringLiteral("title"), app.title },
            { QStringLiteral("subtitle"), app.subtitle }, { QStringLiteral("category"), app.category },
            { QStringLiteral("artwork"), app.artwork }, { QStringLiteral("source"), app.source },
            { QStringLiteral("argv"), QJsonArray::fromStringList(app.argv) },
        });
    }
    const bool ok = JsonFile::write(path, QJsonObject { { QStringLiteral("version"), 1 },
                                                        { QStringLiteral("apps"), array } });
    if (ok)
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    else
        qCWarning(lcAppScan) << "could not write the app cache";
    return ok;
}
