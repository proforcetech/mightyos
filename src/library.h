#pragma once

#include "emulatorlaunch.h"
#include "romscanner.h"
#include "steamlibrary.h"
#include "systemregistry.h"
#include "tilelistmodel.h"

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <atomic>
#include <memory>
#include <optional>

class ArtworkScraper;
class Launcher;
class ProfileService;
class QJSEngine;
class QQmlEngine;

// Systems and their games (PLAN D12: Emulators -> systems, Games ->
// [Steam]; Steam is a system like any emulator).
//
// Sources (M4):
//  - systems: SystemRegistry (systems.d layers), loaded synchronously at
//    construction and again on the worker for every full re-scan (the new
//    registry is applied together with that scan's result),
//  - ROMs: RomScanner on a worker thread, cached in library.sqlite
//    (LibraryCache). The cache is read synchronously in the constructor,
//    before QML restores the saved focus path, so saved positions in game
//    grids resolve on the first frame; `ready` is true from then on if the
//    cache had anything, else after the first scan. A ROM dir that is
//    missing (unplugged drive) keeps its cached games, shown as "not
//    connected", instead of emptying the grid,
//  - Steam: SteamLibrary (appmanifest_*.acf), scanned on the worker with
//    the ROMs, or alone when only a Steam library changed. `scanned` is
//    true after the first full scan (Steam keys resolve from then on).
// Sorting and sort keys are computed on the worker. Re-scans: rescan(),
// and a QFileSystemWatcher on the ROM dirs, their parents (or the nearest
// existing ancestor of a missing dir) and the Steam steamapps dirs, all
// computed on the worker (debounced kRescanDelayMs; deeper files are picked
// up by the next scan). Watcher re-scans wait while a game runs.
//
// Mock mode (tests, WSL development; `--mock-library` or
// MIGHTYOS_MOCK_LIBRARY=1): the bundled data/mock/library.json, no scans,
// and games launch the bundled test app.
//
// Visibility: systems(category) lists systems that have games, plus a
// "+N more" tile (kind "showall") while showAllSystems is off; with it on,
// every defined system. A kids profile (PLAN D13) only sees what its
// profiles/<id>/allowed.json allows: { "version": 1, "systems": [ids],
// "games": [keys] }; no file means nothing is allowed (M8 adds the editor).
// Hidden games don't resolve (favorites show them unavailable) and can't be
// launched.
//
// Models handed to QML are owned by the Library (C++ ownership), keep their
// identity, and are updated with TileListModel::setItems() (a diff, so a
// re-scan doesn't reset focus). `changed` is debounced (kChangedDelayMs):
// a 20k-ROM scan emits it once.
//
// Per-profile emulator choice: profiles/<id>/emulators.json
//   { "version": 1, "games": { "<key>": "<emulator id>" },
//     "systems": { "<system>": "<emulator id>" } }
class Library : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool ready READ isReady NOTIFY readyChanged FINAL)
    // The first full scan finished (all game keys are final).
    Q_PROPERTY(bool scanned READ isScanned NOTIFY scannedChanged FINAL)
    Q_PROPERTY(bool scanning READ isScanning NOTIFY scanningChanged FINAL)
    Q_PROPERTY(bool mock READ isMock CONSTANT FINAL)
    Q_PROPERTY(bool showAllSystems READ showAllSystems WRITE setShowAllSystems NOTIFY showAllSystemsChanged FINAL)
    Q_PROPERTY(int gameCount READ gameCount NOTIFY changed FINAL)
    // The active profile is a kids profile (PLAN D13).
    Q_PROPERTY(bool restricted READ isRestricted NOTIFY restrictedChanged FINAL)
    // Games whose ROM dir is missing ("Not connected"), all systems.
    Q_PROPERTY(int missingCount READ missingCount NOTIFY changed FINAL)

