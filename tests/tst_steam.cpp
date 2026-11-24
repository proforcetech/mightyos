#include "launcher.h"
#include "steamlibrary.h"
#include "steamtracker.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

// Steam as a system (PLAN D12): roots, libraryfolders.vdf, manifests,
// tools filtering, local art, and the run tracker that decides when a
// Steam game has exited.
class TestSteam : public QObject
{
    Q_OBJECT

private slots:
    void manifests_data();
    void manifests();
    void toolsAreSkipped();
    void rootsLibrariesAndArt();
    void flatpakLibraryMapping();
    void registry();
    void processEnvironment();
    void trackerLifecycle();
    void trackerIgnoresStaleRegistry();
    void trackerTimeout();
    void launcherHandOver();

private:
    static void write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    }
    static QByteArray manifest(const QByteArray &appid, const QByteArray &name, const QByteArray &flags = "4")
    {
        return "\"AppState\"\n{\n\t\"appid\"\t\t\"" + appid + "\"\n\t\"name\"\t\t\"" + name
               + "\"\n\t\"StateFlags\"\t\t\"" + flags + "\"\n\t\"installdir\"\t\t\"x\"\n}\n";
    }
    static QByteArray registryVdf(const QByteArray &running, const QByteArray &appsBlock = QByteArray())
    {
        return "\"Registry\" { \"HKCU\" { \"Software\" { \"Valve\" { \"Steam\" { \"RunningAppID\" \"" + running
               + "\" " + appsBlock + " } } } } }";
    }
};

void TestSteam::manifests_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<bool>("ok");
    QTest::newRow("game") << manifest("620", "Portal 2") << "620" << true;
    QTest::newRow("installed + update") << manifest("620", "Portal 2", "6") << "620" << true;
    QTest::newRow("downloading") << manifest("620", "Portal 2", "1026") << "620" << false;
    QTest::newRow("appid mismatch") << manifest("621", "Portal 2") << "620" << false;
    QTest::newRow("appid not digits") << manifest("620; rm", "x") << "" << false;
    QTest::newRow("leading zero") << manifest("0620", "x") << "" << false;
    QTest::newRow("garbage") << QByteArray("\"AppState\" { \"appid\" ") << "" << false;
    QTest::newRow("no AppState") << QByteArray("\"Other\" { \"appid\" \"620\" }") << "" << false;
}

void TestSteam::manifests()
{
    QFETCH(QByteArray, data);
    QFETCH(QString, expected);
    QFETCH(bool, ok);
    const auto game = SteamLibrary::parseManifest(data, expected);
    QCOMPARE(bool(game), ok);
    if (ok)
        QCOMPARE(game->appid, expected);
}

void TestSteam::toolsAreSkipped()
{
    QVERIFY(!SteamLibrary::parseManifest(manifest("228980", "Steamworks Common Redistributables")));
    QVERIFY(!SteamLibrary::parseManifest(manifest("1628350", "Steam Linux Runtime 3.0 (sniper)")));
    QVERIFY(!SteamLibrary::parseManifest(manifest("9999999", "Proton 10.0")));
    QVERIFY(!SteamLibrary::parseManifest(manifest("9999998", "Steam Linux Runtime 4.0")));
    QVERIFY(SteamLibrary::parseManifest(manifest("400", "Portal")));
    QVERIFY(SteamLibrary::parseManifest(manifest("12345", "Protonaut")));   // a game, not "Proton "
    // Names are display text: control and bidi characters go.
    const auto game = SteamLibrary::parseManifest(manifest("400", "Port\xE2\x80\xAE" "al\x01"));
    QVERIFY(game);
    QCOMPARE(game->name, QStringLiteral("Portal"));
}

