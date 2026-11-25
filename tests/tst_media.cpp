#include "appregistry.h"
#include "itemresolver.h"
#include "launcher.h"
#include "library.h"
#include "medialibrary.h"
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

// Home > Media (M5): media apps from AppRegistry, web apps, Moonlight
// hosts/apps, kids allow-lists, adding/hiding tiles, launching and the
// favorites resolver.
class TestMedia : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void modelsAndOrder();
    void addHideAndRemoveWebApps();
    void moonlightHostsAndStreams();
    void kidsSeeOnlyAllowed();
    void resolverCoversNewKinds();
    void launchAndCloseWebApp();
    void removingAnOverrideHidesTheBuiltin();
    void watcherIgnoresLockFiles();
    void deletingAProfileDeletesBrowserData();

private:
    void write(const QString &rel, const QByteArray &data, bool executable = false)
    {
        const QString path = m_tmp->path() + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
        f.close();
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner
                                        | (executable ? QFile::ExeOwner : QFile::Permissions()));
    }
    QString p(const QString &rel) const { return m_tmp->path() + QLatin1Char('/') + rel; }
    std::unique_ptr<MediaLibrary> make(bool watch = false);
    void allow(const QString &profile, const QJsonArray &apps);

    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<ProfileService> m_profiles;
    std::unique_ptr<Launcher> m_launcher;
    std::unique_ptr<AppRegistry> m_apps;
    std::unique_ptr<Library> m_library;
};

static const QByteArray kMoonlight = R"ini([hosts]
1\apps\1\name=Desktop
1\apps\2\name=Steam Big Picture
1\apps\3\hidden=true
1\apps\3\name=Hidden
1\apps\size=3
1\hostname=GAMING-PC
1\uuid=8D8C1F3A-2B7E-4C11-9A51-0123456789AB
size=1
)ini";

void TestMedia::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    const QString h = p(QStringLiteral("home"));
    m_profiles = std::make_unique<ProfileService>(h + QStringLiteral("/.config/mightyos"),
                                                  h + QStringLiteral("/.local/state/mightyos"),
                                                  h + QStringLiteral("/.local/share/mightyos"), h);
    m_launcher = std::make_unique<Launcher>();
    // Kodi (Flatpak export), Moonlight (Flatpak, category Game) and VLC (native, AudioVideo).
    write(QStringLiteral("flatpak/exports/share/applications/tv.kodi.Kodi.desktop"),
          "[Desktop Entry]\nType=Application\nName=Kodi\nExec=true\nCategories=AudioVideo;\n");
    write(QStringLiteral("flatpak/exports/share/applications/com.moonlight_stream.Moonlight.desktop"),
          "[Desktop Entry]\nType=Application\nName=Moonlight\nExec=true\nCategories=Game;\n");
    write(QStringLiteral("apps/vlc.desktop"),
          "[Desktop Entry]\nType=Application\nName=VLC\nExec=true\nCategories=AudioVideo;Player;\n");
    write(QStringLiteral("apps/editor.desktop"),
          "[Desktop Entry]\nType=Application\nName=Editor\nExec=true\nCategories=Utility;\n");
    AppScanConfig c;
    c.applicationDirs = { p(QStringLiteral("apps")), p(QStringLiteral("flatpak/exports/share/applications")) };
    c.flatpakApplicationDirs = { p(QStringLiteral("flatpak/exports/share/applications")) };
    c.currentDesktops = { QStringLiteral("MightyOS") };
    c.path = QStringLiteral("/usr/bin:/bin");
    m_apps = std::make_unique<AppRegistry>(c, false);
    QSignalSpy scanned(m_apps.get(), &AppRegistry::scanFinished);
    m_apps->rescan();
    QVERIFY(scanned.wait(5000));

    Library::Config lc;
    lc.mock = true;
    lc.autoScan = false;
    lc.watch = false;
    lc.changedDelayMs = 0;
    m_library = std::make_unique<Library>(lc, m_profiles.get(), m_launcher.get(), nullptr);

    // Two web apps of our own (built-ins are covered by tst_webapps), and
    // a Moonlight Flatpak with a paired host.
    write(QStringLiteral("webapps.d/tube.json"), R"({"version": 1, "id": "tube", "label": "Tube", "order": 1,
        "url": "https://www.tube.example/tv", "hosts": ["www.tube.example"]})");
    write(QStringLiteral("webapps.d/flix.json"), R"({"version": 1, "id": "flix", "label": "Flix", "order": 2,
        "url": "https://www.flix.example/", "hosts": ["www.flix.example"], "drm": true})");
    write(QStringLiteral("flatpakinst/app/com.moonlight_stream.Moonlight/current/active/files/.keep"), "");
    write(QStringLiteral("bin/flatpak"), "#!/bin/sh\nexec sleep 60\n", true);
    write(QStringLiteral("home/.var/app/com.moonlight_stream.Moonlight/config/Moonlight Game Streaming Project/Moonlight.conf"),
          kMoonlight);
}

