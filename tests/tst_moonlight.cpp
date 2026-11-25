#include "moonlight.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

// Moonlight.conf (untrusted INI from a sandbox-writable dir), item keys and
// the `moonlight stream` argv.
class TestMoonlight : public QObject
{
    Q_OBJECT

private slots:
    void iniStrings();
    void parsesHostsAndApps();
    void hostileConfig();
    void keys();
    void readHostsChecksFiles();
    void streamArgv();

private:
    static void write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    }
};

// As Moonlight-Qt 6 writes it (QSettings INI; trimmed).
static const QByteArray kConfig = R"ini([General]
latestsupportedversion-v1=6.1.0

[hosts]
1\apps\1\appcollector=false
1\apps\1\directlaunch=false
1\apps\1\hdr=false
1\apps\1\hidden=false
1\apps\1\id=881448767
1\apps\1\name=Desktop
1\apps\2\hidden=false
1\apps\2\name=Steam Big Picture
1\apps\3\hidden=true
1\apps\3\name=Secret
1\apps\4\name="Doom, Eternal"
1\apps\5\name=--help
1\apps\size=5
1\customname=false
1\hostname=GAMING-PC
1\localaddress=192.168.0.20
1\mac=@ByteArray(\0\x11\"3DU)
1\srvcert=@ByteArray(-----BEGIN CERTIFICATE-----\n...)
1\uuid=8D8C1F3A-2B7E-4C11-9A51-0123456789AB
2\hostname=Living Room
2\uuid=not-a-uuid
2\apps\size=0
3\hostname=HTPC
3\apps\1\name=Kodi
3\apps\size=1
size=3
)ini";

void TestMoonlight::iniStrings()
{
    QString s;
    QVERIFY(Moonlight::parseIniString("Desktop", &s));
    QCOMPARE(s, QStringLiteral("Desktop"));
    QVERIFY(Moonlight::parseIniString("\"Doom, Eternal\"", &s));
    QCOMPARE(s, QStringLiteral("Doom, Eternal"));
    QVERIFY(Moonlight::parseIniString("\" padded \"", &s));
    QCOMPARE(s, QStringLiteral(" padded "));
    QVERIFY(Moonlight::parseIniString("a\\\"b\\\\c", &s));
    QCOMPARE(s, QStringLiteral("a\"b\\c"));
    QVERIFY(Moonlight::parseIniString("caf\\xe9", &s));
    QCOMPARE(s, QStringLiteral("café"));
    QVERIFY(Moonlight::parseIniString("Caf\xc3\xa9", &s));   // Qt 6: raw UTF-8
    QCOMPARE(s, QStringLiteral("Café"));
    QVERIFY(Moonlight::parseIniString("@@at", &s));
    QCOMPARE(s, QStringLiteral("@at"));
    QVERIFY(!Moonlight::parseIniString("a, b", &s));        // a list
    QVERIFY(!Moonlight::parseIniString("@ByteArray(x)", &s));
}

void TestMoonlight::parsesHostsAndApps()
{
    const QList<MoonlightHost> hosts = Moonlight::parseConfig(kConfig);
    QCOMPARE(hosts.size(), 2);   // "Living Room": no uuid, not a host name
    const MoonlightHost &pc = hosts.at(0);
    QCOMPARE(pc.id, QStringLiteral("8d8c1f3a-2b7e-4c11-9a51-0123456789ab"));
    QCOMPARE(pc.name, QStringLiteral("GAMING-PC"));
    QCOMPARE(pc.apps.size(), 4);   // "--help" rejected
    QCOMPARE(pc.apps.at(0).name, QStringLiteral("Desktop"));
    QCOMPARE(pc.apps.at(1).name, QStringLiteral("Steam Big Picture"));
    QVERIFY(pc.apps.at(2).hidden);
    QCOMPARE(pc.apps.at(3).name, QStringLiteral("Doom, Eternal"));
    QCOMPARE(hosts.at(1).id, QStringLiteral("htpc"));   // no uuid: the host name (lower case)
    QCOMPARE(hosts.at(1).apps.size(), 1);
}

