#include "appregistry.h"
#include "appscanner.h"
#include "envguard.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QMutex>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QElapsedTimer>

#include <atomic>
#include <sys/stat.h>

// AppRegistry / AppScanner with temporary XDG trees: desktop-file-id
// precedence, Hidden/NoDisplay/TryExec/OnlyShowIn/NotShowIn, Flatpak ids,
// icons as untrusted input, manifests, async re-scan on file changes.
using namespace Qt::StringLiterals;

class TestAppRegistry : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void precedenceAndHidden();
    void filters();
    void subdirectoryIds();
    void flatpakIds();
    void categories();
    void iconsAreValidated();
    void oversizedAndBrokenFiles();
    void manifests();
    void manifestValidation_data();
    void manifestValidation();
    void environmentConfig();
    void asyncScanAndWatcher();
    void watchesMissingDirsParent();
    void logsNeverContainArgv();
    void fifoDoesNotHangTheScan();
    void cancelledScanStops();
    void startupCache();
    void startupCacheIsValidated();
    void sortUsesSortKey();

private:
    void write(const QString &path, const QByteArray &data);
    void writeDesktop(const QString &dir, const QString &name, const QByteArray &body);
    AppScanConfig config() const;
    static const AppInfo *find(const AppScanResult &result, const QString &id);

    std::unique_ptr<QTemporaryDir> m_tmp;
    QString m_home, m_sys, m_flatpak, m_userManifests, m_sysManifests;
};

void TestAppRegistry::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());
    m_home = m_tmp->path() + QStringLiteral("/home/applications");
    m_sys = m_tmp->path() + QStringLiteral("/sys/applications");
    m_flatpak = m_tmp->path() + QStringLiteral("/flatpak/exports/share/applications");
    m_userManifests = m_tmp->path() + QStringLiteral("/home/mightyos/apps");
    m_sysManifests = m_tmp->path() + QStringLiteral("/sysmanifests");
    for (const QString &dir : { m_home, m_sys, m_userManifests, m_sysManifests })
        QVERIFY(QDir().mkpath(dir));
}

void TestAppRegistry::write(const QString &path, const QByteArray &data)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(data);
}

void TestAppRegistry::writeDesktop(const QString &dir, const QString &name, const QByteArray &body)
{
    write(dir + QLatin1Char('/') + name, QByteArray("[Desktop Entry]\nType=Application\n") + body);
}

AppScanConfig TestAppRegistry::config() const
{
    AppScanConfig c;
    c.applicationDirs = { m_home, m_flatpak, m_sys };
    c.flatpakApplicationDirs = { m_flatpak };
    c.manifestDirs = { m_userManifests, m_sysManifests };
    c.currentDesktops = { QStringLiteral("MightyOS") };
    c.path = QStringLiteral("/usr/bin:/bin");
    c.locale = QStringLiteral("en_US");
    return c;
}

const AppInfo *TestAppRegistry::find(const AppScanResult &result, const QString &id)
{
    for (const AppInfo &app : result.apps) {
        if (app.id == id)
            return &app;
    }
    return nullptr;
}

void TestAppRegistry::precedenceAndHidden()
{
    writeDesktop(m_home, QStringLiteral("foo.desktop"), "Name=Home Foo\nExec=true --home\n");
    writeDesktop(m_sys, QStringLiteral("foo.desktop"), "Name=System Foo\nExec=true --system\n");
    // A user override with Hidden=true deletes the system entry.
    writeDesktop(m_home, QStringLiteral("bar.desktop"), "Name=Bar\nExec=true\nHidden=true\n");
    writeDesktop(m_sys, QStringLiteral("bar.desktop"), "Name=Bar\nExec=true\n");
    // So does NoDisplay=true.
    writeDesktop(m_home, QStringLiteral("nd.desktop"), "Name=ND\nExec=true\nNoDisplay=true\n");
    writeDesktop(m_sys, QStringLiteral("nd.desktop"), "Name=ND\nExec=true\n");
    // And a broken user file: the id is taken by the higher-precedence dir.
    write(m_home + QStringLiteral("/broken.desktop"), "not a desktop file");
    writeDesktop(m_sys, QStringLiteral("broken.desktop"), "Name=Broken\nExec=true\n");
    writeDesktop(m_sys, QStringLiteral("only-system.desktop"), "Name=Only System\nExec=true\n");

    const AppScanResult result = AppScanner::scan(config());
    const AppInfo *foo = find(result, QStringLiteral("desktop:foo"));
    QVERIFY(foo);
    QCOMPARE(foo->title, QStringLiteral("Home Foo"));
    QCOMPARE(foo->argv, (QStringList { u"true"_s, u"--home"_s }));
    QVERIFY(!find(result, QStringLiteral("desktop:bar")));
    QVERIFY(!find(result, QStringLiteral("desktop:nd")));
    QVERIFY(!find(result, QStringLiteral("desktop:broken")));
    QVERIFY(find(result, QStringLiteral("desktop:only-system")));
    QCOMPARE(result.apps.size(), 2);
}