void TestMedia::cleanup()
{
    m_library.reset();
    m_apps.reset();
    m_launcher.reset();
    m_profiles.reset();
    m_tmp.reset();
}

std::unique_ptr<MediaLibrary> TestMedia::make(bool watch)
{
    MediaLibrary::Config c;
    c.webAppDirs = { p(QStringLiteral("webapps.d")), p(QStringLiteral("home/.config/mightyos/webapps.d")) };
    c.configHome = p(QStringLiteral("home/.config"));
    c.env.home = p(QStringLiteral("home"));
    c.env.dataHome = p(QStringLiteral("home/.local/share"));
    c.env.pathDirs = { p(QStringLiteral("bin")) };
    c.env.flatpakInstallations = { p(QStringLiteral("flatpakinst")) };
    c.moonlightFiles = Moonlight::configFiles(c.env.home, c.configHome);
    c.watch = watch;
    auto media = std::make_unique<MediaLibrary>(c, m_apps.get(), m_library.get(), m_profiles.get(), m_launcher.get());
    media->waitForMoonlight();
    return media;
}

void TestMedia::allow(const QString &profile, const QJsonArray &apps)
{
    const QString path = m_profiles->profileConfigDir(profile) + QStringLiteral("/allowed.json");
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(QJsonObject { { QStringLiteral("version"), 1 }, { QStringLiteral("apps"), apps } }).toJson());
}

