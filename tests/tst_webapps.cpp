#include "webapps.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

// webapps.d (strict definitions, URL rules, layers and trust checks) and
// the kiosk-browser argv (browser choice, per-profile data dirs).
class TestWebApps : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { ::umask(022); }   // trust checks: no group-writable test dirs
    void builtinsAreValid();
    void urls_data();
    void urls();
    void rejects_data();
    void rejects();
    void layersOverrideHideAndTrust();
    void userDefinitions();
    void browserChoice();
    void planArgv();
    void kidsNetworkLimits();
    void nativeBrowsersOnlyFromTrustedPaths();

private:
    static void write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
        f.close();
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    }
    static void touchExecutable(const QString &path)
    {
        write(path, "#!/bin/false\n");
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    static QByteArray def(const QString &id, const QString &url = QStringLiteral("https://www.example.com/tv"))
    {
        return QStringLiteral(R"({"version": 1, "id": "%1", "label": "Ex %1", "url": "%2", "hosts": ["www.example.com"]})")
            .arg(id, url).toUtf8();
    }
};

void TestWebApps::builtinsAreValid()
{
    WebAppRegistry registry;
    registry.load({ WebAppRegistry::kBuiltinDir });
    QVERIFY2(registry.errors().isEmpty(), qPrintable(registry.errors().join(QLatin1Char('\n'))));
    for (const char *id : { "youtube", "netflix", "disneyplus", "primevideo", "twitch", "spotify" })
        QVERIFY2(registry.find(QString::fromLatin1(id)), id);
    QVERIFY(registry.find(QStringLiteral("netflix"))->drm);
    QVERIFY(!registry.find(QStringLiteral("youtube"))->drm);
    QVERIFY(!registry.find(QStringLiteral("youtube"))->userAgent.isEmpty());
    QVERIFY(!registry.find(QStringLiteral("youtube"))->kidsMap.isEmpty());
    for (const WebAppDef &d : registry.webApps())
        QVERIFY2(!d.kidsHosts.isEmpty(), qPrintable(d.id));
    QCOMPARE(registry.webApps().constFirst().id, QStringLiteral("youtube"));   // order 10
    for (const WebAppDef &d : registry.webApps())
        QVERIFY(!d.user && !d.hidden);
}

void TestWebApps::urls_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("valid");
    const auto ok = [](const char *u) { QTest::newRow(u) << QString::fromLatin1(u) << true; };
    const auto bad = [](const char *name, const char *u) { QTest::newRow(name) << QString::fromLatin1(u) << false; };
    ok("https://www.example.com/");
    ok("https://www.example.com");
    ok("https://www.example.com:8443/tv?x=1#top");
    ok("https://192.168.0.10:8920/web/");
    ok("https://xn--bcher-kva.example/");
    bad("http", "http://www.example.com/");
    bad("upper scheme", "HTTPS://www.example.com/");
    bad("file", "file:///etc/passwd");
    bad("javascript", "javascript:alert(1)");
    bad("flag", "--renderer-cmd-prefix=/bin/sh");
    bad("dash", "-https://www.example.com/");
    bad("credentials", "https://user:pw@www.example.com/");
    bad("user", "https://user@www.example.com/");
    bad("at in host", "https://www.example.com@evil.example/");
    bad("space", "https://www.example.com/a b");
    bad("tab", "https://www.example.com/\ta");
    bad("newline", "https://www.example.com/\n--flag");
    bad("quote", "https://www.example.com/\"x");
    bad("backslash", "https://www.example.com\\@evil.example/");
    bad("non-ascii", "https://www.exämple.com/");
    bad("upper host", "https://WWW.example.com/");
    bad("no host", "https:///path");
    bad("single label", "https://localhost/");
    bad("port 0", "https://www.example.com:0/");
    bad("port big", "https://www.example.com:70000/");
    bad("port text", "https://www.example.com:http/");
    bad("ipv6", "https://[::1]/");
    bad("empty", "");
    bad("bad label", "https://-x.example.com/");
    // S5: legacy IPv4 forms a browser would accept.
    bad("short ipv4", "https://127.1/");
    bad("hex ipv4", "https://0x7f.1/");
    bad("hex label", "https://0x7f000001/");
    bad("octal ipv4", "https://0177.0.0.1/");
    bad("leading zero", "https://192.168.0.010/");
    bad("five parts", "https://1.2.3.4.5/");
    bad("numeric tld", "https://www.example.123/");
    ok("https://www.123.example.com/");
}