void TestAppRegistry::filters()
{
    writeDesktop(m_sys, QStringLiteral("link.desktop"), "Name=Link\nExec=true\n");
    write(m_sys + QStringLiteral("/link.desktop"), "[Desktop Entry]\nType=Link\nName=L\nURL=https://x\n");
    writeDesktop(m_sys, QStringLiteral("term.desktop"), "Name=Term\nExec=true\nTerminal=true\n");
    writeDesktop(m_sys, QStringLiteral("tryexec-missing.desktop"),
                 "Name=T\nExec=true\nTryExec=mightyos-no-such-binary\n");
    writeDesktop(m_sys, QStringLiteral("tryexec-ok.desktop"), "Name=T ok\nExec=true\nTryExec=true\n");
    writeDesktop(m_sys, QStringLiteral("only-gnome.desktop"), "Name=G\nExec=true\nOnlyShowIn=GNOME;KDE;\n");
    writeDesktop(m_sys, QStringLiteral("only-us.desktop"), "Name=Us\nExec=true\nOnlyShowIn=GNOME;MightyOS;\n");
    writeDesktop(m_sys, QStringLiteral("not-us.desktop"), "Name=NotUs\nExec=true\nNotShowIn=MightyOS;\n");
    writeDesktop(m_sys, QStringLiteral("not-kde.desktop"), "Name=NotKde\nExec=true\nNotShowIn=KDE;\n");
    writeDesktop(m_sys, QStringLiteral("shell.desktop"), "Name=Shell\nExec=true | rm -rf ~\n");
    writeDesktop(m_sys, QStringLiteral("missing-program.desktop"), "Name=Missing\nExec=mightyos-no-such-binary\n");
    writeDesktop(m_sys, QStringLiteral("no-exec.desktop"), "Name=NoExec\nDBusActivatable=true\n");
    writeDesktop(m_sys, QStringLiteral("no-name.desktop"), "Exec=true\n");

    const AppScanResult result = AppScanner::scan(config());
    QStringList ids;
    for (const AppInfo &app : result.apps)
        ids << app.id;
    ids.sort();
    QCOMPARE(ids, (QStringList { u"desktop:not-kde"_s, u"desktop:only-us"_s, u"desktop:tryexec-ok"_s }));
}

void TestAppRegistry::subdirectoryIds()
{
    writeDesktop(m_sys + QStringLiteral("/kde"), QStringLiteral("konsole.desktop"), "Name=K\nExec=true\n");
    // A file directly named kde-konsole.desktop in a lower dir is shadowed
    // by the subdir file of a higher one (same desktop-file id).
    writeDesktop(m_home + QStringLiteral("/kde"), QStringLiteral("konsole.desktop"), "Name=Home K\nExec=true\n");
    const AppScanResult result = AppScanner::scan(config());
    const AppInfo *app = find(result, QStringLiteral("desktop:kde-konsole"));
    QVERIFY(app);
    QCOMPARE(app->title, QStringLiteral("Home K"));
}