public:
    static constexpr qint64 kMaxFileBytes = 4 * 1024 * 1024;
    static constexpr qint64 kMaxProfileFileBytes = 256 * 1024;
    static constexpr int kChangedDelayMs = 150;
    static constexpr int kRescanDelayMs = 2000;
    static inline const QString kBundledPath = QStringLiteral(":/mightyos/mock/library.json");

    struct Config {
        bool mock = false;
        QString mockPath = kBundledPath;
        QStringList systemDirs;   // SystemRegistry layers
        QString home;
        QString cachePath;        // library.sqlite; empty: no cache
        bool steam = true;
        bool autoScan = true;
        bool watch = true;
        LaunchEnv env;
        int changedDelayMs = kChangedDelayMs;
    };
    // MIGHTYOS_MOCK_LIBRARY=1 selects mock mode.
    static Config configFromEnvironment();

    // Mock mode with the bundled file, no profiles or launcher (tests).
    // Deliberately not default-constructible: QML must use create().
    explicit Library(QObject *parent);
    Library(const Config &config, ProfileService *profiles, Launcher *launcher, ArtworkScraper *scraper,
            QObject *parent = nullptr);
    ~Library() override;
    static Library *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    // Mock mode: replaces the content. Returns false (and keeps the old
    // content) if the file is missing, oversized or malformed.
    bool loadFile(const QString &path);

    bool isReady() const { return m_ready; }
    bool isScanned() const { return m_scannedOnce; }
    bool isScanning() const { return m_future.isRunning(); }
    bool isMock() const { return m_config.mock; }
    bool showAllSystems() const { return m_showAll; }
    void setShowAllSystems(bool show);
    int gameCount() const;
    bool isRestricted() const { return m_allow.restricted; }
    int missingCount() const;

    // Kids allow-list for everything that is not a game or system (PLAN
    // D13, M5): apps (flatpak:/desktop:/app:), web apps (webapp:<id>) and
    // Moonlight (moonlight:<host>/<app>, each app named; the host part is
    // compared case-insensitively), from the "apps" array of allowed.json.
    // Always true for standard profiles.
    Q_INVOKABLE bool itemAllowed(const QString &key) const;

    // Systems of a Home category ("emulators", "games"); an empty model for
    // anything else.
    Q_INVOKABLE TileListModel *systems(const QString &category);
    // Games of one system, sorted by title; nullptr for an unknown (or, for
    // a kids profile, not allowed) system. Never a path: only registry ids.
    Q_INVOKABLE TileListModel *games(const QString &systemId);

    Q_INVOKABLE bool hasSystem(const QString &systemId) const;
    // { key, title, fullName, category, icon, tint, kind ("roms"|"steam"),
    //   gameCount, romDirs (display, "~/ROMs/snes"), extensions } or empty.
    Q_INVOKABLE QVariantMap system(const QString &systemId) const;
    // The visible system `delta` steps away within the same category, or ""
    // at either end / for unknown ids.
    Q_INVOKABLE QString adjacentSystem(const QString &systemId, int delta) const;

    Q_INVOKABLE void rescan();
    // A full re-scan that also drops the cached games of missing ROM dirs
    // (deliberately deleted folders, a drive that is gone for good) from
    // the grids and library.sqlite. Returns how many games are dropped
    // (the current missingCount; 0: nothing to do, no scan).
    Q_INVOKABLE int forgetMissing();

    // Starts the game (through Launcher). { ok, attempted, message, install }:
    // attempted == false means Launcher was not called (message says why:
    // missing emulator, file gone, not allowed); otherwise Launcher's own
    // signals report the outcome.
    Q_INVOKABLE QVariantMap launch(const QString &key);
    // Emulator choices for a game key or a system id: [{ id, label,
    // available, selected, isDefault }]; empty for Steam / mock / unknown.
    Q_INVOKABLE QVariantList emulatorChoices(const QString &key) const;
    // Per-profile choice; "" resets to the default. False if invalid.
    Q_INVOKABLE bool setEmulator(const QString &key, const QString &emulatorId);
    // { title, system, file, folder, region, tags, discs, emulator } for a
    // game, or empty.
    Q_INVOKABLE QVariantMap details(const QString &key) const;

    // A game (rom:/steam: key) or a system (bare id). For favorites the
    // game's subtitle is its system's label.
    std::optional<TileItem> resolve(const QString &key) const;
    // True for game keys while the first cache/scan result is outstanding.
    bool isPending(const QString &key) const;

    const SystemRegistry &registry() const { return m_registry; }
    // The emulator a launch of `key` would use (override, else default,
    // else the first available one); nullptr if none.
    const EmulatorDef *emulatorFor(const QString &key) const;
    // Blocks until a running scan is done (tests).
    void waitForScan();

    // Internal (shared with the scan worker).
    struct Game {
        QString id, system, title, sortKey;
        QString subtitle;
        QString localArt;   // Steam library cache image
        QString root, relPath, region, tags;
        int discs = 1;
        QString appid;
        bool flatpakSteam = false;
        bool offline = false;   // its ROM dir is missing (drive unplugged)
        int year = 0;
    };
    struct ScanOutput;
    static void sortGames(QList<Game> &games);
    static Game gameFromRom(const RomRecord &r, bool offline);