void TestSteam::rootsLibrariesAndArt()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    const QString root = home + QStringLiteral("/.local/share/Steam");
    const QString extra = home + QStringLiteral("/games/SteamLibrary");
    QDir().mkpath(root + QStringLiteral("/steamapps"));
    QDir().mkpath(home + QStringLiteral("/.steam"));
    QVERIFY(QFile::link(root, home + QStringLiteral("/.steam/steam")));   // the usual symlink
    write(root + QStringLiteral("/steamapps/libraryfolders.vdf"),
          "\"libraryfolders\" { \"0\" { \"path\" \"" + root.toUtf8() + "\" } \"1\" { \"path\" \"" + extra.toUtf8()
          + "/\" } \"2\" { \"path\" \"relative/lib\" } \"3\" { \"path\" \"/nonexistent/lib\" } }");
    write(root + QStringLiteral("/steamapps/appmanifest_620.acf"), manifest("620", "Portal 2"));
    write(root + QStringLiteral("/steamapps/appmanifest_228980.acf"), manifest("228980", "Steamworks Common Redistributables"));
    write(root + QStringLiteral("/steamapps/appmanifest_999.acf"), manifest("998", "Mismatch"));
    write(extra + QStringLiteral("/steamapps/appmanifest_400.acf"), manifest("400", "Portal"));
    write(extra + QStringLiteral("/steamapps/appmanifest_620.acf"), manifest("620", "Portal 2 duplicate"));
    // Art: old flat layout for 620, new nested + hashed layout for 400.
    write(root + QStringLiteral("/appcache/librarycache/620_library_600x900.jpg"), "jpg");
    write(root + QStringLiteral("/appcache/librarycache/400/abc123/library_600x900.jpg"), "jpg");

    const QList<SteamRoot> roots = SteamLibrary::findRoots(home);
    QCOMPARE(roots.size(), 1);   // ~/.steam/steam and ~/.local/share/Steam are one
    QCOMPARE(SteamLibrary::libraryFolders(roots.first(), home), (QStringList { root, extra }));

    const QList<SteamGame> games = SteamLibrary::scan(home);
    QCOMPARE(games.size(), 2);
    QHash<QString, SteamGame> byId;
    for (const SteamGame &g : games)
        byId.insert(g.appid, g);
    QCOMPARE(byId.value(QStringLiteral("620")).name, QStringLiteral("Portal 2"));   // first library wins
    QCOMPARE(byId.value(QStringLiteral("620")).artwork, root + QStringLiteral("/appcache/librarycache/620_library_600x900.jpg"));
    QCOMPARE(byId.value(QStringLiteral("400")).artwork,
             root + QStringLiteral("/appcache/librarycache/400/abc123/library_600x900.jpg"));
    QVERIFY(!byId.value(QStringLiteral("400")).flatpak);
}

void TestSteam::flatpakLibraryMapping()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    const QString fp = home + QStringLiteral("/.var/app/com.valvesoftware.Steam");
    const QString root = fp + QStringLiteral("/.local/share/Steam");
    QDir().mkpath(root + QStringLiteral("/steamapps"));
    // Inside the sandbox the root is at ~/.local/share/Steam.
    write(root + QStringLiteral("/steamapps/libraryfolders.vdf"),
          "\"libraryfolders\" { \"0\" { \"path\" \"" + home.toUtf8() + "/.local/share/Steam\" } }");
    write(root + QStringLiteral("/steamapps/appmanifest_620.acf"), manifest("620", "Portal 2"));
    const auto roots = SteamLibrary::findRoots(home);
    QCOMPARE(roots.size(), 1);
    QVERIFY(roots.first().flatpak);
    QCOMPARE(SteamLibrary::libraryFolders(roots.first(), home), QStringList { root });
    const auto games = SteamLibrary::scan(home);
    QCOMPARE(games.size(), 1);
    QVERIFY(games.first().flatpak);
}

void TestSteam::registry()
{
    QVERIFY(SteamLibrary::registrySaysRunning(registryVdf("620"), QStringLiteral("620")));
    QVERIFY(!SteamLibrary::registrySaysRunning(registryVdf("0"), QStringLiteral("620")));
    QVERIFY(SteamLibrary::registrySaysRunning(registryVdf("0", "\"apps\" { \"620\" { \"Running\" \"1\" } }"),
                                              QStringLiteral("620")));
    QVERIFY(!SteamLibrary::registrySaysRunning(registryVdf("0", "\"apps\" { \"620\" { \"Running\" \"0\" } }"),
                                               QStringLiteral("620")));
    QVERIFY(!SteamLibrary::registrySaysRunning("garbage {", QStringLiteral("620")));
    QVERIFY(!SteamLibrary::registrySaysRunning(registryVdf("620"), QStringLiteral("62x")));
}

