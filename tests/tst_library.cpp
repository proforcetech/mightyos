#include "jsonfile.h"
#include "launcher.h"
#include "library.h"
#include "profileservice.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

// The real Library (M4): systems.d + RomScanner + SQLite cache + Steam,
// visibility rules, kids profiles, per-profile emulator choice, launching,
// and profile deletion keeping saves.
class TestLibrary : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void scanBuildsModels();
    void cacheServesTheNextStart();
    void emptyCacheStaysPending();
    void kidsGridSurvivesStartup();
    void missingRomDirKeepsGames();
    void forgetMissingGames();
    void retroarchConfigNeverFollowsSymlinks();
    void rescanIsADiff();
    void showAllSystems();
    void kidsProfileSeesOnlyAllowed();
    void emulatorChoicePerProfile();
    void launching();
    void deleteProfileKeepsSaves();

private:
    std::unique_ptr<Library> make(bool autoScan = true);
    void write(const QString &rel, const QByteArray &data = "x")
    {
        const QString path = m_tmp->path() + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    }
    QString home() const { return m_tmp->path() + QStringLiteral("/home"); }

    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<ProfileService> m_profiles;
    std::unique_ptr<Launcher> m_launcher;
};

void TestLibrary::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    const QString h = home();
    m_profiles = std::make_unique<ProfileService>(h + QStringLiteral("/.config/mightyos"),
                                                  h + QStringLiteral("/.local/state/mightyos"),
                                                  h + QStringLiteral("/.local/share/mightyos"), h);
    m_launcher = std::make_unique<Launcher>();
    // Two ROM systems (one with a runnable "emulator": /usr/bin/true) and Steam.
    write(QStringLiteral("systems.d/snes.json"), R"json({"version": 1, "id": "snes", "label": "SNES",
        "category": "emulators", "order": 1, "icon": "cartridge", "tint": "#9b8cff", "extensions": ["sfc"],
        "emulators": [
          {"id": "snes9x", "label": "Snes9x (RetroArch)", "type": "retroarch", "core": "snes9x"},
          {"id": "fake", "label": "Fake", "type": "native", "program": "true", "args": ["--", "{rom}"]}]})json");
    write(QStringLiteral("systems.d/nes.json"), R"json({"version": 1, "id": "nes", "label": "NES",
        "category": "emulators", "order": 2, "extensions": ["nes"],
        "emulators": [{"id": "mesen", "label": "Mesen (RetroArch)", "type": "retroarch", "core": "mesen"}]})json");
    write(QStringLiteral("systems.d/steam.json"), R"json({"version": 1, "id": "steam", "label": "Steam",
        "category": "games", "kind": "steam"})json");
    write(QStringLiteral("home/ROMs/snes/Zelda, The (USA).sfc"));
    write(QStringLiteral("home/ROMs/snes/Aero Fighters (Japan).sfc"));
    write(QStringLiteral("home/ROMs/snes/Mario (USA) (Rev 1).sfc"));
    write(QStringLiteral("home/.local/share/Steam/steamapps/appmanifest_620.acf"),
          "\"AppState\" { \"appid\" \"620\" \"name\" \"Portal 2\" \"StateFlags\" \"4\" }");
}

void TestLibrary::cleanup()
{
    m_launcher.reset();
    m_profiles.reset();
    m_tmp.reset();
}

std::unique_ptr<Library> TestLibrary::make(bool autoScan)
{
    Library::Config config;
    config.systemDirs = { m_tmp->path() + QStringLiteral("/systems.d") };
    config.home = home();
    config.cachePath = m_tmp->path() + QStringLiteral("/cache/library.sqlite");
    config.autoScan = autoScan;
    config.watch = false;
    config.changedDelayMs = 30;
    config.env.home = home();
    config.env.dataHome = home() + QStringLiteral("/.local/share");
    config.env.stateHome = home() + QStringLiteral("/.local/state");
    config.env.pathDirs = { QStringLiteral("/usr/bin"), QStringLiteral("/bin") };
    auto library = std::make_unique<Library>(config, m_profiles.get(), m_launcher.get(), nullptr);
    library->waitForScan();
    return library;
}