void TestMedia::modelsAndOrder()
{
    auto media = make();
    QAbstractItemModelTester tester(media->home(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    TileListModel *apps = media->apps();
    // Well-known first (Kodi, Moonlight), other media apps, then the
    // "Get it" tiles of missing well-known apps (not Plexamp).
    QStringList keys;
    for (int i = 0; i < apps->count(); ++i)
        keys.append(apps->keyAt(i));
    QCOMPARE(keys, (QStringList { QStringLiteral("flatpak:tv.kodi.Kodi"), QStringLiteral("flatpak:com.moonlight_stream.Moonlight"),
                                  QStringLiteral("desktop:vlc"), QStringLiteral("getapp:org.jellyfin.JellyfinDesktop"),
                                  QStringLiteral("getapp:tv.plex.PlexHTPC") }));
    QCOMPARE(apps->get(3).value(QStringLiteral("kind")).toString(), QStringLiteral("getapp"));
    QCOMPARE(media->webApps()->count(), 2);
    QCOMPARE(media->webApps()->keyAt(0), QStringLiteral("webapp:tube"));
    QCOMPARE(media->webApps()->get(1).value(QStringLiteral("subtitle")).toString(), QStringLiteral("Needs Google Chrome"));
    QCOMPARE(media->hosts()->count(), 1);
    // Home: apps without "Get it", web apps, hosts.
    QCOMPARE(media->home()->count(), 3 + 2 + 1);
    QCOMPARE(media->home()->indexOfKey(QStringLiteral("getapp:tv.plex.PlexHTPC")), -1);
    QVERIFY(media->browserLabel().isEmpty());
    QVERIFY(media->moonlightInstalled());
    QVERIFY(media->isReady());
}

void TestMedia::addHideAndRemoveWebApps()
{
    auto media = make();
    QVariantMap r = media->addWebApp(QStringLiteral("Jelly"), QStringLiteral("https://jelly.example.org/web/"));
    QVERIFY2(r.value(QStringLiteral("ok")).toBool(), qPrintable(r.value(QStringLiteral("message")).toString()));
    const QString key = r.value(QStringLiteral("key")).toString();
    QCOMPARE(key, QStringLiteral("webapp:user-jelly"));
    const QString file = p(QStringLiteral("home/.config/mightyos/webapps.d/user-jelly.json"));
    QVERIFY(QFileInfo::exists(file));
    QCOMPARE(QFileInfo(file).permissions() & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther),
             QFile::Permissions());
    QVERIFY(media->webApps()->indexOfKey(key) >= 0);
    QVERIFY(media->webAppInfo(key).value(QStringLiteral("user")).toBool());
    QVERIFY(!media->addWebApp(QStringLiteral("Bad"), QStringLiteral("http://x.example/")).value(QStringLiteral("ok")).toBool());
    QVERIFY(!media->addWebApp(QStringLiteral("Bad"), QStringLiteral("https://a@x.example/")).value(QStringLiteral("ok")).toBool());

    // A built-in is hidden (stub), shown again (stub removed).
    QVERIFY(media->removeWebApp(QStringLiteral("webapp:tube")));
    QCOMPARE(media->webApps()->indexOfKey(QStringLiteral("webapp:tube")), -1);
    QVERIFY(media->webAppInfo(QStringLiteral("webapp:tube")).value(QStringLiteral("hidden")).toBool());
    QVERIFY(media->manage()->indexOfKey(QStringLiteral("webapp:tube")) >= 0);
    QVERIFY(!media->resolve(QStringLiteral("webapp:tube")));
    QVERIFY(media->showWebApp(QStringLiteral("webapp:tube")));
    QVERIFY(media->webApps()->indexOfKey(QStringLiteral("webapp:tube")) >= 0);
    // A user tile is deleted.
    QVERIFY(media->removeWebApp(key));
    QVERIFY(!QFileInfo::exists(file));
    QCOMPARE(media->webApps()->indexOfKey(key), -1);
    QVERIFY(!media->removeWebApp(QStringLiteral("webapp:nope")));
    QCOMPARE(media->manage()->keyAt(0), QStringLiteral("addwebapp"));
}

void TestMedia::moonlightHostsAndStreams()
{
    auto media = make();
    const QString host = QStringLiteral("moonlight:8d8c1f3a-2b7e-4c11-9a51-0123456789ab");
    QCOMPARE(media->hosts()->keyAt(0), host);
    QCOMPARE(media->hosts()->get(0).value(QStringLiteral("kind")).toString(), QStringLiteral("streamhost"));
    // M2: the count is the visible apps (hidden one not counted).
    QVERIFY(media->hosts()->get(0).value(QStringLiteral("subtitle")).toString().endsWith(QStringLiteral("· 2 apps")));
    QVERIFY(media->hasHost(host));
    QCOMPARE(media->hostName(host), QStringLiteral("GAMING-PC"));
    TileListModel *streams = media->streams(host);
    QVERIFY(streams);
    QCOMPARE(streams->count(), 2);   // hidden app skipped
    QCOMPARE(streams->get(1).value(QStringLiteral("title")).toString(), QStringLiteral("Steam Big Picture"));
    QCOMPARE(streams->get(0).value(QStringLiteral("kind")).toString(), QStringLiteral("stream"));
    QVERIFY(!media->streams(QStringLiteral("moonlight:other")));
    QVERIFY(!media->streams(streams->keyAt(0)));   // an app key is not a host
    QVERIFY(media->resolve(streams->keyAt(1)));
    QVERIFY(!media->resolve(host + QStringLiteral("/Hidden")));
    QVERIFY(!media->isPending(host));

    // Launching a host tile asks for an app; an app launches the stream.
    QVERIFY(!media->launch(host).value(QStringLiteral("attempted")).toBool());
    QSignalSpy launched(m_launcher.get(), &Launcher::launched);
    QSignalSpy exited(m_launcher.get(), &Launcher::exited);
    QVERIFY(media->launch(streams->keyAt(1)).value(QStringLiteral("ok")).toBool());
    QVERIFY(launched.wait(5000));
    QVERIFY(m_launcher->terminate());
    QVERIFY(exited.wait(5000));
    QCOMPARE(exited.at(0).at(1).toInt(), 0);
}

void TestMedia::kidsSeeOnlyAllowed()
{
    auto media = make();
    const QString kid = m_profiles->create(QStringLiteral("Kid"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"),
                                           QStringLiteral("kids"));
    QVERIFY(m_profiles->select(kid));
    QVERIFY(m_library->isRestricted());
    QCOMPARE(media->home()->count(), 0);   // default deny
    QCOMPARE(media->apps()->count(), 0);   // no "Get it" tiles either
    QCOMPARE(media->webApps()->count(), 0);
    QCOMPARE(media->hosts()->count(), 0);
    QVERIFY(!media->launch(QStringLiteral("webapp:tube")).value(QStringLiteral("attempted")).toBool());
    QVERIFY(!media->addWebApp(QStringLiteral("X"), QStringLiteral("https://x.example/")).value(QStringLiteral("ok")).toBool());
    QVERIFY(!media->removeWebApp(QStringLiteral("webapp:tube")));

    const QString host = QStringLiteral("moonlight:8d8c1f3a-2b7e-4c11-9a51-0123456789ab");
    // L5: the host part matches in any case.
    allow(kid, { QStringLiteral("flatpak:tv.kodi.Kodi"), QStringLiteral("webapp:tube"),
                 QStringLiteral("moonlight:8D8C1F3A-2B7E-4C11-9A51-0123456789AB/Desktop") });
    QVERIFY(m_profiles->select(m_profiles->profiles().first().toMap().value(QStringLiteral("id")).toString()));
    QVERIFY(!m_library->isRestricted());
    QCOMPARE(media->apps()->count(), 5);
    QVERIFY(m_profiles->select(kid));   // re-read
    QCOMPARE(media->apps()->count(), 1);
    QCOMPARE(media->webApps()->count(), 1);
    QCOMPARE(media->hosts()->count(), 1);
    QVERIFY(media->hosts()->get(0).value(QStringLiteral("subtitle")).toString().endsWith(QStringLiteral("· 1 apps")));
    QCOMPARE(media->streams(host)->count(), 1);
    QVERIFY(!media->resolve(host + QStringLiteral("/Steam%20Big%20Picture")));
    QVERIFY(media->resolve(host + QStringLiteral("/Desktop")));
    // S3: a host entry is no wildcard (it would cover Desktop / Big Picture).
    allow(kid, { host });
    QVERIFY(m_profiles->select(m_profiles->profiles().first().toMap().value(QStringLiteral("id")).toString()));
    QVERIFY(m_profiles->select(kid));
    QCOMPARE(media->hosts()->count(), 0);
    QVERIFY(!media->streams(host));
    QVERIFY(!media->resolve(host + QStringLiteral("/Desktop")));
    QVERIFY(!m_library->itemAllowed(QStringLiteral("flatpak:tv.kodi.Kodi")));
}

void TestMedia::resolverCoversNewKinds()
{
    auto media = make();
    CatalogResolver resolver(m_library.get(), m_apps.get(), media.get());
    QCOMPARE(resolver.resolve(QStringLiteral("webapp:tube"))->kind, QStringLiteral("webapp"));
    const QString app = QStringLiteral("moonlight:8d8c1f3a-2b7e-4c11-9a51-0123456789ab/Desktop");
    QCOMPARE(resolver.resolve(app)->kind, QStringLiteral("stream"));
    QVERIFY(!resolver.resolve(QStringLiteral("webapp:gone")));
    QVERIFY(!resolver.resolve(QStringLiteral("moonlight:8d8c1f3a-2b7e-4c11-9a51-0123456789ab/Gone")));
    QVERIFY(resolver.resolve(QStringLiteral("desktop:editor")));
    QSignalSpy changed(&resolver, &ItemResolver::changed);
    media->reload();
    media->waitForMoonlight();
    QVERIFY(changed.count() > 0);
    // Kids: apps outside the allow-list don't resolve (favorites show them unavailable).
    const QString kid = m_profiles->create(QStringLiteral("Kid"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"),
                                           QStringLiteral("kids"));
    QVERIFY(m_profiles->select(kid));
    QVERIFY(!resolver.resolve(QStringLiteral("desktop:editor")));
    QVERIFY(!resolver.resolve(QStringLiteral("webapp:tube")));
}

void TestMedia::launchAndCloseWebApp()
{
    auto media = make();
    // No browser yet: not attempted, install hint.
    QVariantMap r = media->launch(QStringLiteral("webapp:flix"));
    QVERIFY(!r.value(QStringLiteral("attempted")).toBool());
    QCOMPARE(r.value(QStringLiteral("install")).toString(), QStringLiteral("Google Chrome"));
    // A "Chrome Flatpak" (the fake flatpak sleeps): launched, then closed.
    write(QStringLiteral("flatpakinst/app/com.google.Chrome/current/active/files/.keep"), "");
    media->reload();
    QCOMPARE(media->browserLabel(), QStringLiteral("Google Chrome (Flatpak)"));
    QSignalSpy launched(m_launcher.get(), &Launcher::launched);
    QSignalSpy exited(m_launcher.get(), &Launcher::exited);
    r = media->launch(QStringLiteral("webapp:flix"));
    QVERIFY2(r.value(QStringLiteral("ok")).toBool(), qPrintable(r.value(QStringLiteral("message")).toString()));
    QVERIFY(launched.wait(5000));
    QCOMPARE(m_launcher->currentId(), QStringLiteral("webapp:flix"));
    QVERIFY(m_launcher->terminate());
    QVERIFY(exited.wait(5000));
    QCOMPARE(exited.at(0).at(1).toInt(), 0);
    QVERIFY(!m_launcher->isRunning());
}

void TestMedia::removingAnOverrideHidesTheBuiltin()
{
    // L2: a user file replacing a built-in id: "remove" means gone (a stub),
    // not "the built-in comes back".
    write(QStringLiteral("home/.config/mightyos/webapps.d/tube.json"), R"({"version": 1, "id": "tube", "label": "My Tube",
        "url": "https://www.tube.example/", "hosts": ["www.tube.example"]})");
    auto media = make();
    auto title = [&media] {
        return media->webApps()->get(media->webApps()->indexOfKey(QStringLiteral("webapp:tube")))
            .value(QStringLiteral("title")).toString();
    };
    QCOMPARE(title(), QStringLiteral("My Tube"));
    QVERIFY(media->removeWebApp(QStringLiteral("webapp:tube")));
    QCOMPARE(media->webApps()->indexOfKey(QStringLiteral("webapp:tube")), -1);
    QVERIFY(media->webAppInfo(QStringLiteral("webapp:tube")).value(QStringLiteral("hidden")).toBool());
    QVERIFY(media->showWebApp(QStringLiteral("webapp:tube")));   // back to the built-in
    QCOMPARE(title(), QStringLiteral("Tube"));
}

void TestMedia::watcherIgnoresLockFiles()
{
    // L4: lock/temp files next to Moonlight.conf don't reload anything; a
    // changed Moonlight.conf does (once).
    auto media = make(true);
    QSignalSpy changed(media.get(), &MediaLibrary::changed);
    const QString dir = QStringLiteral("home/.var/app/com.moonlight_stream.Moonlight/config/Moonlight Game Streaming Project/");
    write(dir + QStringLiteral("Moonlight.conf.lock"), "1234");
    write(dir + QStringLiteral("Moonlight.conf.aBcDeF"), "temp");
    QTest::qWait(MediaLibrary::kDebounceMs + 400);
    QCOMPARE(changed.count(), 0);
    QByteArray more = kMoonlight;
    more.replace("1\\apps\\size=3", "1\\apps\\4\\name=Third\n1\\apps\\size=4");
    QTest::qWait(20);   // a different mtime
    write(dir + QStringLiteral("Moonlight.conf"), more);
    QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 3000);
    media->waitForMoonlight();
    QCOMPARE(changed.count(), 1);   // one rebuild per reload
    QCOMPARE(media->streams(QStringLiteral("moonlight:8d8c1f3a-2b7e-4c11-9a51-0123456789ab"))->count(), 3);
}

void TestMedia::deletingAProfileDeletesBrowserData()
{
    // S4: browser data (logins) goes with the profile even when its saves
    // stay; the Flatpak's tree is walked without following symlinks.
    const QString id = m_profiles->create(QStringLiteral("Gone"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    QVERIFY(!id.isEmpty());
    write(QStringLiteral("victim.txt"), "precious");
    const QString native = QStringLiteral("home/.local/share/mightyos/profiles/") + id;
    write(native + QStringLiteral("/browsers/chrome/Default/Cookies"), "c");
    write(native + QStringLiteral("/saves/snes/Game.srm"), "save");
    const QString chrome = QStringLiteral("home/.var/app/com.google.Chrome/data/mightyos/profiles/") + id;
    write(chrome + QStringLiteral("/Default/Cookies"), "c");
    QVERIFY(QFile::link(p(QStringLiteral("victim.txt")), p(chrome + QStringLiteral("/Default/evil-link"))));
    write(QStringLiteral("elsewhere/mightyos/profiles/") + id + QStringLiteral("/keep"), "k");
    QDir().mkpath(p(QStringLiteral("home/.var/app/com.brave.Browser")));
    // A sandbox that swapped its data dir for a symlink out of its tree.
    QVERIFY(QFile::link(p(QStringLiteral("elsewhere")), p(QStringLiteral("home/.var/app/com.brave.Browser/data"))));

    QVERIFY(m_profiles->remove(id, true));
    QVERIFY(!QFileInfo::exists(p(native + QStringLiteral("/browsers"))));
    QVERIFY(QFileInfo::exists(p(native + QStringLiteral("/saves/snes/Game.srm"))));   // kept
    QVERIFY(!QFileInfo::exists(p(chrome)));
    QFile victim(p(QStringLiteral("victim.txt")));
    QVERIFY(victim.open(QIODevice::ReadOnly));
    QCOMPARE(victim.readAll(), QByteArray("precious"));
    QVERIFY(QFileInfo::exists(p(QStringLiteral("elsewhere/mightyos/profiles/") + id + QStringLiteral("/keep"))));
}

QTEST_GUILESS_MAIN(TestMedia)
#include "tst_media.moc"