void TestSteam::processEnvironment()
{
    QTemporaryDir tmp;
    write(tmp.path() + QStringLiteral("/123/environ"), QByteArray("HOME=/h\0SteamAppId=620\0", 23));
    write(tmp.path() + QStringLiteral("/456/environ"), QByteArray("SteamAppId=6200\0", 16));
    write(tmp.path() + QStringLiteral("/self/environ"), QByteArray("SteamAppId=700\0", 15));   // not a pid dir
    QVERIFY(SteamLibrary::processRunning(tmp.path(), QStringLiteral("620")));
    QVERIFY(!SteamLibrary::processRunning(tmp.path(), QStringLiteral("62")));
    QVERIFY(!SteamLibrary::processRunning(tmp.path(), QStringLiteral("700")));
    QVERIFY(SteamLibrary::processRunning(tmp.path(), QStringLiteral("6200")));
}

void TestSteam::trackerLifecycle()
{
    QTemporaryDir tmp;
    const QString reg = tmp.path() + QStringLiteral("/registry.vdf");
    const QString proc = tmp.path() + QStringLiteral("/proc");
    write(reg, registryVdf("0"));
    SteamRunTracker::Options options;
    options.pollMs = 50;
    options.startTimeoutMs = 5000;
    options.procRoot = proc;
    options.pidFiles = { tmp.path() + QStringLiteral("/steam.pid") };
    SteamRunTracker tracker(QStringLiteral("620"), { reg, tmp.path() + QStringLiteral("/missing.vdf") }, options);
    QSignalSpy started(&tracker, &LaunchTracker::appStarted);
    QSignalSpy finished(&tracker, &LaunchTracker::appFinished);
    tracker.start();
    // The registry alone is not enough (H2): no live process agrees yet.
    write(reg, registryVdf("620"));
    QTest::qWait(250);
    QCOMPARE(started.count(), 0);
    // The game's process appears: both agree.
    write(proc + QStringLiteral("/42/environ"), QByteArray("SteamGameId=620\0", 16));
    QTRY_COMPARE(started.count(), 1);
    QVERIFY(tracker.seen());
    // Process gone but the Steam client (steam.pid) still alive and the
    // registry still says running: running.
    QFile::remove(proc + QStringLiteral("/42/environ"));
    write(tmp.path() + QStringLiteral("/steam.pid"), "77\n");
    write(proc + QStringLiteral("/77/comm"), "steam\n");
    QTest::qWait(250);
    QCOMPARE(finished.count(), 0);
    // The registry says it ended: finished.
    write(reg, registryVdf("0"));
    QTRY_COMPARE(finished.count(), 1);
}

void TestSteam::trackerIgnoresStaleRegistry()
{
    // A registry left at "running" by a crashed session doesn't count
    // until it changes after the launch (H2 baseline).
    QTemporaryDir tmp;
    const QString reg = tmp.path() + QStringLiteral("/registry.vdf");
    const QString proc = tmp.path() + QStringLiteral("/proc");
    write(reg, registryVdf("620"));
    write(tmp.path() + QStringLiteral("/steam.pid"), "77");
    write(proc + QStringLiteral("/77/comm"), "steamwebhelper");   // "steam*": alive
    SteamRunTracker::Options options;
    options.pollMs = 50;
    options.startTimeoutMs = 5000;
    options.procRoot = proc;
    options.pidFiles = { tmp.path() + QStringLiteral("/steam.pid") };
    SteamRunTracker tracker(QStringLiteral("620"), { reg }, options);
    QSignalSpy started(&tracker, &LaunchTracker::appStarted);
    tracker.start();
    QTest::qWait(250);
    QCOMPARE(started.count(), 0);
    write(reg, registryVdf("620", "\"apps\" { \"620\" { \"Running\" \"1\" } }"));   // Steam rewrote it
    QTRY_COMPARE(started.count(), 1);
    // A pid file naming a process that isn't Steam doesn't count.
    write(proc + QStringLiteral("/77/comm"), "bash");
    QVERIFY(!SteamLibrary::steamClientAlive(proc, options.pidFiles));
    write(tmp.path() + QStringLiteral("/steam.pid"), "77; rm -rf");
    QVERIFY(!SteamLibrary::steamClientAlive(proc, options.pidFiles));
}