void TestLibrary::scanBuildsModels()
{
    auto library = make();
    QVERIFY(library->isReady());
    QVERIFY(!library->isMock());
    TileListModel *systems = library->systems(QStringLiteral("emulators"));
    QAbstractItemModelTester tester(systems, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QCOMPARE(systems->count(), 2);                                     // SNES + "1 more"
    QCOMPARE(systems->keyAt(0), QStringLiteral("snes"));
    QCOMPARE(systems->get(1).value(QStringLiteral("kind")).toString(), QStringLiteral("showall"));
    QCOMPARE(library->systems(QStringLiteral("games"))->keyAt(0), QStringLiteral("steam"));

    TileListModel *games = library->games(QStringLiteral("snes"));
    QVERIFY(games);
    QCOMPARE(games->count(), 3);
    QCOMPARE(games->get(0).value(QStringLiteral("title")).toString(), QStringLiteral("Aero Fighters"));
    QCOMPARE(games->get(2).value(QStringLiteral("title")).toString(), QStringLiteral("The Zelda"));   // ", The" moved
    QVERIFY(games->keyAt(0).startsWith(QLatin1String("rom:snes:")));
    QCOMPARE(games->get(0).value(QStringLiteral("subtitle")).toString(), QStringLiteral("Japan"));
    QCOMPARE(library->games(QStringLiteral("steam"))->keyAt(0), QStringLiteral("steam:620"));

    // Only registry ids resolve; nothing is a path.
    QVERIFY(!library->games(QStringLiteral("../snes")));
    QVERIFY(!library->games(QStringLiteral("/etc")));
    QVERIFY(library->hasSystem(QStringLiteral("nes")));
    QCOMPARE(library->system(QStringLiteral("snes")).value(QStringLiteral("romDirs")).toStringList(),
             QStringList { QStringLiteral("~/ROMs/snes") });
    const auto tile = library->resolve(games->keyAt(1));
    QVERIFY(tile);
    QCOMPARE(tile->subtitle, QStringLiteral("SNES"));
    QVERIFY(!library->isPending(games->keyAt(1)));
    QCOMPARE(library->adjacentSystem(QStringLiteral("snes"), 1), QString());   // nes is hidden (no games)
    QCOMPARE(library->details(games->keyAt(1)).value(QStringLiteral("tags")).toString(), QStringLiteral("Rev 1"));
}

void TestLibrary::cacheServesTheNextStart()
{
    make();   // writes the cache
    // Next start (H1): the cache is read synchronously in the constructor,
    // so saved positions resolve before QML restores them; no scan yet.
    auto library = make(false);
    QVERIFY(library->isReady());
    QVERIFY(!library->isScanned());
    QCOMPARE(library->games(QStringLiteral("snes"))->count(), 3);
    QVERIFY(!library->isPending(library->games(QStringLiteral("snes"))->keyAt(0)));
    QVERIFY(library->isPending(QStringLiteral("steam:620")));   // Steam isn't cached
    library->rescan();
    library->waitForScan();
    QVERIFY(library->isScanned());
    QCOMPARE(library->games(QStringLiteral("snes"))->count(), 3);
    QVERIFY(!library->isPending(QStringLiteral("steam:620")));
}

void TestLibrary::emptyCacheStaysPending()
{
    // M7: nothing cached -> not "ready" (no empty grids passed off as final).
    auto library = make(false);
    QVERIFY(!library->isReady());
    QVERIFY(library->isPending(RomScanner::romId(QStringLiteral("snes"), QStringLiteral("x.sfc"))));
}

void TestLibrary::kidsGridSurvivesStartup()
{
    // H1: a kids profile allowed one game keeps its grid before any games
    // are loaded (the saved path must not be dropped).
    const QString game = RomScanner::romId(QStringLiteral("snes"), QStringLiteral("Aero Fighters (Japan).sfc"));
    const QString kid = m_profiles->create(QStringLiteral("Kid"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"),
                                           QStringLiteral("kids"));
    write(QStringLiteral("home/.config/mightyos/profiles/") + kid + QStringLiteral("/allowed.json"),
          QJsonDocument(QJsonObject { { QStringLiteral("version"), 1 },
                                      { QStringLiteral("games"), QJsonArray { game } } }).toJson());
    QVERIFY(m_profiles->select(kid));
    auto library = make(false);   // no cache, no scan
    QVERIFY(library->games(QStringLiteral("snes")));
    QVERIFY(!library->games(QStringLiteral("nes")));
    QVERIFY(!library->games(QStringLiteral("steam")));
    library->rescan();
    library->waitForScan();
    QCOMPARE(library->games(QStringLiteral("snes"))->count(), 1);
    QCOMPARE(library->games(QStringLiteral("snes"))->keyAt(0), game);
}

void TestLibrary::missingRomDirKeepsGames()
{
    // M6: an unplugged drive is not "no games".
    auto library = make();
    QCOMPARE(library->games(QStringLiteral("snes"))->count(), 3);
    QVERIFY(QDir(home() + QStringLiteral("/ROMs/snes")).rename(home() + QStringLiteral("/ROMs/snes"),
                                                              home() + QStringLiteral("/ROMs/unplugged")));
    library->rescan();
    library->waitForScan();
    TileListModel *games = library->games(QStringLiteral("snes"));
    QCOMPARE(games->count(), 3);
    QVERIFY(games->get(0).value(QStringLiteral("subtitle")).toString().contains(QLatin1String("Not connected")));
    const QVariantMap r = library->launch(games->keyAt(0));
    QVERIFY(!r.value(QStringLiteral("attempted")).toBool());
    QVERIFY(r.value(QStringLiteral("message")).toString().contains(QLatin1String("isn't connected")));
    // Still cached for the next start, and back once the dir returns.
    QVERIFY(QDir(home() + QStringLiteral("/ROMs")).rename(QStringLiteral("unplugged"), QStringLiteral("snes")));
    library->rescan();
    library->waitForScan();
    QVERIFY(!games->get(0).value(QStringLiteral("subtitle")).toString().contains(QLatin1String("Not connected")));
}

void TestLibrary::forgetMissingGames()
{
    // M4 review follow-up: a deliberately deleted ROM folder can be forgotten.
    auto library = make();
    QCOMPARE(library->missingCount(), 0);
    QCOMPARE(library->forgetMissing(), 0);   // nothing to do: no scan
    QVERIFY(!library->isScanning());
    QVERIFY(QDir(home() + QStringLiteral("/ROMs/snes")).removeRecursively());
    library->rescan();
    library->waitForScan();
    QCOMPARE(library->missingCount(), 3);
    QCOMPARE(library->games(QStringLiteral("snes"))->count(), 3);   // "Not connected"
    QCOMPARE(library->forgetMissing(), 3);
    library->waitForScan();
    QCOMPARE(library->missingCount(), 0);
    QCOMPARE(library->games(QStringLiteral("snes"))->count(), 0);
    // Gone from library.sqlite too: the next start doesn't bring them back.
    library.reset();
    auto again = make(false);
    QCOMPARE(again->gameCount(), 0);
}

void TestLibrary::retroarchConfigNeverFollowsSymlinks()
{
    // S1: the per-launch cfg is written with writePrivate(): a symlink in
    // its place is replaced, not followed.
    const QString victim = m_tmp->path() + QStringLiteral("/victim.txt");
    write(QStringLiteral("victim.txt"), "precious");
    QFile::setPermissions(victim, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    const QString dir = m_tmp->path() + QStringLiteral("/state");
    QDir().mkpath(dir + QStringLiteral("/mightyos/profiles/p/retroarch"));
    QVERIFY(QFile::link(victim, dir + QStringLiteral("/mightyos/profiles/p/retroarch/snes.cfg")));
    const QStringList subdirs { QStringLiteral("mightyos"), QStringLiteral("profiles"), QStringLiteral("p"),
                                QStringLiteral("retroarch") };
    QVERIFY(JsonFile::writePrivate(dir, subdirs, QStringLiteral("snes.cfg"), "cfg"));
    QFile v(victim);
    QVERIFY(v.open(QIODevice::ReadOnly));
    QCOMPARE(v.readAll(), QByteArray("precious"));
    QVERIFY(QFileInfo(victim).permissions() & QFile::ReadOther);   // no chmod through the link
    const QFileInfo cfg(dir + QStringLiteral("/mightyos/profiles/p/retroarch/snes.cfg"));
    QVERIFY(!cfg.isSymLink());
    QCOMPARE(cfg.permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
    // A symlinked directory on the way: refused, nothing written there.
    const QString elsewhere = m_tmp->path() + QStringLiteral("/elsewhere");
    QDir().mkpath(elsewhere);
    QVERIFY(QDir(dir + QStringLiteral("/mightyos/profiles")).removeRecursively());
    QDir().mkpath(dir + QStringLiteral("/mightyos"));
    QVERIFY(QFile::link(elsewhere, dir + QStringLiteral("/mightyos/profiles")));
    QString error;
    QVERIFY(!JsonFile::writePrivate(dir, subdirs, QStringLiteral("snes.cfg"), "cfg", 0600, &error));
    QVERIFY(QDir(elsewhere).isEmpty());
    QVERIFY(!JsonFile::writePrivate(dir, { QStringLiteral("..") }, QStringLiteral("x"), "cfg"));
    QVERIFY(!JsonFile::writePrivate(dir, {}, QStringLiteral("../x"), "cfg"));
}

void TestLibrary::rescanIsADiff()
{
    auto library = make();
    TileListModel *games = library->games(QStringLiteral("snes"));
    const int resets = games->resetCount();
    QSignalSpy changed(library.get(), &Library::changed);
    QSignalSpy inserted(games, &QAbstractItemModel::rowsInserted);
    write(QStringLiteral("home/ROMs/snes/Bomberman (USA).sfc"));
    QFile::remove(home() + QStringLiteral("/ROMs/snes/Mario (USA) (Rev 1).sfc"));
    library->rescan();
    library->waitForScan();
    QCOMPARE(games->count(), 3);
    QCOMPARE(games->resetCount(), resets);   // diff, focus survives
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(games->get(1).value(QStringLiteral("title")).toString(), QStringLiteral("Bomberman"));
    QTRY_COMPARE(changed.count(), 1);         // debounced
    QTest::qWait(80);
    QCOMPARE(changed.count(), 1);
}

void TestLibrary::showAllSystems()
{
    auto library = make();
    TileListModel *systems = library->systems(QStringLiteral("emulators"));
    library->setShowAllSystems(true);
    QCOMPARE(systems->count(), 2);
    QCOMPARE(systems->keyAt(1), QStringLiteral("nes"));
    QVERIFY(library->games(QStringLiteral("nes")));
    QCOMPARE(library->games(QStringLiteral("nes"))->count(), 0);
    QCOMPARE(library->adjacentSystem(QStringLiteral("snes"), 1), QStringLiteral("nes"));
    library->setShowAllSystems(false);
    QCOMPARE(systems->keyAt(1), QStringLiteral("showall:emulators"));
}

void TestLibrary::kidsProfileSeesOnlyAllowed()
{
    auto library = make();
    const QString game = library->games(QStringLiteral("snes"))->keyAt(0);
    const QString kid = m_profiles->create(QStringLiteral("Kid"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"),
                                           QStringLiteral("kids"));
    QVERIFY(m_profiles->select(kid));
    QVERIFY(library->isRestricted());
    QCOMPARE(library->systems(QStringLiteral("emulators"))->count(), 0);   // no allowed.json: nothing
    QCOMPARE(library->systems(QStringLiteral("games"))->count(), 0);
    QVERIFY(!library->games(QStringLiteral("snes")));
    QVERIFY(!library->resolve(game));
    QVERIFY(!library->launch(game).value(QStringLiteral("attempted")).toBool());

    const QString allowed = m_profiles->profileConfigDir(kid) + QStringLiteral("/allowed.json");
    QDir().mkpath(QFileInfo(allowed).absolutePath());
    QFile f(allowed);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(QJsonObject { { QStringLiteral("version"), 1 },
                                        { QStringLiteral("games"), QJsonArray { game } } }).toJson());
    f.close();
    QVERIFY(m_profiles->select(m_profiles->profiles().first().toMap().value(QStringLiteral("id")).toString()));
    QVERIFY(m_profiles->select(kid));   // re-read
    QCOMPARE(library->systems(QStringLiteral("emulators"))->count(), 1);
    QCOMPARE(library->games(QStringLiteral("snes"))->count(), 1);
    QCOMPARE(library->games(QStringLiteral("snes"))->keyAt(0), game);
    QVERIFY(library->resolve(game));
    QVERIFY(!library->games(QStringLiteral("steam")));
}

void TestLibrary::emulatorChoicePerProfile()
{
    auto library = make();
    const QString game = library->games(QStringLiteral("snes"))->keyAt(0);
    // Default snes9x is not installed, so the available "fake" one is used.
    QCOMPARE(library->emulatorFor(game)->id, QStringLiteral("fake"));
    const QVariantList choices = library->emulatorChoices(game);
    QCOMPARE(choices.size(), 2);
    QVERIFY(!choices.at(0).toMap().value(QStringLiteral("available")).toBool());
    QVERIFY(choices.at(1).toMap().value(QStringLiteral("selected")).toBool());
    QVERIFY(library->setEmulator(game, QStringLiteral("snes9x")));   // explicit, even if missing
    QCOMPARE(library->emulatorFor(game)->id, QStringLiteral("snes9x"));
    QVERIFY(!library->setEmulator(game, QStringLiteral("nope")));
    QVERIFY(!library->setEmulator(QStringLiteral("rom:snes:unknown"), QStringLiteral("fake")));
    QVERIFY(library->setEmulator(QStringLiteral("snes"), QStringLiteral("fake")));   // system default

    auto second = make(false);   // a new start reads the profile's file
    QCOMPARE(second->emulatorFor(game)->id, QStringLiteral("snes9x"));
    QCOMPARE(second->emulatorFor(second->games(QStringLiteral("snes"))->keyAt(1))->id, QStringLiteral("fake"));
    QVERIFY(second->setEmulator(game, QString()));
    QCOMPARE(second->emulatorFor(game)->id, QStringLiteral("fake"));
}

void TestLibrary::launching()
{
    auto library = make();
    const QString game = library->games(QStringLiteral("snes"))->keyAt(0);
    QSignalSpy exited(m_launcher.get(), &Launcher::exited);
    QVariantMap r = library->launch(game);   // "fake" = /usr/bin/true -- <rom>
    QVERIFY(r.value(QStringLiteral("attempted")).toBool());
    QTRY_COMPARE(exited.count(), 1);
    QCOMPARE(exited.first().at(1).toInt(), 0);

    QVERIFY(library->setEmulator(game, QStringLiteral("snes9x")));
    r = library->launch(game);
    QVERIFY(!r.value(QStringLiteral("attempted")).toBool());
    QCOMPARE(r.value(QStringLiteral("install")).toString(), QStringLiteral("RetroArch"));
    QVERIFY(r.value(QStringLiteral("message")).toString().contains(QLatin1String("App Store")));

    // Steam is not installed here.
    r = library->launch(QStringLiteral("steam:620"));
    QCOMPARE(r.value(QStringLiteral("install")).toString(), QStringLiteral("Steam"));

    // The file vanished since the scan: no launch, a re-scan.
    const QString other = library->games(QStringLiteral("snes"))->keyAt(1);
    const QString rel = library->details(other).value(QStringLiteral("file")).toString();
    QVERIFY(QFile::remove(home() + QStringLiteral("/ROMs/snes/") + rel));
    r = library->launch(other);
    QVERIFY(!r.value(QStringLiteral("attempted")).toBool());
    library->waitForScan();
    QCOMPARE(library->games(QStringLiteral("snes"))->count(), 2);
    QVERIFY(!library->launch(QStringLiteral("rom:snes:0000")).value(QStringLiteral("attempted")).toBool());
}

void TestLibrary::deleteProfileKeepsSaves()
{
    const QString other = m_profiles->create(QStringLiteral("Other"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    const QString third = m_profiles->create(QStringLiteral("Third"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    QVERIFY(!m_profiles->hasSaves(other));
    for (const QString &id : { other, third }) {
        write(QStringLiteral("home/.local/share/mightyos/profiles/") + id + QStringLiteral("/saves/snes/Game.srm"));
        write(QStringLiteral("home/.config/mightyos/profiles/") + id + QStringLiteral("/favorites.json"), "{}");
    }
    QVERIFY(m_profiles->hasSaves(other));
    QCOMPARE(m_profiles->savesLocation(other), QStringLiteral("~/.local/share/mightyos/profiles/") + other
                                                   + QStringLiteral("/saves"));
    QVERIFY(m_profiles->remove(other));   // default: keep
    QVERIFY(QFileInfo::exists(home() + QStringLiteral("/.local/share/mightyos/profiles/") + other
                              + QStringLiteral("/saves/snes/Game.srm")));
    QVERIFY(!QFileInfo::exists(m_profiles->profileConfigDir(other)));
    QVERIFY(m_profiles->remove(third, false));
    QVERIFY(!QFileInfo::exists(home() + QStringLiteral("/.local/share/mightyos/profiles/") + third));
}

QTEST_GUILESS_MAIN(TestLibrary)
#include "tst_library.moc"