void TestAppRegistry::flatpakIds()
{
    writeDesktop(m_flatpak, QStringLiteral("org.example.Player.desktop"),
                 "Name=Player\nExec=true run org.example.Player\nCategories=AudioVideo;\n");
    // Inside an export dir, X-Flatpak= names the app.
    writeDesktop(m_flatpak, QStringLiteral("renamed.desktop"), "Name=Renamed\nExec=true\nX-Flatpak=com.example.Real\n");
    // Outside, it is only a claim: an impostor in ~/.local/share/applications
    // must not become flatpak:<id> (and inherit its favorites/history).
    writeDesktop(m_home, QStringLiteral("impostor.desktop"), "Name=Impostor\nExec=true\nX-Flatpak=org.example.Player\n");
    writeDesktop(m_sys, QStringLiteral("viaKey.desktop"), "Name=Via key\nExec=true\nX-Flatpak=com.example.ViaKey\n");
    writeDesktop(m_flatpak, QStringLiteral("badKey.desktop"), "Name=Bad key\nExec=true\nX-Flatpak=../../etc\n");
    const AppScanResult result = AppScanner::scan(config());
    const AppInfo *player = find(result, QStringLiteral("flatpak:org.example.Player"));
    QVERIFY(player);
    QCOMPARE(player->title, QStringLiteral("Player"));                  // the real one
    QVERIFY(find(result, QStringLiteral("flatpak:com.example.Real")));
    QVERIFY(find(result, QStringLiteral("desktop:impostor")));
    QVERIFY(find(result, QStringLiteral("desktop:viaKey")));
    QVERIFY(!find(result, QStringLiteral("flatpak:com.example.ViaKey")));
    QVERIFY(find(result, QStringLiteral("desktop:badKey")));   // neither a valid key nor a valid file id
}

void TestAppRegistry::categories()
{
    QCOMPARE(AppScanner::categoryFor({ u"AudioVideo"_s, u"Player"_s }), QStringLiteral("media"));
    QCOMPARE(AppScanner::categoryFor({ u"Audio"_s }), QStringLiteral("media"));
    QCOMPARE(AppScanner::categoryFor({ u"Video"_s }), QStringLiteral("media"));
    QCOMPARE(AppScanner::categoryFor({ u"TV"_s }), QStringLiteral("media"));
    QCOMPARE(AppScanner::categoryFor({ u"Game"_s, u"ArcadeGame"_s }), QStringLiteral("games"));
    QCOMPARE(AppScanner::categoryFor({ u"Utility"_s }), QStringLiteral("apps"));
    QCOMPARE(AppScanner::categoryFor({}), QStringLiteral("apps"));
}

void TestAppRegistry::iconsAreValidated()
{
    const QString png = m_tmp->path() + QStringLiteral("/icon.png");
    write(png, QByteArray("\x89PNG\r\n\x1a\n", 8));
    QCOMPARE(AppScanner::artworkFor(png, nullptr), QStringLiteral("file:") + png);
    QCOMPARE(AppScanner::artworkFor(QStringLiteral("vlc"), nullptr), QStringLiteral("theme:vlc"));
    QCOMPARE(AppScanner::artworkFor(QStringLiteral("vlc.png"), nullptr), QStringLiteral("theme:vlc"));
    QCOMPARE(AppScanner::artworkFor(QStringLiteral("../../../etc/passwd"), nullptr), QString());
    QCOMPARE(AppScanner::artworkFor(QStringLiteral("icons/foo.png"), nullptr), QString());
    QCOMPARE(AppScanner::artworkFor(QStringLiteral(".hidden"), nullptr), QString());
    QCOMPARE(AppScanner::artworkFor(m_tmp->path() + QStringLiteral("/../x/icon.png"), nullptr), QString());
    QCOMPARE(AppScanner::artworkFor(QStringLiteral("/etc/passwd"), nullptr), QString());   // not an image type
    QCOMPARE(AppScanner::artworkFor(m_tmp->path() + QStringLiteral("/missing.png"), nullptr), QString());
}

void TestAppRegistry::oversizedAndBrokenFiles()
{
    writeDesktop(m_sys, QStringLiteral("big.desktop"),
                 "Name=Big\nExec=true\n" + QByteArray(DesktopEntry::kMaxFileBytes, '#'));
    write(m_sys + QStringLiteral("/binary.desktop"), QByteArray("\x00\xff\xfe garbage", 12));
    write(m_sys + QStringLiteral("/bad name!.desktop"), "[Desktop Entry]\nType=Application\nName=x\nExec=true\n");
    const AppScanResult result = AppScanner::scan(config());
    QVERIFY(result.apps.isEmpty());
    QVERIFY(result.skipped >= 3);
}