void TestWebApps::urls()
{
    QFETCH(QString, url);
    QFETCH(bool, valid);
    QString error;
    QCOMPARE(WebAppRegistry::isValidUrl(url, {}, &error), valid);
    QCOMPARE(error.isEmpty(), valid);
    // Host allowlist: exact names and "*." subdomain patterns.
    QVERIFY(WebAppRegistry::isValidUrl(QStringLiteral("https://www.netflix.com/"), { QStringLiteral("www.netflix.com") }));
    QVERIFY(!WebAppRegistry::isValidUrl(QStringLiteral("https://evil.com/"), { QStringLiteral("www.netflix.com") }));
    QVERIFY(WebAppRegistry::isValidUrl(QStringLiteral("https://a.b.netflix.com/"), { QStringLiteral("*.netflix.com") }));
    QVERIFY(!WebAppRegistry::isValidUrl(QStringLiteral("https://netflix.com/"), { QStringLiteral("*.netflix.com") }));
    QVERIFY(!WebAppRegistry::isValidUrl(QStringLiteral("https://evilnetflix.com/"), { QStringLiteral("*.netflix.com") }));
}

void TestWebApps::rejects_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::newRow("unknown key") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"],"exec":"sh"})");
    QTest::newRow("host not allowed") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://evil.example/","hosts":["www.example.com"]})");
    QTest::newRow("no hosts") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/"})");
    QTest::newRow("bad host pattern") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["*"]})");
    QTest::newRow("http") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"http://www.example.com/","hosts":["www.example.com"]})");
    QTest::newRow("drm chromium") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"],"drm":true,"browser":"chromium"})");
    QTest::newRow("unknown browser") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"],"browser":"firefox"})");
    QTest::newRow("flag user agent") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"],"userAgent":"--no-sandbox"})");
    QTest::newRow("control in ua") << QByteArray("{\"version\":1,\"id\":\"x\",\"label\":\"X\",\"url\":\"https://www.example.com/\",\"hosts\":[\"www.example.com\"],\"userAgent\":\"a\\nb\"}");
    QTest::newRow("bidi label") << QByteArray("{\"version\":1,\"id\":\"x\",\"label\":\"X\\u202e\",\"url\":\"https://www.example.com/\",\"hosts\":[\"www.example.com\"]}");
    QTest::newRow("path id") << QByteArray(R"({"version":1,"id":"../x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"]})");
    QTest::newRow("version 2") << QByteArray(R"({"version":2,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"]})");
    QTest::newRow("drm string") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"],"drm":"yes"})");
    QTest::newRow("stub extra") << QByteArray(R"({"version":1,"id":"x","disabled":true,"url":"https://www.example.com/"})");
    QTest::newRow("not object") << QByteArray("[]");
    QTest::newRow("kidsMap covered") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"],"kidsHosts":["*.example.com"],"kidsMap":{"www.example.com":"restrict.example.com"}})");
    QTest::newRow("kidsMap pattern") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"],"kidsMap":{"*.example.com":"r.example.com"}})");
    QTest::newRow("kidsHosts bad") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"],"kidsHosts":["a b"]})");
    QTest::newRow("kidsHosts numeric") << QByteArray(R"({"version":1,"id":"x","label":"X","url":"https://www.example.com/","hosts":["www.example.com"],"kidsHosts":["127.1"]})");
}

void TestWebApps::rejects()
{
    QFETCH(QByteArray, json);
    QString error;
    QVERIFY(!WebAppRegistry::parse(json, &error));
    QVERIFY(!error.isEmpty());
}