void TestMoonlight::hostileConfig()
{
    // Control and bidi characters, huge counts, garbage lines, CRLF.
    QByteArray ini = "[hosts]\r\n1\\uuid=11111111-2222-3333-4444-555555555555\r\n"
                     "1\\hostname=Evil\\x202ePC\\nX\r\n"
                     "1\\apps\\size=999999\r\n"
                     "1\\apps\\1\\name=Bad\\x0aName\r\n"
                     "1\\apps\\2\\name=Ok\r\n"
                     "garbage line\r\n=nokey\r\n"
                     "size=100000\r\n";
    const QList<MoonlightHost> hosts = Moonlight::parseConfig(ini);
    QCOMPARE(hosts.size(), 1);
    QVERIFY(!hosts.at(0).name.contains(QChar(0x202e)));
    QVERIFY(!hosts.at(0).name.contains(QLatin1Char('\n')));
    QCOMPARE(hosts.at(0).apps.size(), 1);
    QCOMPARE(hosts.at(0).apps.at(0).name, QStringLiteral("Ok"));
    QVERIFY(Moonlight::parseConfig(QByteArray(100000, '\0')).isEmpty());
    QVERIFY(Moonlight::parseConfig("[hosts]\nsize=abc\n").isEmpty());
}

void TestMoonlight::keys()
{
    const QString host = QStringLiteral("8d8c1f3a-2b7e-4c11-9a51-0123456789ab");
    const QString key = Moonlight::appKey(host, QStringLiteral("Steam Big Picture"));
    QCOMPARE(key, QStringLiteral("moonlight:8d8c1f3a-2b7e-4c11-9a51-0123456789ab/Steam%20Big%20Picture"));
    QString h, a;
    QVERIFY(Moonlight::parseKey(key, &h, &a));
    QCOMPARE(h, host);
    QCOMPARE(a, QStringLiteral("Steam Big Picture"));
    QVERIFY(Moonlight::parseKey(Moonlight::hostKey(host), &h, &a));
    QVERIFY(a.isEmpty());
    QVERIFY(Moonlight::parseKey(Moonlight::appKey(QStringLiteral("htpc"), QStringLiteral("Café / Ü")), &h, &a));
    QCOMPARE(a, QStringLiteral("Café / Ü"));
    QVERIFY(!Moonlight::parseKey(QStringLiteral("moonlight:htpc/Steam Big Picture"), &h, &a));   // not canonical
    QVERIFY(!Moonlight::parseKey(QStringLiteral("moonlight:htpc/%2Dhelp"), &h, &a));            // "-help"
    QVERIFY(!Moonlight::parseKey(QStringLiteral("moonlight:-x/Desktop"), &h, &a));
    QVERIFY(!Moonlight::parseKey(QStringLiteral("moonlight:a b/Desktop"), &h, &a));
    QVERIFY(!Moonlight::parseKey(QStringLiteral("moonlight:htpc/%0A"), &h, &a));
    QVERIFY(!Moonlight::parseKey(QStringLiteral("steam:620"), &h, &a));
    QVERIFY(Moonlight::appKey(QStringLiteral("--x"), QStringLiteral("Desktop")).isEmpty());
    QVERIFY(Moonlight::isValidHostId(QStringLiteral("192.168.0.20")));
    QVERIFY(!Moonlight::isValidHostId(QStringLiteral("a/b")));
    // S5: legacy IPv4 forms are not host ids.
    for (const char *bad : { "127.1", "0x7f.1", "0177.0.0.1", "192.168.0.010", "1.2.3.4.5", "0x7f000001", "2130706433" })
        QVERIFY2(!Moonlight::isValidHostId(QString::fromLatin1(bad)), bad);
    QVERIFY(Moonlight::isValidHostId(QStringLiteral("pc2.home")));
    // L5: one case for the host part.
    QCOMPARE(Moonlight::canonicalKey(QStringLiteral("moonlight:HTPC/Steam%20Big%20Picture")),
             QStringLiteral("moonlight:htpc/Steam%20Big%20Picture"));
    QCOMPARE(Moonlight::canonicalKey(QStringLiteral("moonlight:8D8C1F3A-2B7E-4C11-9A51-0123456789AB")),
             QStringLiteral("moonlight:8d8c1f3a-2b7e-4c11-9a51-0123456789ab"));
    QCOMPARE(Moonlight::canonicalKey(QStringLiteral("flatpak:tv.kodi.Kodi")), QStringLiteral("flatpak:tv.kodi.Kodi"));
}