void TestAppRegistry::manifests()
{
    write(m_userManifests + QStringLiteral("/kodi.json"),
          R"json({ "version": 1, "id": "kodi", "name": "Kodi (user)", "exec": ["true", "--user"],
               "category": "media", "subtitle": "Local media", "icon": "kodi" })json");
    write(m_sysManifests + QStringLiteral("/kodi.json"),
          R"json({ "version": 1, "id": "kodi", "name": "Kodi", "exec": ["true"], "category": "media" })json");
    write(m_sysManifests + QStringLiteral("/moonlight.json"),
          R"json({ "version": 1, "id": "moonlight", "name": "Moonlight", "exec": ["true", "a b", "$HOME"] })json");
    const AppScanResult result = AppScanner::scan(config());
    const AppInfo *kodi = find(result, QStringLiteral("app:kodi"));
    QVERIFY(kodi);
    QCOMPARE(kodi->title, QStringLiteral("Kodi (user)"));       // user dir wins
    QCOMPARE(kodi->category, QStringLiteral("media"));
    QCOMPARE(kodi->subtitle, QStringLiteral("Local media"));
    QCOMPARE(kodi->artwork, QStringLiteral("theme:kodi"));
    const AppInfo *moonlight = find(result, QStringLiteral("app:moonlight"));
    QVERIFY(moonlight);
    // Arguments are passed verbatim: no shell, so "$HOME" stays literal.
    QCOMPARE(moonlight->argv, (QStringList { u"true"_s, u"a b"_s, u"$HOME"_s }));
    QCOMPARE(moonlight->category, QStringLiteral("apps"));
}

void TestAppRegistry::manifestValidation_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<bool>("valid");
    QTest::newRow("valid") << QByteArray(R"json({"version":1,"id":"ok","name":"Ok","exec":["true"]})json") << true;
    QTest::newRow("exec is a string") << QByteArray(R"json({"version":1,"id":"x","name":"X","exec":"true --flag"})json") << false;
    QTest::newRow("exec missing") << QByteArray(R"json({"version":1,"id":"x","name":"X"})json") << false;
    QTest::newRow("exec empty") << QByteArray(R"json({"version":1,"id":"x","name":"X","exec":[]})json") << false;
    QTest::newRow("exec non-string") << QByteArray(R"json({"version":1,"id":"x","name":"X","exec":["true",1]})json") << false;
    QTest::newRow("relative program") << QByteArray(R"json({"version":1,"id":"x","name":"X","exec":["bin/true"]})json") << false;
    QTest::newRow("unclean program path") << QByteArray(R"json({"version":1,"id":"x","name":"X","exec":["/usr/bin/../bin/true"]})json") << false;
    QTest::newRow("missing program") << QByteArray(R"json({"version":1,"id":"x","name":"X","exec":["mightyos-no-such-binary"]})json") << false;
    QTest::newRow("id traversal") << QByteArray(R"json({"version":1,"id":"../x","name":"X","exec":["true"]})json") << false;
    QTest::newRow("id with slash") << QByteArray(R"json({"version":1,"id":"a/b","name":"X","exec":["true"]})json") << false;
    QTest::newRow("no name") << QByteArray(R"json({"version":1,"id":"x","exec":["true"]})json") << false;
    QTest::newRow("bad category") << QByteArray(R"json({"version":1,"id":"x","name":"X","exec":["true"],"category":"system"})json") << false;
    QTest::newRow("no version") << QByteArray(R"json({"id":"x","name":"X","exec":["true"]})json") << false;
    QTest::newRow("not json") << QByteArray("exec=true") << false;
    QTest::newRow("too large") << (QByteArray(R"json({"version":1,"id":"x","name":"X","exec":["true"],"pad":")json")
                                   + QByteArray(AppScanner::kMaxManifestBytes, 'a') + "\"}") << false;
}

void TestAppRegistry::manifestValidation()
{
    QFETCH(QByteArray, json);
    QFETCH(bool, valid);
    QString why;
    const auto app = AppScanner::fromManifest(json, QStringLiteral("/m.json"), config(), &why);
    QVERIFY2(app.has_value() == valid, qPrintable(why));
}

