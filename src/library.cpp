#include "library.h"

#include "artworkprovider.h"
#include "artworkscraper.h"
#include "ids.h"
#include "jsonfile.h"
#include "launcher.h"
#include "librarycache.h"
#include "profileservice.h"
#include "steamtracker.h"
#include "textsanitize.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPromise>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lcLibrary, "mightyos.library", QtWarningMsg)

struct Library::ScanOutput {
    std::optional<SystemRegistry> registry;   // set when the scan reloaded systems.d
    bool full = false;                        // a ROM scan (not Steam only)
    QSet<QString> romSystems;                 // systems whose ROM lists are in `games`
    bool steamScanned = false;
    QHash<QString, QList<Library::Game>> games;   // sorted
    QStringList watchPaths;
    RomScanStats stats;
    int steamGames = 0;
    int written = 0;
    int deleted = 0;
    qint64 elapsedMs = 0;
};

namespace {

bool isValidColour(const QString &colour)
{
    static const QRegularExpression re(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return re.match(colour).hasMatch();
}

const QString kShowAllPrefix = QStringLiteral("showall:");

struct ScanInput {
    QStringList systemDirs;
    SystemRegistry registry;   // used when !reloadRegistry
    bool reloadRegistry = false;
    bool scanRoms = false;
    bool scanSteam = false;
    QString home;
    QString cachePath;
    QHash<QString, RomRecord> cacheBase;   // the startup cache load, reused once
    bool haveCacheBase = false;
};

// Nearest existing directory at or above `path` (but not "/").
QString existingAncestor(QString path)
{
    while (path.size() > 1) {
        if (QFileInfo(path).isDir())
            return path;
        path = QFileInfo(path).absolutePath();
    }
    return {};
}

// The worker job: (registry), Steam and every ROM system, the cache diff
// and the watch list. Results arrive in the GUI thread via the watcher.
void runScan(QPromise<std::shared_ptr<Library::ScanOutput>> &promise, const ScanInput &input,
             std::shared_ptr<std::atomic_bool> cancel)
{
    QElapsedTimer timer;
    timer.start();
    auto out = std::make_shared<Library::ScanOutput>();
    out->full = input.scanRoms;
    SystemRegistry registry = input.registry;
    if (input.reloadRegistry) {
        registry.load(input.systemDirs, input.home);
        out->registry = registry;
    }

    QString steamSystem;
    for (const SystemDef &s : registry.systems()) {
        if (s.kind == SystemDef::Kind::Steam && steamSystem.isEmpty())
            steamSystem = s.id;
    }
    if (input.scanSteam && !steamSystem.isEmpty()) {
        QList<Library::Game> list;
        for (const SteamGame &sg : SteamLibrary::scan(input.home, cancel.get())) {
            Library::Game g;
            g.id = QStringLiteral("steam:") + sg.appid;
            g.system = steamSystem;
            g.title = sg.name;
            g.appid = sg.appid;
            g.flatpakSteam = sg.flatpak;
            g.localArt = sg.artwork;
            list.append(g);
        }
        out->steamGames = static_cast<int>(list.size());
        Library::sortGames(list);
        out->games.insert(steamSystem, list);
        out->steamScanned = true;
    }
    if (cancel->load())
        return;

    QStringList watch;
    for (const SteamRoot &root : SteamLibrary::findRoots(input.home)) {
        for (const QString &library : SteamLibrary::libraryFolders(root, input.home))
            watch.append(library + QStringLiteral("/steamapps"));
    }

    if (input.scanRoms) {
        LibraryCache cache(input.cachePath);
        const bool cacheOpen = !input.cachePath.isEmpty() && cache.open();
        const QHash<QString, RomRecord> previous = input.haveCacheBase ? input.cacheBase
                                                 : cacheOpen ? cache.load() : QHash<QString, RomRecord>();
        QList<RomRecord> all;
        QSet<QString> systems;
        for (const SystemDef &s : registry.systems()) {
            if (s.kind != SystemDef::Kind::Roms)
                continue;
            if (cancel->load())
                return;
            systems.insert(s.id);
            RomScanStats stats;
            QList<RomRecord> records = RomScanner::scan({ s.id, s.extensions, s.romDirs }, previous, &stats, cancel.get());
            if (cancel->load())
                return;
            // A missing ROM dir (unplugged drive) is not "no games": keep
            // its cached rows, marked offline, and keep them in the cache.
            QSet<QString> offlineRoots(stats.missingRoots.cbegin(), stats.missingRoots.cend());
            QList<Library::Game> list;
            list.reserve(records.size());
            for (const RomRecord &r : std::as_const(records))
                list.append(Library::gameFromRom(r, false));
            if (!offlineRoots.isEmpty()) {
                for (const RomRecord &r : previous) {
                    if (r.system == s.id && offlineRoots.contains(r.root)) {
                        records.append(r);
                        list.append(Library::gameFromRom(r, true));
                    }
                }
            }
            for (const QString &dir : s.romDirs) {
                const QString parent = QFileInfo(dir).absolutePath();
                if (QFileInfo(dir).isDir())
                    watch.append(dir);
                if (const QString ancestor = existingAncestor(parent); !ancestor.isEmpty())
                    watch.append(ancestor);   // a new system folder or a re-plugged drive
            }
            out->stats.dirs += stats.dirs;
            out->stats.files += stats.files;
            out->stats.matched += stats.matched;
            out->stats.reused += stats.reused;
            out->stats.symlinksRejected += stats.symlinksRejected;
            out->stats.specialSkipped += stats.specialSkipped;
            out->stats.truncated = out->stats.truncated || stats.truncated;
            out->stats.missingRoots += stats.missingRoots;
            Library::sortGames(list);
            out->games.insert(s.id, list);
            out->romSystems.insert(s.id);
            all.append(records);
        }
        if (cacheOpen && !cache.sync(all, systems, previous, &out->written, &out->deleted, cancel.get())
            && !cancel->load())
            qCWarning(lcLibrary) << "could not update the library cache";
    } else {
        // Steam-only: keep watching the ROM dirs as before.
        for (const SystemDef &s : registry.systems()) {
            for (const QString &dir : s.romDirs) {
                if (QFileInfo(dir).isDir())
                    watch.append(dir);
                if (const QString ancestor = existingAncestor(QFileInfo(dir).absolutePath()); !ancestor.isEmpty())
                    watch.append(ancestor);
            }
        }
    }
    if (cancel->load())
        return;
    watch.removeDuplicates();
    out->watchPaths = watch;
    out->elapsedMs = timer.elapsed();
    promise.addResult(out);
}

} // namespace

// ---------------------------------------------------------------------------

void Library::sortGames(QList<Game> &games)
{
    for (Game &g : games)
        g.sortKey = TileListModel::sortKey(g.title);
    std::stable_sort(games.begin(), games.end(), [](const Game &a, const Game &b) {
        const int c = a.sortKey.compare(b.sortKey);
        if (c != 0)
            return c < 0;
        const int t = a.title.compare(b.title);
        return t != 0 ? t < 0 : a.id < b.id;
    });
}

Library::Game Library::gameFromRom(const RomRecord &r, bool offline)
{
    Game g;
    g.id = r.id;
    g.system = r.system;
    g.title = r.title;
    g.root = r.root;
    g.relPath = r.relPath;
    g.region = r.region;
    g.tags = r.tags;
    g.discs = r.discs;
    g.offline = offline;
    QStringList sub;
    if (offline)
        sub.append(QCoreApplication::translate("Library", "Not connected"));
    if (!r.region.isEmpty())
        sub.append(r.region);
    if (r.discs > 1)
        sub.append(QCoreApplication::translate("Library", "%n discs", nullptr, r.discs));
    g.subtitle = sub.join(QStringLiteral("  ·  "));
    return g;
}

Library::Config Library::configFromEnvironment()
{
    Config config;
    config.mock = qEnvironmentVariable("MIGHTYOS_MOCK_LIBRARY") == QLatin1String("1");
    config.systemDirs = SystemRegistry::defaultDirs();
    config.home = QDir::homePath();
    config.cachePath = LibraryCache::defaultPath();
    config.env = LaunchEnv::fromEnvironment();
    return config;
}

Library::Library(QObject *parent)
    : Library([] {
          Config c;
          c.mock = true;
          c.autoScan = false;
          c.watch = false;
          c.changedDelayMs = 0;
          return c;
      }(), nullptr, nullptr, nullptr, parent)
{
}

Library::Library(const Config &config, ProfileService *profiles, Launcher *launcher, ArtworkScraper *scraper,
                 QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_profiles(profiles)
    , m_launcher(launcher)
    , m_scraper(scraper)
{
    m_changedTimer.setSingleShot(true);
    m_changedTimer.setInterval(qMax(0, config.changedDelayMs));
    connect(&m_changedTimer, &QTimer::timeout, this, &Library::changed);
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(kRescanDelayMs);
    connect(&m_rescanTimer, &QTimer::timeout, this, [this] {
        const int kinds = m_timerKinds;
        m_timerKinds = 0;
        if (m_launcher && m_launcher->isRunning()) {
            // Never scan disks under a running game; catch up afterwards.
            m_timerKinds = kinds;
            m_afterLaunch = true;
            return;
        }
        startScan(kinds, (kinds & FullScan) != 0);
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
        scheduleRescan(path.endsWith(QLatin1String("/steamapps")) ? SteamScan : FullScan);
    });

    connect(&m_future, &QFutureWatcher<std::shared_ptr<ScanOutput>>::resultReadyAt, this, [this](int index) {
        if (m_cancel->load())
            return;
        const std::shared_ptr<ScanOutput> out = m_future.resultAt(index);
        if (out)
            applyScan(*out);
    });
    connect(&m_future, &QFutureWatcher<std::shared_ptr<ScanOutput>>::finished, this, [this] {
        emit scanningChanged();
        if (m_pendingKinds != 0) {
            const int kinds = m_pendingKinds;
            const bool registry = m_pendingRegistry;
            m_pendingKinds = 0;
            m_pendingRegistry = false;
            startScan(kinds, registry);
        }
    });

    if (launcher) {
        connect(launcher, &Launcher::runningChanged, this, [this] {
            if (m_launcher->isRunning() || !m_afterLaunch)
                return;
            m_afterLaunch = false;
            m_rescanTimer.start();   // the postponed watcher re-scan
        });
        connect(launcher, &Launcher::exited, this, [this](const QString &id) {
            // Kids profiles: the Steam client is unrestricted once it runs
            // (store, browser, other games), so close it after the game.
            if (id.isEmpty() || id != m_kidsSteamKey)
                return;
            m_kidsSteamKey.clear();
            const QStringList argv = EmulatorLaunch::steamShutdownArgv(m_kidsSteamFlatpak, m_config.env);
            if (!argv.isEmpty() && m_launcher->runAuxiliary(argv))
                qCInfo(lcLibrary) << "kids profile: asked the Steam client to shut down";
        });
    }
    if (profiles)
        connect(profiles, &ProfileService::activeChanged, this, [this] {
            const bool wasRestricted = m_allow.restricted;
            loadProfileFiles();
            refreshModels();
            if (wasRestricted != m_allow.restricted)
                emit restrictedChanged();
            notifyChanged();
        });
    if (scraper) {
        connect(scraper, &ArtworkScraper::artworkReady, this, &Library::artworkArrived);
        connect(scraper, &ArtworkScraper::keyChanged, this, [this] {
            for (auto it = m_models.cbegin(); it != m_models.cend(); ++it) {
                if (it.key().startsWith(QLatin1String("games:")))
                    requestArtwork(it.key().mid(6), true);
            }
        });
    }
    loadProfileFiles();

    if (m_config.mock) {
        if (!loadFile(m_config.mockPath))
            qCWarning(lcLibrary) << "mock library missing or invalid";
        return;
    }
    SystemRegistry registry;
    registry.load(m_config.systemDirs, m_config.home);
    applyRegistry(registry);
    loadCacheNow();
    if (m_config.autoScan)
        startScan(FullScan | SteamScan, false);
}

Library::~Library()
{
    m_cancel->store(true);
    m_future.disconnect(this);
    m_future.waitForFinished();
}

Library *Library::create(QQmlEngine *qmlEngine, QJSEngine *)
{
    auto *profiles = qmlEngine->singletonInstance<ProfileService *>("MightyOS.Core", "ProfileService");
    auto *launcher = qmlEngine->singletonInstance<Launcher *>("MightyOS.Core", "Launcher");
    auto *scraper = qmlEngine->singletonInstance<ArtworkScraper *>("MightyOS.Core", "Scraper");
    return new Library(configFromEnvironment(), profiles, launcher, scraper);
}

void Library::waitForScan()
{
    while (m_future.isRunning() || m_pendingKinds != 0) {
        m_future.waitForFinished();
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();
}

// --- sources ---------------------------------------------------------------

bool Library::loadFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFileBytes)
        return false;
    QJsonParseError error {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(kMaxFileBytes), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()
        || doc.object().value(QStringLiteral("version")).toInt() != 1)
        return false;
    const QJsonObject root = doc.object();

    QList<System> systems;
    QHash<QString, int> systemIndex;
    for (const QJsonValue &value : root.value(QStringLiteral("systems")).toArray()) {
        const QJsonObject o = value.toObject();
        System s;
        s.id = o.value(QStringLiteral("id")).toString();
        s.label = o.value(QStringLiteral("label")).toString();
        s.fullName = o.value(QStringLiteral("fullName")).toString();
        s.category = o.value(QStringLiteral("category")).toString();
        s.icon = o.value(QStringLiteral("icon")).toString();
        s.tint = o.value(QStringLiteral("tint")).toString();
        s.kind = s.id == QLatin1String("steam") ? SystemDef::Kind::Steam : SystemDef::Kind::Roms;
        if (!SystemRegistry::isValidSystemId(s.id) || s.label.isEmpty() || systemIndex.contains(s.id))
            continue;
        if (!isValidColour(s.tint))
            s.tint.clear();
        systemIndex.insert(s.id, static_cast<int>(systems.size()));
        systems.append(s);
    }

    QHash<QString, QList<Game>> games;
    QSet<QString> seen;
    const QJsonObject gameRoot = root.value(QStringLiteral("games")).toObject();
    for (const System &s : std::as_const(systems)) {
        QList<Game> list;
        for (const QJsonValue &value : gameRoot.value(s.id).toArray()) {
            const QJsonObject o = value.toObject();
            Game g;
            g.id = o.value(QStringLiteral("id")).toString();
            g.title = o.value(QStringLiteral("title")).toString();
            g.system = s.id;
            g.year = o.value(QStringLiteral("year")).toInt();
            g.subtitle = g.year > 0 ? QString::number(g.year) : QString();
            if (g.id.isEmpty() || g.title.isEmpty() || seen.contains(g.id))
                continue;
            seen.insert(g.id);
            list.append(g);
        }
        sortGames(list);
        games.insert(s.id, list);
    }

    m_systems = systems;
    m_systemIndex = systemIndex;
    setGames(std::move(games));
    const bool wasReady = m_ready, wasScanned = m_scannedOnce;
    m_ready = true;
    m_scannedOnce = true;
    refreshModels();
    if (!wasReady)
        emit readyChanged();
    if (!wasScanned)
        emit scannedChanged();
    notifyChanged();
    return true;
}

void Library::applyRegistry(const SystemRegistry &registry)
{
    m_registry = registry;
    m_systems.clear();
    m_systemIndex.clear();
    for (const SystemDef &def : m_registry.systems()) {
        System s;
        s.id = def.id;
        s.label = def.label;
        s.fullName = def.fullName;
        s.category = def.category;
        s.icon = def.icon;
        s.tint = def.tint;
        s.kind = def.kind;
        s.romDirs = def.romDirs;
        s.extensions = def.extensions;
        m_systemIndex.insert(s.id, static_cast<int>(m_systems.size()));
        m_systems.append(s);
    }
    qCInfo(lcLibrary) << "systems:" << m_systems.size() << "defined," << m_registry.errors().size() << "rejected";
}

// Synchronous (GUI thread, before QML restores the saved path): the cache
// rows become the games until the first scan replaces them. 20k ROMs take
// about a quarter of a second.
void Library::loadCacheNow()
{
    if (m_config.cachePath.isEmpty())
        return;
    LibraryCache cache(m_config.cachePath);
    if (!cache.open())
        return;
    m_cacheBase = cache.load();
    if (m_cacheBase.isEmpty())
        return;   // nothing cached: stay pending until the first scan
    QHash<QString, QList<Game>> games;
    for (const RomRecord &r : std::as_const(m_cacheBase)) {
        if (m_systemIndex.contains(r.system))
            games[r.system].append(gameFromRom(r, false));
    }
    for (auto it = games.begin(); it != games.end(); ++it)
        sortGames(it.value());
    setGames(std::move(games));
    m_ready = true;
    qCInfo(lcLibrary) << "cache:" << m_cacheBase.size() << "ROMs";
}

void Library::rescan()
{
    if (m_config.mock)
        return;
    m_rescanTimer.stop();
    m_timerKinds = 0;
    startScan(FullScan | SteamScan, true);
}

void Library::scheduleRescan(int kinds)
{
    if (m_config.mock)
        return;
    m_timerKinds |= kinds;
    if (!m_afterLaunch)
        m_rescanTimer.start();
}

void Library::startScan(int kinds, bool reloadRegistry)
{
    if (kinds == 0)
        return;
    if (m_future.isRunning()) {
        m_pendingKinds |= kinds;
        m_pendingRegistry = m_pendingRegistry || reloadRegistry;
        return;
    }
    ScanInput input;
    input.systemDirs = m_config.systemDirs;
    input.registry = m_registry;
    input.reloadRegistry = reloadRegistry;
    input.scanRoms = (kinds & FullScan) != 0;
    input.scanSteam = m_config.steam && (kinds & (FullScan | SteamScan)) != 0;
    input.home = m_config.home;
    input.cachePath = m_config.cachePath;
    if (input.scanRoms) {
        if (!m_cacheBase.isEmpty()) {
            input.cacheBase = std::exchange(m_cacheBase, {});   // the startup load: the first diff base
            input.haveCacheBase = true;
        }
        m_cacheBase.clear();
    }
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    m_future.setFuture(QtConcurrent::run(
        [input, cancel](QPromise<std::shared_ptr<ScanOutput>> &promise) { runScan(promise, input, cancel); }));
    emit scanningChanged();
}

void Library::applyScan(const ScanOutput &out)
{
    // A reloaded registry and the games scanned with it land together.
    if (out.registry)
        applyRegistry(*out.registry);
    QHash<QString, QList<Game>> games;
    for (const System &s : std::as_const(m_systems)) {
        const bool fresh = s.kind == SystemDef::Kind::Steam ? out.steamScanned : out.romSystems.contains(s.id);
        if (fresh)
            games.insert(s.id, out.games.value(s.id));
        else if (m_games.contains(s.id))
            games.insert(s.id, m_games.value(s.id));   // not part of this scan
    }
    setGames(std::move(games));
    const bool wasReady = m_ready;
    m_ready = true;
    qCInfo(lcLibrary).nospace() << "scan: " << out.stats.matched << " ROMs (" << out.stats.reused << " unchanged), "
                                << out.steamGames << " Steam games, " << out.stats.dirs << " dirs, "
                                << out.stats.symlinksRejected << " symlinks rejected, "
                                << out.stats.missingRoots.size() << " ROM dirs missing, " << out.written
                                << " cache writes, " << out.deleted << " deletes, " << out.elapsedMs << " ms"
                                << (out.stats.truncated ? " (limits hit)" : "");
    updateWatcher(out.watchPaths);
    refreshModels();
    for (auto it = m_models.cbegin(); it != m_models.cend(); ++it) {
        if (it.key().startsWith(QLatin1String("games:")))
            requestArtwork(it.key().mid(6), false);
    }
    if (!wasReady)
        emit readyChanged();
    if (out.full && !m_scannedOnce) {
        m_scannedOnce = true;
        emit scannedChanged();
    }
    notifyChanged();
    emit scanFinished();
}

void Library::setGames(QHash<QString, QList<Game>> games)
{
    m_gameIndex.clear();
    for (auto it = games.cbegin(); it != games.cend(); ++it) {
        const QList<Game> &list = it.value();
        for (int i = 0; i < list.size(); ++i) {
            if (!m_gameIndex.contains(list.at(i).id))
                m_gameIndex.insert(list.at(i).id, { it.key(), i });
        }
    }
    m_games = std::move(games);
}

void Library::updateWatcher(const QStringList &paths)
{
    if (!m_config.watch)
        return;
    const QStringList old = m_watcher.directories();
    if (QSet<QString>(old.cbegin(), old.cend()) == QSet<QString>(paths.cbegin(), paths.cend()))
        return;
    if (!old.isEmpty())
        m_watcher.removePaths(old);
    if (!paths.isEmpty())
        m_watcher.addPaths(paths);
}

// --- profiles --------------------------------------------------------------

QString Library::profileFile(const QString &name) const
{
    if (!m_profiles)
        return {};
    const QString dir = m_profiles->profileConfigDir(m_profileId);
    return dir.isEmpty() ? QString() : dir + QLatin1Char('/') + name;
}

void Library::loadProfileFiles()
{
    m_profileId = m_profiles ? m_profiles->activeId() : QString();
    m_allow = {};
    m_gameEmulator.clear();
    m_systemEmulator.clear();
    if (!m_profiles)
        return;
    const QVariantMap profile = m_profiles->profile(m_profileId);
    if (profile.value(QStringLiteral("kind")).toString() == QLatin1String("kids")) {
        m_allow.restricted = true;
        const auto root = JsonFile::read(profileFile(QStringLiteral("allowed.json")), kMaxProfileFileBytes);
        if (root && root->value(QStringLiteral("version")).toInt(-1) == 1) {
            for (const QJsonValue &v : root->value(QStringLiteral("systems")).toArray()) {
                if (SystemRegistry::isValidSystemId(v.toString()))
                    m_allow.systems.insert(v.toString());
            }
            for (const QJsonValue &v : root->value(QStringLiteral("games")).toArray()) {
                if (Ids::isValidItemKey(v.toString()) && m_allow.games.size() < 10000)
                    m_allow.games.insert(v.toString());
            }
        }
    }
    const auto overrides = JsonFile::read(profileFile(QStringLiteral("emulators.json")), kMaxProfileFileBytes);
    if (overrides && overrides->value(QStringLiteral("version")).toInt(-1) == 1) {
        const QJsonObject games = overrides->value(QStringLiteral("games")).toObject();
        for (auto it = games.begin(); it != games.end() && m_gameEmulator.size() < 10000; ++it) {
            if (Ids::isValidItemKey(it.key()) && SystemRegistry::isValidSystemId(it.value().toString()))
                m_gameEmulator.insert(it.key(), it.value().toString());
        }
        const QJsonObject systems = overrides->value(QStringLiteral("systems")).toObject();
        for (auto it = systems.begin(); it != systems.end(); ++it) {
            if (SystemRegistry::isValidSystemId(it.key()) && SystemRegistry::isValidSystemId(it.value().toString()))
                m_systemEmulator.insert(it.key(), it.value().toString());
        }
    }
}

bool Library::saveOverrides() const
{
    QJsonObject games, systems;
    for (auto it = m_gameEmulator.cbegin(); it != m_gameEmulator.cend(); ++it)
        games.insert(it.key(), it.value());
    for (auto it = m_systemEmulator.cbegin(); it != m_systemEmulator.cend(); ++it)
        systems.insert(it.key(), it.value());
    const QString path = profileFile(QStringLiteral("emulators.json"));
    return !path.isEmpty()
           && JsonFile::write(path, QJsonObject { { QStringLiteral("version"), 1 },
                                                  { QStringLiteral("games"), games },
                                                  { QStringLiteral("systems"), systems } });
}

bool Library::systemAllowed(const QString &systemId) const
{
    if (!m_allow.restricted || m_allow.systems.contains(systemId))
        return true;
    // Individually allowed games: their keys name the system, so this holds
    // before any game is loaded (a saved grid survives startup).
    const auto found = m_systemIndex.constFind(systemId);
    if (found == m_systemIndex.cend())
        return false;
    const bool steam = m_systems.at(found.value()).kind == SystemDef::Kind::Steam;
    const QString prefix = steam ? QStringLiteral("steam:") : QStringLiteral("rom:") + systemId + QLatin1Char(':');
    for (const QString &key : m_allow.games) {
        if (key.startsWith(prefix))
            return true;
    }
    return false;
}

bool Library::gameAllowed(const Game &game) const
{
    return !m_allow.restricted || m_allow.systems.contains(game.system) || m_allow.games.contains(game.id);
}

// --- models ----------------------------------------------------------------

void Library::notifyChanged()
{
    if (m_config.changedDelayMs <= 0)
        emit changed();
    else
        m_changedTimer.start();   // debounced: one signal per burst
}

void Library::setShowAllSystems(bool show)
{
    if (m_showAll == show)
        return;
    m_showAll = show;
    emit showAllSystemsChanged();
    refreshModels();
}

int Library::gameCount() const
{
    int n = 0;
    for (const System &s : m_systems)
        n += visibleGameCount(s.id);
    return n;
}

int Library::visibleGameCount(const QString &systemId) const
{
    const auto found = m_games.constFind(systemId);
    if (found == m_games.cend())
        return 0;
    if (!m_allow.restricted)
        return static_cast<int>(found->size());
    int n = 0;
    for (const Game &g : *found)
        n += gameAllowed(g) ? 1 : 0;
    return n;
}

QList<const Library::System *> Library::visibleSystems(const QString &category) const
{
    QList<const System *> out;
    for (const System &s : m_systems) {
        if (s.category != category || !systemAllowed(s.id))
            continue;
        if ((!m_showAll || m_allow.restricted) && visibleGameCount(s.id) == 0)
            continue;
        out.append(&s);
    }
    return out;
}

QString Library::artworkFor(const Game &game) const
{
    QString path = game.localArt;   // Steam's own art first (D11)
    if (path.isEmpty())
        path = m_scraped.value(game.id);
    return path.isEmpty() ? QString() : ArtworkProvider::urlFor(QStringLiteral("file:") + path);
}

TileItem Library::systemTile(const System &system) const
{
    TileItem item;
    item.key = system.id;
    item.title = system.label;
    item.subtitle = system.fullName;
    item.icon = system.icon;
    item.tint = system.tint;
    item.kind = QStringLiteral("system");
    if (!m_config.mock && visibleGameCount(system.id) == 0)
        item.subtitle = tr("%1 · no games yet").arg(system.fullName);
    return item;
}

TileItem Library::gameTile(const Game &game, const System &system) const
{
    TileItem item;
    item.key = game.id;
    item.title = game.title;
    item.subtitle = game.subtitle;
    item.icon = system.icon;
    item.tint = system.tint;
    item.kind = QStringLiteral("game");
    item.artwork = artworkFor(game);
    return item;
}

QList<TileItem> Library::itemsFor(const QString &cacheKey) const
{
    QList<TileItem> items;
    if (cacheKey.startsWith(QLatin1String("systems:"))) {
        const QString category = cacheKey.mid(8);
        const QList<const System *> visible = visibleSystems(category);
        for (const System *s : visible)
            items.append(systemTile(*s));
        if (!m_showAll && !m_allow.restricted && !m_config.mock) {
            int hidden = 0;
            for (const System &s : m_systems)
                hidden += s.category == category ? 1 : 0;
            hidden -= static_cast<int>(visible.size());
            if (hidden > 0) {
                TileItem more;
                more.key = kShowAllPrefix + category;
                more.title = visible.isEmpty() ? tr("Show all systems") : tr("%n more", nullptr, hidden);
                more.subtitle = tr("Systems without games yet");
                more.icon = QStringLiteral("add");
                more.tint = QStringLiteral("#5a6274");
                more.kind = QStringLiteral("showall");
                items.append(more);
            }
        }
        return items;
    }
    const QString systemId = cacheKey.mid(6);
    const auto found = m_systemIndex.constFind(systemId);
    if (found == m_systemIndex.cend())
        return items;   // e.g. removed from systems.d by a re-scan
    const System &s = m_systems.at(found.value());
    const auto games = m_games.constFind(systemId);
    if (games == m_games.cend())
        return items;
    items.reserve(games->size());
    for (const Game &g : *games) {
        if (gameAllowed(g))
            items.append(gameTile(g, s));
    }
    return items;
}

void Library::refreshModels()
{
    for (auto it = m_models.cbegin(); it != m_models.cend(); ++it)
        it.value()->setItems(itemsFor(it.key()));
}

TileListModel *Library::cachedModel(const QString &cacheKey)
{
    TileListModel *model = m_models.value(cacheKey);
    if (!model) {
        model = new TileListModel(this);
        QQmlEngine::setObjectOwnership(model, QQmlEngine::CppOwnership);
        m_models.insert(cacheKey, model);
        model->setItems(itemsFor(cacheKey));
    }
    return model;
}

TileListModel *Library::systems(const QString &category)
{
    return cachedModel(QStringLiteral("systems:") + category);
}

TileListModel *Library::games(const QString &systemId)
{
    if (!m_systemIndex.contains(systemId) || !systemAllowed(systemId))
        return nullptr;
    const bool fresh = !m_models.contains(QStringLiteral("games:") + systemId);
    TileListModel *model = cachedModel(QStringLiteral("games:") + systemId);
    if (fresh)
        requestArtwork(systemId, true);
    return model;
}

void Library::requestArtwork(const QString &systemId, bool front)
{
    if (!m_scraper || m_config.mock || !m_scraper->hasKey())
        return;
    const auto found = m_games.constFind(systemId);
    if (found == m_games.cend())
        return;
    // Index lookups only (no file stats here: 20k games on the GUI thread).
    QList<const Game *> batch;
    bool changedArt = false;
    for (const Game &g : *found) {
        if (!g.localArt.isEmpty() || !gameAllowed(g))
            continue;
        if (const QString cached = m_scraper->cachedPath(g.id); !cached.isEmpty()) {
            if (m_scraped.value(g.id) != cached) {
                m_scraped.insert(g.id, cached);
                changedArt = true;
            }
            continue;
        }
        batch.append(&g);
    }
    // The first screenful goes to the front of the queue *in grid order*
    // (each front request is prepended, so they go in reverse).
    const int frontCount = front ? qMin<int>(48, batch.size()) : 0;
    for (int i = frontCount - 1; i >= 0; --i)
        m_scraper->request(batch.at(i)->id, batch.at(i)->title, batch.at(i)->appid, true);
    for (int i = frontCount; i < batch.size(); ++i)
        m_scraper->request(batch.at(i)->id, batch.at(i)->title, batch.at(i)->appid, false);
    if (changedArt) {
        if (TileListModel *model = m_models.value(QStringLiteral("games:") + systemId))
            model->setItems(itemsFor(QStringLiteral("games:") + systemId));
    }
}

void Library::artworkArrived(const QString &key, const QString &path)
{
    if (!Ids::isCleanAbsolutePath(path))
        return;
    m_scraped.insert(key, path);
    const auto found = m_gameIndex.constFind(key);
    if (found == m_gameIndex.cend())
        return;
    const auto system = m_systemIndex.constFind(found->first);
    TileListModel *model = m_models.value(QStringLiteral("games:") + found->first);
    const int row = model ? model->indexOfKey(key) : -1;
    if (row >= 0 && system != m_systemIndex.cend()) {
        const Game &g = m_games.constFind(found->first)->at(found->second);
        model->updateItem(row, gameTile(g, m_systems.at(system.value())));
    }
    notifyChanged();   // favorites / recent tiles
}

// --- queries ---------------------------------------------------------------

bool Library::hasSystem(const QString &systemId) const
{
    return m_systemIndex.contains(systemId) && systemAllowed(systemId);
}

QString Library::displayPath(const QString &path) const
{
    if (!m_config.home.isEmpty() && path.startsWith(m_config.home + QLatin1Char('/')))
        return QStringLiteral("~") + path.mid(m_config.home.size());
    return path;
}

QVariantMap Library::system(const QString &systemId) const
{
    if (!hasSystem(systemId))
        return {};
    const System &s = m_systems.at(m_systemIndex.value(systemId));
    QStringList dirs;
    for (const QString &d : s.romDirs)
        dirs.append(displayPath(d));
    return {
        { QStringLiteral("key"), s.id }, { QStringLiteral("title"), s.label },
        { QStringLiteral("fullName"), s.fullName }, { QStringLiteral("category"), s.category },
        { QStringLiteral("icon"), s.icon }, { QStringLiteral("tint"), s.tint },
        { QStringLiteral("kind"), s.kind == SystemDef::Kind::Steam ? QStringLiteral("steam") : QStringLiteral("roms") },
        { QStringLiteral("gameCount"), visibleGameCount(s.id) },
        { QStringLiteral("romDirs"), dirs }, { QStringLiteral("extensions"), s.extensions },
    };
}

QString Library::adjacentSystem(const QString &systemId, int delta) const
{
    const auto found = m_systemIndex.constFind(systemId);
    if (found == m_systemIndex.cend())
        return {};
    const QString category = m_systems.at(found.value()).category;
    QStringList peers;
    for (const System *s : visibleSystems(category))
        peers.append(s->id);
    if (!peers.contains(systemId))
        return {};
    const qsizetype next = peers.indexOf(systemId) + delta;
    return (next >= 0 && next < peers.size()) ? peers.at(next) : QString();
}

const Library::Game *Library::findGame(const QString &key) const
{
    const auto found = m_gameIndex.constFind(key);
    if (found == m_gameIndex.cend())
        return nullptr;
    const auto games = m_games.constFind(found->first);
    if (games == m_games.cend() || found->second >= games->size())
        return nullptr;
    const Game *g = &games->at(found->second);
    return gameAllowed(*g) && m_systemIndex.contains(g->system) ? g : nullptr;
}

std::optional<TileItem> Library::resolve(const QString &key) const
{
    if (const auto system = m_systemIndex.constFind(key); system != m_systemIndex.cend()) {
        if (!systemAllowed(key))
            return std::nullopt;
        return systemTile(m_systems.at(system.value()));
    }
    const Game *g = findGame(key);
    if (!g)
        return std::nullopt;
    const System &s = m_systems.at(m_systemIndex.value(g->system));
    TileItem item = gameTile(*g, s);
    item.subtitle = g->offline ? tr("%1 · not connected").arg(s.label) : s.label;
    return item;
}

bool Library::isPending(const QString &key) const
{
    if (m_config.mock)
        return false;
    if (key.startsWith(QLatin1String("steam:")))
        return !m_scannedOnce;
    return key.startsWith(QLatin1String("rom:")) && !m_ready;
}

const EmulatorDef *Library::emulatorFor(const QString &key) const
{
    const Game *g = findGame(key);
    const QString systemId = g ? g->system : key;
    const SystemDef *def = m_registry.find(systemId);
    if (!def || def->emulators.isEmpty())
        return nullptr;
    QString chosen = g ? m_gameEmulator.value(key) : QString();
    if (chosen.isEmpty())
        chosen = m_systemEmulator.value(systemId);
    if (const EmulatorDef *e = chosen.isEmpty() ? nullptr : def->emulator(chosen))
        return e;   // an explicit choice, even if it's not installed
    const EmulatorDef *fallback = def->emulator(def->defaultEmulator);
    if (fallback && EmulatorLaunch::available(*fallback, m_config.env))
        return fallback;
    for (const EmulatorDef &e : def->emulators) {
        if (EmulatorLaunch::available(e, m_config.env))
            return &e;
    }
    return fallback ? fallback : &def->emulators.constFirst();
}

QVariantList Library::emulatorChoices(const QString &key) const
{
    QVariantList out;
    if (m_config.mock)
        return out;
    const Game *g = findGame(key);
    const SystemDef *def = m_registry.find(g ? g->system : key);
    if (!def)
        return out;
    const EmulatorDef *selected = emulatorFor(key);
    for (const EmulatorDef &e : def->emulators) {
        out.append(QVariantMap {
            { QStringLiteral("id"), e.id }, { QStringLiteral("label"), e.label },
            { QStringLiteral("available"), EmulatorLaunch::available(e, m_config.env) },
            { QStringLiteral("selected"), selected && selected->id == e.id },
            { QStringLiteral("isDefault"), e.id == def->defaultEmulator },
        });
    }
    return out;
}

bool Library::setEmulator(const QString &key, const QString &emulatorId)
{
    if (m_config.mock || profileFile(QStringLiteral("emulators.json")).isEmpty())
        return false;
    const Game *g = findGame(key);
    const SystemDef *def = m_registry.find(g ? g->system : key);
    if (!def || (!emulatorId.isEmpty() && !def->emulator(emulatorId)))
        return false;
    QHash<QString, QString> &map = g ? m_gameEmulator : m_systemEmulator;
    const QString old = map.value(key);
    if (emulatorId.isEmpty())
        map.remove(key);
    else
        map.insert(key, emulatorId);
    if (!saveOverrides()) {
        if (old.isEmpty())
            map.remove(key);
        else
            map.insert(key, old);
        return false;
    }
    return true;
}

QVariantMap Library::details(const QString &key) const
{
    const Game *g = findGame(key);
    if (!g)
        return {};
    const System &s = m_systems.at(m_systemIndex.value(g->system));
    const EmulatorDef *e = emulatorFor(key);
    return {
        { QStringLiteral("title"), g->title },
        { QStringLiteral("system"), s.fullName },
        { QStringLiteral("file"), TextSanitize::label(g->relPath, 160) },
        { QStringLiteral("folder"), TextSanitize::label(displayPath(g->root), 160) },
        { QStringLiteral("region"), g->region },
        { QStringLiteral("tags"), g->tags },
        { QStringLiteral("discs"), g->discs },
        { QStringLiteral("offline"), g->offline },
        { QStringLiteral("emulator"), s.kind == SystemDef::Kind::Steam ? QStringLiteral("Steam")
                                      : e ? e->label : QString() },
        { QStringLiteral("year"), g->year },
    };
}

// --- launching -------------------------------------------------------------

QVariantMap Library::launch(const QString &key)
{
    auto result = [](bool attempted, const QString &message, const QString &install = QString()) {
        return QVariantMap { { QStringLiteral("ok"), attempted && message.isEmpty() },
                             { QStringLiteral("attempted"), attempted },
                             { QStringLiteral("message"), message },
                             { QStringLiteral("install"), install } };
    };
    if (!m_launcher)
        return result(false, tr("Launching is not available."));
    const Game *g = findGame(key);
    if (!g)
        return result(false, tr("This game is not in the library (any more)."));

    if (m_config.mock) {
        const QStringList argv = m_launcher->testAppCommand();
        if (argv.isEmpty())
            return result(false, tr("No test application found on this system"));
        return result(m_launcher->launch(key, argv), QString());
    }

    const System &s = m_systems.at(m_systemIndex.value(g->system));
    if (s.kind == SystemDef::Kind::Steam) {
        const LaunchPlan plan = EmulatorLaunch::planSteam(g->appid, g->flatpakSteam, m_config.env);
        if (!plan.ok())
            return result(false, plan.error, plan.install);
        SteamRunTracker::Options options;
        options.pidFiles = SteamLibrary::steamPidFiles(m_config.home);
        auto *tracker = new SteamRunTracker(g->appid, SteamLibrary::registryFiles(m_config.home), options);
        const bool restricted = m_allow.restricted;
        const bool flatpak = g->flatpakSteam;
        if (m_launcher->launch(key, plan.argv, tracker) && restricted) {
            m_kidsSteamKey = key;
            m_kidsSteamFlatpak = flatpak;
        }
        return result(true, QString());
    }

    if (g->offline)
        return result(false, tr("The drive with %1 isn't connected.").arg(g->title));
    const SystemDef *def = m_registry.find(s.id);
    const EmulatorDef *emulator = emulatorFor(key);
    if (!def || !emulator)
        return result(false, tr("No emulator is set up for %1.").arg(s.label));
    const QString rom = RomScanner::verifiedPath(g->root, g->relPath);
    if (rom.isEmpty()) {
        const QString title = g->title;
        const QString label = s.label;
        rescan();
        return result(false, tr("The file for %1 is missing. Scanning %2 again.").arg(title, label));
    }
    LaunchPlan plan = EmulatorLaunch::plan(*def, *emulator, rom, m_profileId, m_config.env, m_allow.restricted);
    if (!plan.ok())
        return result(false, plan.error, plan.install);
    if (!plan.configPath.isEmpty()) {
        QString error;
        if (!JsonFile::writePrivate(plan.configBase, plan.configSubdirs, plan.configName, plan.configContent, 0600,
                                    &error)) {
            qCWarning(lcLibrary).noquote() << "RetroArch config for" << s.id << "not written:" << error
                                           << "- launching without it";
            for (const QString &arg : std::as_const(plan.configArgv))
                plan.argv.removeOne(arg);
        }
    }
    // Save folders exist before the emulator needs them (ours, outside any sandbox).
    const QString saves = EmulatorLaunch::savesDir(m_config.env, m_profileId, s.id);
    if (!saves.isEmpty())
        QDir().mkpath(saves);
    qCInfo(lcLibrary) << "launching" << key << "with" << emulator->id;
    m_launcher->launch(key, plan.argv);
    return result(true, QString());
}