signals:
    void changed();
    void readyChanged();
    void scannedChanged();
    void restrictedChanged();
    // allowed.json was (re)loaded: profile switch. Emitted synchronously.
    void allowListChanged();
    void scanningChanged();
    void showAllSystemsChanged();
    void scanFinished();

private:
    struct System {
        QString id, label, fullName, category, icon, tint;
        SystemDef::Kind kind = SystemDef::Kind::Roms;
        QStringList romDirs, extensions;
    };
    struct Allow {
        bool restricted = false;
        QSet<QString> systems, games, apps;
    };
    enum ScanKind { FullScan = 1, SteamScan = 2 };
    void applyRegistry(const SystemRegistry &registry);
    void loadCacheNow();
    void startScan(int kinds, bool reloadRegistry);
    void scheduleRescan(int kinds);
    void applyScan(const ScanOutput &out);
    void setGames(QHash<QString, QList<Game>> games);   // already sorted
    void refreshModels();
    void notifyChanged();
    void loadProfileFiles();
    bool saveOverrides() const;
    void updateWatcher(const QStringList &paths);
    void requestArtwork(const QString &systemId, bool front);
    void artworkArrived(const QString &key, const QString &path);

    bool gameAllowed(const Game &game) const;
    bool systemAllowed(const QString &systemId) const;
    int visibleGameCount(const QString &systemId) const;
    QList<const System *> visibleSystems(const QString &category) const;
    const Game *findGame(const QString &key) const;
    QString artworkFor(const Game &game) const;
    TileItem systemTile(const System &system) const;
    TileItem gameTile(const Game &game, const System &system) const;
    TileListModel *cachedModel(const QString &cacheKey);
    QList<TileItem> itemsFor(const QString &cacheKey) const;
    QString profileFile(const QString &name) const;
    QString displayPath(const QString &path) const;

    Config m_config;
    QPointer<ProfileService> m_profiles;
    QPointer<Launcher> m_launcher;
    QPointer<ArtworkScraper> m_scraper;
    SystemRegistry m_registry;
    QList<System> m_systems;
    QHash<QString, int> m_systemIndex;           // id -> m_systems row
    QHash<QString, QList<Game>> m_games;         // system -> sorted games
    QHash<QString, QPair<QString, int>> m_gameIndex;   // game id -> (system, row)
    QHash<QString, TileListModel *> m_models;    // "systems:<cat>" / "games:<sys>"
    QHash<QString, QString> m_scraped;           // key -> scraped artwork path
    QHash<QString, RomRecord> m_cacheBase;       // cache rows for the first scan's diff
    Allow m_allow;
    QString m_profileId;
    QHash<QString, QString> m_gameEmulator, m_systemEmulator;
    bool m_showAll = false;
    bool m_ready = false;
    bool m_scannedOnce = false;
    int m_pendingKinds = 0;        // scan kinds to run after the current one
    bool m_pendingRegistry = false;
    bool m_pendingForget = false;  // the next full scan drops offline games
    int m_timerKinds = 0;          // kinds the debounce timer will start
    bool m_afterLaunch = false;    // a watcher re-scan waits for the game to end
    bool m_kidsSteamFlatpak = false;
    QString m_kidsSteamKey;        // a kids profile's Steam launch: close the client after it
    QTimer m_changedTimer;
    QTimer m_rescanTimer;
    QFileSystemWatcher m_watcher;
    QFutureWatcher<std::shared_ptr<ScanOutput>> m_future;
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
};