void TestAppRegistry::environmentConfig()
{
    const EnvGuard home("HOME", m_tmp->path().toUtf8());
    const EnvGuard dataHome("XDG_DATA_HOME", (m_tmp->path() + QStringLiteral("/data")).toUtf8());
    const EnvGuard dataDirs("XDG_DATA_DIRS", QByteArray("/opt/a:relative/ignored:/opt/b:/opt/a"));
    const EnvGuard desktop("XDG_CURRENT_DESKTOP", QByteArray("X-Test:MightyOS"));
    const AppScanConfig c = AppScanConfig::fromEnvironment(QStringLiteral("/sys/m"));
    QCOMPARE(c.applicationDirs, (QStringList {
        m_tmp->path() + QStringLiteral("/data/applications"),
        m_tmp->path() + QStringLiteral("/.local/share/flatpak/exports/share/applications"),
        QStringLiteral("/var/lib/flatpak/exports/share/applications"),
        QStringLiteral("/opt/a/applications"),
        QStringLiteral("/opt/b/applications") }));
    QVERIFY(c.flatpakApplicationDirs.contains(QStringLiteral("/var/lib/flatpak/exports/share/applications")));
    QCOMPARE(c.manifestDirs, (QStringList { m_tmp->path() + QStringLiteral("/data/mightyos/apps"), u"/sys/m"_s }));
    QCOMPARE(c.currentDesktops, (QStringList { u"X-Test"_s, u"MightyOS"_s }));
    QVERIFY(!c.path.isEmpty());
}

void TestAppRegistry::asyncScanAndWatcher()
{
    writeDesktop(m_sys, QStringLiteral("player.desktop"), "Name=Player\nExec=true %U\nCategories=AudioVideo;\n");
    writeDesktop(m_sys, QStringLiteral("editor.desktop"), "Name=Editor\nExec=true\nCategories=Utility;\n");
    AppRegistry registry(config(), false);
    QVERIFY(!registry.isReady());
    QSignalSpy finished(&registry, &AppRegistry::scanFinished);
    registry.rescan();
    QVERIFY(finished.wait(5000));
    QVERIFY(registry.isReady());
    QCOMPARE(registry.count(), 2);
    QCOMPARE(registry.apps()->count(), 2);
    QCOMPARE(registry.apps()->keyAt(0), QStringLiteral("desktop:editor"));   // sorted by title
    QCOMPARE(registry.media()->count(), 1);
    QCOMPARE(registry.media()->keyAt(0), QStringLiteral("desktop:player"));
    QCOMPARE(registry.command(QStringLiteral("desktop:player")), QStringList { u"true"_s });
    QVERIFY(registry.command(QStringLiteral("desktop:nope")).isEmpty());
    const auto tile = registry.resolve(QStringLiteral("desktop:player"));
    QVERIFY(tile);
    QCOMPARE(tile->kind, QStringLiteral("app"));
    QVERIFY(registry.watchedPaths().contains(m_sys));

    // Installing an app: the watcher triggers a debounced re-scan.
    TileListModel *media = registry.media();
    writeDesktop(m_home, QStringLiteral("tv.desktop"), "Name=TV\nExec=true\nCategories=TV;\n");
    QTRY_COMPARE_WITH_TIMEOUT(registry.count(), 3, 5000);
    QCOMPARE(registry.media(), media);                    // same model object
    QCOMPARE(media->count(), 2);
    // Removing one.
    QVERIFY(QFile::remove(m_sys + QStringLiteral("/editor.desktop")));
    QTRY_COMPARE_WITH_TIMEOUT(registry.count(), 2, 5000);
    QVERIFY(!registry.contains(QStringLiteral("desktop:editor")));
}