void TestWebApps::layersOverrideHideAndTrust()
{
    QTemporaryDir tmp;
    const QString system = tmp.path() + QStringLiteral("/system");
    const QString user = tmp.path() + QStringLiteral("/user");
    write(system + QStringLiteral("/a.json"), def(QStringLiteral("a")));
    write(system + QStringLiteral("/b.json"), def(QStringLiteral("b")));
    write(system + QStringLiteral("/c.json"), def(QStringLiteral("c")));
    write(system + QStringLiteral("/wrongname.json"), def(QStringLiteral("d")));
    write(user + QStringLiteral("/a.json"), R"({"version": 1, "id": "a", "label": "Mine", "url": "https://www.example.com/", "hosts": ["www.example.com"]})");
    write(user + QStringLiteral("/b.json"), WebAppRegistry::disabledStub(QStringLiteral("b")));
    write(user + QStringLiteral("/user-x.json"), def(QStringLiteral("user-x")));
    QVERIFY(QFile::link(system + QStringLiteral("/c.json"), user + QStringLiteral("/linked.json")));

    WebAppRegistry registry;
    registry.load({ system, user }, user);
    QCOMPARE(registry.find(QStringLiteral("a"))->label, QStringLiteral("Mine"));
    QVERIFY(registry.find(QStringLiteral("a"))->user);
    QVERIFY(registry.find(QStringLiteral("a"))->shadowsLower);   // L2: replaces the system "a"
    QVERIFY(!registry.find(QStringLiteral("user-x"))->shadowsLower);
    QVERIFY(registry.find(QStringLiteral("b"))->hidden);   // hidden, kept for "Show"
    QVERIFY(!registry.find(QStringLiteral("c"))->user);
    QVERIFY(registry.find(QStringLiteral("user-x"))->user);
    QVERIFY(!registry.find(QStringLiteral("d")));
    QCOMPARE(registry.errors().size(), 2);                 // wrong name, symlink

    // A stub in a non-user layer removes the definition outright.
    write(user + QStringLiteral("/c.json"), WebAppRegistry::disabledStub(QStringLiteral("c")));
    registry.load({ system, user });
    QVERIFY(!registry.find(QStringLiteral("c")));

    // A directory (or file) other users can write is ignored.
    QFile::setPermissions(user, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::WriteOther
                                    | QFile::ReadOther | QFile::ExeOther);
    registry.load({ system, user }, user);
    QCOMPARE(registry.find(QStringLiteral("a"))->label, QStringLiteral("Ex a"));
    QVERIFY(!registry.find(QStringLiteral("user-x")));
    QFile::setPermissions(user, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile::setPermissions(user + QStringLiteral("/user-x.json"), QFile::ReadOwner | QFile::WriteOwner | QFile::WriteOther);
    registry.load({ system, user }, user);
    QVERIFY(!registry.find(QStringLiteral("user-x")));
}

void TestWebApps::userDefinitions()
{
    QString id, error;
    QByteArray json = WebAppRegistry::userDefinition(QStringLiteral("  My Jellyfin! "), QStringLiteral("jelly.example.org/web"),
                                                     {}, &id, &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    QCOMPARE(id, QStringLiteral("user-my-jellyfin"));
    const auto parsed = WebAppRegistry::parse(json);
    QVERIFY(parsed);
    QCOMPARE(parsed->url, QStringLiteral("https://jelly.example.org/web"));   // https:// added
    QCOMPARE(parsed->hosts, QStringList { QStringLiteral("jelly.example.org") });
    QCOMPARE(parsed->label, QStringLiteral("My Jellyfin!"));

    WebAppRegistry::userDefinition(QStringLiteral("My Jellyfin"), QStringLiteral("https://jelly.example.org/"),
                                   { QStringLiteral("user-my-jellyfin") }, &id, &error);
    QCOMPARE(id, QStringLiteral("user-my-jellyfin-2"));
    QVERIFY(WebAppRegistry::userDefinition(QStringLiteral("x"), QStringLiteral("http://a.example/"), {}, &id, &error).isEmpty());
    QVERIFY(error.contains(QLatin1String("https")));
    QVERIFY(WebAppRegistry::userDefinition(QStringLiteral("x"), QStringLiteral("https://u:p@a.example/"), {}, &id, &error).isEmpty());
    QVERIFY(WebAppRegistry::userDefinition(QStringLiteral("x"), QStringLiteral("--kiosk-printing"), {}, &id, &error).isEmpty());
    QVERIFY(WebAppRegistry::userDefinition(QStringLiteral(""), QStringLiteral("https://a.example/"), {}, &id, &error).isEmpty());
    QVERIFY(WebAppRegistry::userDefinition(QString(40, QLatin1Char('x')), QStringLiteral("https://a.example/"), {}, &id, &error).isEmpty());
    WebAppRegistry::userDefinition(QStringLiteral("☺☺"), QStringLiteral("https://a.example/"), {}, &id, &error);
    QCOMPARE(id, QStringLiteral("user-web"));
    // Scheme and host are case-insensitive; the path is kept.
    json = WebAppRegistry::userDefinition(QStringLiteral("Up"), QStringLiteral("HTTPS://Jelly.Example.ORG/Web/X"), {}, &id, &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    QCOMPARE(WebAppRegistry::parse(json)->url, QStringLiteral("https://jelly.example.org/Web/X"));
    // Single-label hosts: the message says why.
    QVERIFY(WebAppRegistry::userDefinition(QStringLiteral("x"), QStringLiteral("https://jellyfin:8096/"), {}, &id, &error).isEmpty());
    QVERIFY(error.contains(QLatin1String("single names")));
}

void TestWebApps::browserChoice()
{
    QTemporaryDir tmp;
    LaunchEnv env;
    env.home = tmp.path() + QStringLiteral("/home");
    env.dataHome = env.home + QStringLiteral("/.local/share");
    env.pathDirs = { tmp.path() + QStringLiteral("/bin") };
    env.flatpakInstallations = { tmp.path() + QStringLiteral("/flatpak") };
    auto installFlatpak = [&](const QString &id) {
        QDir().mkpath(tmp.path() + QStringLiteral("/flatpak/app/") + id + QStringLiteral("/current/active/files"));
    };
    WebAppDef plain;
    plain.id = QStringLiteral("yt");
    plain.label = QStringLiteral("YT");
    plain.url = QStringLiteral("https://www.youtube.com/tv");
    plain.hosts = { QStringLiteral("www.youtube.com") };
    WebAppDef drm = plain;
    drm.drm = true;

    QVERIFY(!WebAppLaunch::choose(plain, env).ok());   // nothing installed
    installFlatpak(QStringLiteral("org.chromium.Chromium"));
    QVERIFY(!WebAppLaunch::choose(plain, env).ok());   // no flatpak program
    touchExecutable(tmp.path() + QStringLiteral("/bin/flatpak"));
    QCOMPARE(WebAppLaunch::choose(plain, env).browser->id, QStringLiteral("chromium"));
    QVERIFY(WebAppLaunch::choose(plain, env).flatpak);
    QVERIFY(!WebAppLaunch::choose(drm, env).ok());     // Chromium has no Widevine
    env.systemRoot = tmp.path() + QStringLiteral("/root");
    env.trustedUid = ::getuid();
    touchExecutable(env.systemRoot + QStringLiteral("/usr/bin/brave-browser"));
    QCOMPARE(WebAppLaunch::choose(drm, env).browser->id, QStringLiteral("brave"));
    QVERIFY(!WebAppLaunch::choose(drm, env).flatpak);
    installFlatpak(QStringLiteral("com.google.Chrome"));
    QCOMPARE(WebAppLaunch::choose(drm, env).browser->id, QStringLiteral("chrome"));
    QCOMPARE(WebAppLaunch::choose(plain, env).browser->id, QStringLiteral("chrome"));
    WebAppDef prefers = plain;
    prefers.browser = QStringLiteral("brave");
    QCOMPARE(WebAppLaunch::choose(prefers, env).browser->id, QStringLiteral("brave"));
    QCOMPARE(WebAppLaunch::order(drm), (QStringList { QStringLiteral("chrome"), QStringLiteral("edge"), QStringLiteral("brave") }));
}

void TestWebApps::planArgv()
{
    QTemporaryDir tmp;
    LaunchEnv env;
    env.home = tmp.path() + QStringLiteral("/home");
    env.dataHome = env.home + QStringLiteral("/.local/share");
    env.pathDirs = { tmp.path() + QStringLiteral("/bin") };
    env.flatpakInstallations = { tmp.path() + QStringLiteral("/flatpak") };
    WebAppDef app;
    app.id = QStringLiteral("netflix");
    app.label = QStringLiteral("Netflix");
    app.url = QStringLiteral("https://www.netflix.com/browse");
    app.hosts = { QStringLiteral("www.netflix.com") };
    app.drm = true;

    LaunchPlan plan = WebAppLaunch::plan(app, QStringLiteral("kid"), env);
    QVERIFY(!plan.ok());
    QCOMPARE(plan.install, QStringLiteral("Google Chrome"));

    QDir().mkpath(tmp.path() + QStringLiteral("/flatpak/app/com.google.Chrome/current/active/files"));
    touchExecutable(tmp.path() + QStringLiteral("/bin/flatpak"));
    plan = WebAppLaunch::plan(app, QStringLiteral("kid"), env);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    const QString dataDir = env.home + QStringLiteral("/.var/app/com.google.Chrome/data/mightyos/profiles/kid");
    QCOMPARE(plan.argv, (QStringList {
        tmp.path() + QStringLiteral("/bin/flatpak"), QStringLiteral("run"), QStringLiteral("--die-with-parent"),
        QStringLiteral("--nodevice=all"), QStringLiteral("--device=dri"), QStringLiteral("--device=input"),
        QStringLiteral("--nofilesystem=host"), QStringLiteral("--nofilesystem=home"),
        QStringLiteral("--nofilesystem=xdg-documents"), QStringLiteral("--nofilesystem=xdg-download"),
        QStringLiteral("com.google.Chrome"),
        QStringLiteral("--user-data-dir=") + dataDir, QStringLiteral("--kiosk"), QStringLiteral("--no-first-run"),
        QStringLiteral("--no-default-browser-check"), QStringLiteral("--password-store=basic"),
        QStringLiteral("--hide-crash-restore-bubble"), QStringLiteral("--noerrdialogs"),
        QStringLiteral("--ozone-platform-hint=auto"), QStringLiteral("--app=https://www.netflix.com/browse") }));
    QVERIFY(!QFileInfo::exists(dataDir));   // the browser creates it, not MightyOS

    // Another profile, another data dir (no shared logins).
    const LaunchPlan other = WebAppLaunch::plan(app, QStringLiteral("parent"), env);
    QVERIFY(other.argv.at(11) != plan.argv.at(11));
    QVERIFY(plan.argv.at(11).startsWith(QLatin1String("--user-data-dir=")));
    // The user agent is one element; invalid input never reaches argv.
    app.userAgent = QStringLiteral("Mozilla/5.0 (SMART-TV; Linux)");
    QCOMPARE(WebAppLaunch::plan(app, QStringLiteral("kid"), env).argv.at(19), QStringLiteral("--user-agent=Mozilla/5.0 (SMART-TV; Linux)"));
    app.url = QStringLiteral("https://evil.example/");
    QVERIFY(!WebAppLaunch::plan(app, QStringLiteral("kid"), env).ok());
    app.url = QStringLiteral("https://www.netflix.com/browse");
    QVERIFY(!WebAppLaunch::plan(app, QStringLiteral("../x"), env).ok());
    const LaunchPlan noProfile = WebAppLaunch::plan(app, QString(), env);
    QVERIFY(!noProfile.ok());
    QVERIFY(noProfile.error.contains(QLatin1String("profile")));

    // Native browser: data under our own XDG data dir.
    QVERIFY(QDir(tmp.path() + QStringLiteral("/flatpak")).removeRecursively());
    env.systemRoot = tmp.path() + QStringLiteral("/root");
    env.trustedUid = ::getuid();
    touchExecutable(env.systemRoot + QStringLiteral("/usr/bin/google-chrome-stable"));
    app.userAgent.clear();
    plan = WebAppLaunch::plan(app, QStringLiteral("kid"), env);
    QCOMPARE(plan.argv.at(0), env.systemRoot + QStringLiteral("/usr/bin/google-chrome-stable"));
    QCOMPARE(plan.argv.at(1), QStringLiteral("--user-data-dir=") + env.dataHome + QStringLiteral("/mightyos/profiles/kid/browsers/chrome"));
}

void TestWebApps::kidsNetworkLimits()
{
    // S1: kids get only the service's hosts and CDNs; everything else
    // (IP literals, loopback, other sites) goes to a dead proxy, and the
    // kidsMap re-points names (YouTube Restricted Mode).
    WebAppRegistry registry;
    registry.load({ WebAppRegistry::kBuiltinDir });
    const WebAppDef *yt = registry.find(QStringLiteral("youtube"));
    QVERIFY(yt);
    const QStringList args = WebAppLaunch::kidsArgs(*yt);
    QCOMPARE(args.size(), 3);
    QCOMPARE(args.at(0), QStringLiteral("--proxy-server=127.0.0.1:9"));
    QVERIFY(args.at(1).startsWith(QLatin1String("--proxy-bypass-list=<-loopback>;")));
    QVERIFY(args.at(1).contains(QLatin1String(";*.googlevideo.com")));
    QVERIFY(args.at(1).contains(QLatin1String(";www.youtube.com")));
    QVERIFY(args.at(1).contains(QLatin1String(";restrict.youtube.com")));
    const QString rules = args.at(2);
    QVERIFY(rules.startsWith(QLatin1String("--host-resolver-rules=MAP ")));
    QVERIFY(rules.contains(QLatin1String("MAP www.youtube.com restrict.youtube.com, ")));
    QVERIFY(rules.indexOf(QLatin1String("MAP * ~NOTFOUND")) > rules.indexOf(QLatin1String("MAP www.youtube.com")));
    QVERIFY(rules.contains(QLatin1String("EXCLUDE *.googlevideo.com")));
    QVERIFY(rules.contains(QLatin1String("EXCLUDE restrict.youtube.com")));
    QVERIFY(!rules.contains(QLatin1String("EXCLUDE www.youtube.com")));   // that would undo the MAP

    QTemporaryDir tmp;
    LaunchEnv env;
    env.home = tmp.path();
    env.pathDirs = { tmp.path() + QStringLiteral("/bin") };
    env.flatpakInstallations = { tmp.path() + QStringLiteral("/flatpak") };
    QDir().mkpath(tmp.path() + QStringLiteral("/flatpak/app/com.google.Chrome/current/active/files"));
    touchExecutable(tmp.path() + QStringLiteral("/bin/flatpak"));
    const LaunchPlan kid = WebAppLaunch::plan(*yt, QStringLiteral("kid"), env, true);
    const LaunchPlan adult = WebAppLaunch::plan(*yt, QStringLiteral("adult"), env, false);
    QVERIFY(kid.argv.contains(args.at(2)));
    QVERIFY(!adult.argv.contains(args.at(0)));
    QVERIFY(kid.argv.last().startsWith(QLatin1String("--app=https://")));
}

void TestWebApps::nativeBrowsersOnlyFromTrustedPaths()
{
    // M5 / S7: fixed system paths only, root's (here: ours), not writable
    // by others, no snaps (canonical /snap/..., /usr/bin/snap, or wrapper
    // scripts handing over to /snap/bin).
    QTemporaryDir tmp;
    LaunchEnv env;
    env.systemRoot = tmp.path();
    env.trustedUid = ::getuid();
    env.pathDirs = { tmp.path() + QStringLiteral("/home/bin") };
    WebAppDef plain;
    plain.url = QStringLiteral("https://www.example.com/");
    plain.hosts = { QStringLiteral("www.example.com") };
    // In PATH but not a fixed path: ignored.
    touchExecutable(tmp.path() + QStringLiteral("/home/bin/chromium"));
    QVERIFY(!WebAppLaunch::choose(plain, env).ok());
    // Ubuntu's transitional chromium-browser script: skipped.
    write(tmp.path() + QStringLiteral("/usr/bin/chromium-browser"), "#!/bin/sh\nexec /snap/bin/chromium \"$@\"\n");
    QFile::setPermissions(tmp.path() + QStringLiteral("/usr/bin/chromium-browser"),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QVERIFY(!WebAppLaunch::choose(plain, env).ok());
    // A symlink to /usr/bin/snap: skipped.
    touchExecutable(tmp.path() + QStringLiteral("/usr/bin/snap"));
    QVERIFY(QFile::link(tmp.path() + QStringLiteral("/usr/bin/snap"), tmp.path() + QStringLiteral("/usr/bin/chromium")));
    QVERIFY(!WebAppLaunch::choose(plain, env).ok());
    QFile::remove(tmp.path() + QStringLiteral("/usr/bin/chromium"));
    // Under /snap: skipped.
    touchExecutable(tmp.path() + QStringLiteral("/snap/chromium/current/chrome"));
    QVERIFY(QFile::link(tmp.path() + QStringLiteral("/snap/chromium/current/chrome"),
                        tmp.path() + QStringLiteral("/usr/bin/chromium")));
    QVERIFY(!WebAppLaunch::choose(plain, env).ok());
    QFile::remove(tmp.path() + QStringLiteral("/usr/bin/chromium"));
    // A real binary, but group-writable: refused; fixed: taken.
    const QString chrome = tmp.path() + QStringLiteral("/opt/google/chrome/google-chrome");
    touchExecutable(chrome);
    QFile::setPermissions(chrome, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::WriteGroup);
    QVERIFY(!WebAppLaunch::choose(plain, env).ok());
    QFile::setPermissions(chrome, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const BrowserChoice c = WebAppLaunch::choose(plain, env);
    QVERIFY(c.ok());
    QCOMPARE(c.browser->id, QStringLiteral("chrome"));
    QCOMPARE(c.program, chrome);
    // Its directory writable by others: refused.
    const QString dir = tmp.path() + QStringLiteral("/opt/google/chrome");
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::WriteOther | QFile::ExeOther);
    QVERIFY(WebAppLaunch::trustedNativeBrowser(QStringLiteral("/opt/google/chrome/google-chrome"), env).isEmpty());
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    // Owned by someone else than the trusted uid: refused.
    env.trustedUid = ::getuid() + 1;
    QVERIFY(!WebAppLaunch::choose(plain, env).ok());
}

QTEST_GUILESS_MAIN(TestWebApps)
#include "tst_webapps.moc"