void TestMoonlight::readHostsChecksFiles()
{
    QTemporaryDir tmp;
    LaunchEnv env;
    env.home = tmp.path();
    env.pathDirs = { tmp.path() + QStringLiteral("/bin") };
    env.flatpakInstallations = { tmp.path() + QStringLiteral("/flatpak") };
    const QList<Moonlight::ConfigFile> files = Moonlight::configFiles(tmp.path(), tmp.path() + QStringLiteral("/.config"));
    QCOMPARE(files.size(), 2);
    QVERIFY(files.at(0).flatpak);
    QVERIFY(files.at(0).path.endsWith(QLatin1String("/.var/app/com.moonlight_stream.Moonlight/config/Moonlight Game Streaming Project/Moonlight.conf")));
    write(files.at(0).path, kConfig);
    // Not installed: not read.
    QVERIFY(Moonlight::readHosts(files, env).isEmpty());
    QVERIFY(!Moonlight::installed(env));
    QDir().mkpath(tmp.path() + QStringLiteral("/flatpak/app/com.moonlight_stream.Moonlight/current/active/files"));
    write(tmp.path() + QStringLiteral("/bin/flatpak"), "#!/bin/false\n");
    QFile::setPermissions(tmp.path() + QStringLiteral("/bin/flatpak"), QFile::ReadOwner | QFile::ExeOwner);
    QVERIFY(Moonlight::installed(env));
    QList<MoonlightHost> hosts = Moonlight::readHosts(files, env);
    QCOMPARE(hosts.size(), 2);
    QVERIFY(hosts.at(0).flatpak);
    // A symlink planted in the sandbox's dir is not followed; nor is a huge file read.
    QFile::remove(files.at(0).path);
    write(tmp.path() + QStringLiteral("/elsewhere.conf"), kConfig);
    QVERIFY(QFile::link(tmp.path() + QStringLiteral("/elsewhere.conf"), files.at(0).path));
    QVERIFY(Moonlight::readHosts(files, env).isEmpty());
    QFile::remove(files.at(0).path);
    write(files.at(0).path, kConfig + QByteArray(Moonlight::kMaxConfigBytes, ';'));
    QVERIFY(Moonlight::readHosts(files, env).isEmpty());
}

void TestMoonlight::streamArgv()
{
    QTemporaryDir tmp;
    LaunchEnv env;
    env.home = tmp.path();
    env.pathDirs = { tmp.path() + QStringLiteral("/bin") };
    env.flatpakInstallations = { tmp.path() + QStringLiteral("/flatpak") };
    LaunchPlan plan = Moonlight::planStream(QStringLiteral("htpc"), QStringLiteral("Desktop"), true, env);
    QVERIFY(!plan.ok());
    QCOMPARE(plan.install, QStringLiteral("Moonlight"));
    write(tmp.path() + QStringLiteral("/bin/moonlight"), "#!/bin/false\n");
    QFile::setPermissions(tmp.path() + QStringLiteral("/bin/moonlight"), QFile::ReadOwner | QFile::ExeOwner);
    plan = Moonlight::planStream(QStringLiteral("htpc"), QStringLiteral("Steam Big Picture"), true, env);
    QCOMPARE(plan.argv, (QStringList { tmp.path() + QStringLiteral("/bin/moonlight"), QStringLiteral("stream"),
                                       QStringLiteral("--"), QStringLiteral("htpc"), QStringLiteral("Steam Big Picture") }));
    QDir().mkpath(tmp.path() + QStringLiteral("/flatpak/app/com.moonlight_stream.Moonlight/current/active/files"));
    write(tmp.path() + QStringLiteral("/bin/flatpak"), "#!/bin/false\n");
    QFile::setPermissions(tmp.path() + QStringLiteral("/bin/flatpak"), QFile::ReadOwner | QFile::ExeOwner);
    plan = Moonlight::planStream(QStringLiteral("htpc"), QStringLiteral("Desktop"), true, env);
    QCOMPARE(plan.argv.mid(0, 4), (QStringList { tmp.path() + QStringLiteral("/bin/flatpak"), QStringLiteral("run"),
                                                 Moonlight::kFlatpakId, QStringLiteral("stream") }));
    QCOMPARE(Moonlight::planStream(QStringLiteral("htpc"), QStringLiteral("Desktop"), false, env).argv.at(0),
             tmp.path() + QStringLiteral("/bin/moonlight"));
    QVERIFY(!Moonlight::planStream(QStringLiteral("-x"), QStringLiteral("Desktop"), true, env).ok());
    QVERIFY(!Moonlight::planStream(QStringLiteral("htpc"), QStringLiteral("--help"), true, env).ok());
}

QTEST_GUILESS_MAIN(TestMoonlight)
#include "tst_moonlight.moc"