void TestAppRegistry::watchesMissingDirsParent()
{
    // The Flatpak export dir doesn't exist yet (no Flatpak installed); the
    // first install creates it, which must still be noticed.
    QVERIFY(QDir().mkpath(m_tmp->path() + QStringLiteral("/flatpak")));   // like ~/.local/share/flatpak
    AppRegistry registry(config(), false);
    QSignalSpy finished(&registry, &AppRegistry::scanFinished);
    registry.rescan();
    QVERIFY(finished.wait(5000));
    QCOMPARE(registry.count(), 0);
    QVERIFY(registry.watchedPaths().contains(m_tmp->path() + QStringLiteral("/flatpak")));
    writeDesktop(m_flatpak, QStringLiteral("org.example.New.desktop"), "Name=New\nExec=true\n");
    QTRY_COMPARE_WITH_TIMEOUT(registry.count(), 1, 5000);
    QVERIFY(registry.contains(QStringLiteral("flatpak:org.example.New")));
}

namespace {
// The scan logs from a worker thread while the test thread may log too.
QMutex g_logMutex;
QStringList *g_log = nullptr;
void captureLog(QtMsgType, const QMessageLogContext &, const QString &message)
{
    QMutexLocker lock(&g_logMutex);
    if (g_log)
        g_log->append(message);
}
} // namespace

void TestAppRegistry::logsNeverContainArgv()
{
    writeDesktop(m_sys, QStringLiteral("ok.desktop"), "Name=Ok\nExec=true --token=SECRET-OK %U\n");
    writeDesktop(m_sys, QStringLiteral("bad.desktop"), "Name=Bad\nExec=true --token=SECRET-BAD | x\n");
    write(m_userManifests + QStringLiteral("/m.json"),
          R"json({"version":1,"id":"m","name":"M","exec":["true","--token=SECRET-MANIFEST"]})json");
    QStringList log;
    g_log = &log;
    QLoggingCategory::setFilterRules(QStringLiteral("mightyos.*=true"));
    const QtMessageHandler previous = qInstallMessageHandler(captureLog);
    {
        AppRegistry registry(config(), false);
        QSignalSpy finished(&registry, &AppRegistry::scanFinished);
        registry.rescan();
        QVERIFY(finished.wait(5000));
        QCOMPARE(registry.count(), 2);
    }
    qInstallMessageHandler(previous);
    QLoggingCategory::setFilterRules(QString());
    {
        QMutexLocker lock(&g_logMutex);
        g_log = nullptr;
    }
    QVERIFY(!log.isEmpty());                              // debug output did happen
    for (const QString &line : std::as_const(log))
        QVERIFY2(!line.contains(QStringLiteral("SECRET")), qPrintable(line));
}

void TestAppRegistry::fifoDoesNotHangTheScan()
{
    // A FIFO named like a .desktop file / manifest: opening it blocking would
    // hang the scan forever (and then the quit, which waits for the scan).
    writeDesktop(m_sys, QStringLiteral("ok.desktop"), "Name=Ok\nExec=true\n");
    QVERIFY(::mkfifo(QFile::encodeName(m_sys + QStringLiteral("/fifo.desktop")).constData(), 0600) == 0);
    QVERIFY(::mkfifo(QFile::encodeName(m_userManifests + QStringLiteral("/fifo.json")).constData(), 0600) == 0);
    // A symlinked manifest is not followed (only app dirs follow links).
    write(m_tmp->path() + QStringLiteral("/elsewhere.json"),
          R"json({"version":1,"id":"linked","name":"Linked","exec":["true"]})json");
    QVERIFY(QFile::link(m_tmp->path() + QStringLiteral("/elsewhere.json"), m_userManifests + QStringLiteral("/linked.json")));
    QElapsedTimer timer;
    timer.start();
    const AppScanResult result = AppScanner::scan(config());
    QVERIFY(timer.elapsed() < 5000);
    QCOMPARE(result.apps.size(), 1);
    QVERIFY(find(result, QStringLiteral("desktop:ok")));
}

void TestAppRegistry::cancelledScanStops()
{
    for (int i = 0; i < 50; ++i)
        writeDesktop(m_sys, QStringLiteral("a%1.desktop").arg(i), "Name=A\nExec=true\n");
    std::atomic_bool cancel { true };
    const AppScanResult result = AppScanner::scan(config(), &cancel);
    QVERIFY(result.apps.isEmpty());
    // The destructor cancels a running scan and returns promptly.
    QElapsedTimer timer;
    timer.start();
    {
        AppRegistry registry(config(), false);
        registry.rescan();
    }
    QVERIFY(timer.elapsed() < 5000);
}