void TestSteam::trackerTimeout()
{
    QTemporaryDir tmp;
    SteamRunTracker::Options options;
    options.pollMs = 50;
    options.startTimeoutMs = 200;
    options.procRoot = tmp.path();
    SteamRunTracker tracker(QStringLiteral("620"), {}, options);
    QSignalSpy finished(&tracker, &LaunchTracker::appFinished);
    tracker.start();
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(!tracker.seen());
}

// `steam steam://...` exits at once (hand-over): the launch stays running
// until the tracker says the game ended, or Guide (stopTracking) ends it.
void TestSteam::launcherHandOver()
{
    QTemporaryDir tmp;
    const QString reg = tmp.path() + QStringLiteral("/registry.vdf");
    const QString proc = tmp.path() + QStringLiteral("/proc");
    write(reg, registryVdf("0"));
    SteamRunTracker::Options options;
    options.pollMs = 50;
    options.procRoot = proc;
    Launcher launcher;
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("steam:620"), { QStringLiteral("true") },
                            new SteamRunTracker(QStringLiteral("620"), { reg }, options)));
    QVERIFY(launcher.isWaiting());
    QVERIFY(launcher.isTracked());
    QTest::qWait(300);                       // `true` has exited by now
    QVERIFY(launcher.isRunning());
    QCOMPARE(exited.count(), 0);
    write(reg, registryVdf("620"));
    write(proc + QStringLiteral("/42/environ"), QByteArray("SteamAppId=620\0", 15));
    QTRY_VERIFY(!launcher.isWaiting());
    write(reg, registryVdf("0"));
    QTRY_COMPARE(exited.count(), 1);
    QCOMPARE(exited.first().at(1).toInt(), 0);
    QVERIFY(!launcher.isRunning());

    // A failing launcher process before the game showed up is a failure.
    QFile::remove(proc + QStringLiteral("/42/environ"));
    QSignalSpy failedExit(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("steam:620"), { QStringLiteral("false") },
                            new SteamRunTracker(QStringLiteral("620"), { reg }, options)));
    QTRY_COMPARE(failedExit.count(), 1);
    QCOMPARE(failedExit.first().at(1).toInt(), 1);

    // H2: Guide ends a tracked launch even after the game was seen (the
    // heuristics may never see it end).
    QSignalSpy stopped(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("steam:620"), { QStringLiteral("sleep"), QStringLiteral("5") },
                            new SteamRunTracker(QStringLiteral("620"), { reg }, options)));
    QTest::qWait(150);   // started: the tracker has taken its registry baseline
    write(reg, registryVdf("620", "\"x\" \"1\""));
    write(proc + QStringLiteral("/43/environ"), QByteArray("SteamAppId=620\0", 15));
    QTRY_VERIFY(!launcher.isWaiting());
    launcher.stopTracking();
    QCOMPARE(stopped.count(), 1);
    QVERIFY(!launcher.isRunning());   // `sleep` keeps running in the background, killed with the Launcher
    // Untracked launches are not affected.
    QVERIFY(launcher.launch(QStringLiteral("x"), { QStringLiteral("sleep"), QStringLiteral("1") }));
    QVERIFY(!launcher.isTracked());
    launcher.stopTracking();
    QVERIFY(launcher.isRunning());
    QTRY_VERIFY_WITH_TIMEOUT(!launcher.isRunning(), 5000);
}

QTEST_GUILESS_MAIN(TestSteam)
#include "tst_steam.moc"