void TestAppRegistry::startupCache()
{
    const QString cache = m_tmp->path() + QStringLiteral("/cache/apps.json");
    writeDesktop(m_sys, QStringLiteral("player.desktop"), "Name=Player\nExec=true\nCategories=Video;\n");
    {
        AppRegistry registry(config(), false, nullptr, cache);
        QCOMPARE(registry.count(), 0);                          // no cache yet
        QSignalSpy finished(&registry, &AppRegistry::scanFinished);
        registry.rescan();
        QVERIFY(finished.wait(5000));
        QVERIFY(QFileInfo::exists(cache));                       // written after the scan
        QCOMPARE(QFileInfo(cache).permissions() & (QFile::ReadOther | QFile::ReadGroup), QFile::Permissions());
    }
    // Next start: the apps are there synchronously, before any scan.
    QVERIFY(QFile::remove(m_sys + QStringLiteral("/player.desktop")));
    AppRegistry registry(config(), false, nullptr, cache);
    QVERIFY(registry.contains(QStringLiteral("desktop:player")));
    QCOMPARE(registry.media()->count(), 1);
    QCOMPARE(registry.command(QStringLiteral("desktop:player")), QStringList { u"true"_s });
    QVERIFY(!registry.isReady());                                // not a real scan
    QVERIFY(registry.isFromCache());
    // The real scan replaces the cached content (the app was uninstalled).
    QSignalSpy ready(&registry, &AppRegistry::readyChanged);
    registry.rescan();
    QVERIFY(ready.wait(5000));
    QVERIFY(registry.isReady());
    QVERIFY(!registry.isFromCache());
    QCOMPARE(registry.count(), 0);
    QCOMPARE(AppRegistry::readCache(cache)->size(), 0);          // cache updated
}

void TestAppRegistry::startupCacheIsValidated()
{
    const QString cache = m_tmp->path() + QStringLiteral("/apps.json");
    write(cache, R"json({"version":1,"apps":[
        {"id":"desktop:ok","title":"Ok","category":"apps","argv":["true"]},
        {"id":"desktop:../x","title":"Traversal","category":"apps","argv":["true"]},
        {"id":"flatpak:nodots","title":"Bad flatpak id","category":"apps","argv":["true"]},
        {"id":"evil:x","title":"Unknown prefix","category":"apps","argv":["true"]},
        {"id":"app:shell","title":"Shell string","category":"apps","argv":"true --x"},
        {"id":"app:rel","title":"Relative","category":"apps","argv":["bin/true"]},
        {"id":"app:art","title":"Bad art","category":"apps","argv":["true"],"artwork":"theme:../x"},
        {"id":"app:cat","title":"Bad category","category":"system","argv":["true"]},
        {"id":"app:notitle","category":"apps","argv":["true"]}
    ]})json");
    const auto apps = AppRegistry::readCache(cache);
    QVERIFY(apps);
    QCOMPARE(apps->size(), 1);
    QCOMPARE(apps->first().id, QStringLiteral("desktop:ok"));
    write(cache, "{\"version\":2,\"apps\":[]}");
    QVERIFY(!AppRegistry::readCache(cache));
    write(cache, "garbage");
    QVERIFY(!AppRegistry::readCache(cache));
}

void TestAppRegistry::sortUsesSortKey()
{
    // Same ordering as TileListModel's letter jump.
    writeDesktop(m_sys, QStringLiteral("z.desktop"), "Name=zeta\nExec=true\n");
    writeDesktop(m_sys, QStringLiteral("e.desktop"), "Name=\xc3\x89" "chec\nExec=true\n");
    writeDesktop(m_sys, QStringLiteral("b.desktop"), "Name=[Beta] X\nExec=true\n");
    writeDesktop(m_sys, QStringLiteral("a.desktop"), "Name=_Alpha\nExec=true\n");
    const AppScanResult result = AppScanner::scan(config());
    QStringList ids;
    for (const AppInfo &app : result.apps)
        ids << app.id;
    QCOMPARE(ids, (QStringList { u"desktop:a"_s, u"desktop:b"_s, u"desktop:e"_s, u"desktop:z"_s }));
}

QTEST_GUILESS_MAIN(TestAppRegistry)
#include "tst_appregistry.moc"
