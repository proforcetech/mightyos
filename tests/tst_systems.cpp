#include "emulatorlaunch.h"
#include "systemregistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

// systems.d definitions (strict validation, layers) and the argv planning
// for RetroArch, Flatpak and native emulators and Steam.
class TestSystems : public QObject
{
    Q_OBJECT

private slots:
    void builtinsCoverTargets();
    void rejects_data();
    void rejects();
    void argTemplates();
    void layersOverrideAndDisable();
    void symlinkedDefinitionIgnored();
    void untrustedDefinitionsIgnored();
    void planRetroArch();
    void planFlatpakAndNative();
    void planSteam();
    void flatpakSandboxArgs();
    void sandboxKey();

private:
    static QJsonObject valid()
    {
        return QJsonObject {
            { QStringLiteral("version"), 1 }, { QStringLiteral("id"), QStringLiteral("snes") },
            { QStringLiteral("label"), QStringLiteral("SNES") }, { QStringLiteral("category"), QStringLiteral("emulators") },
            { QStringLiteral("extensions"), QJsonArray { QStringLiteral("sfc") } },
            { QStringLiteral("emulators"), QJsonArray { QJsonObject {
                { QStringLiteral("id"), QStringLiteral("snes9x") }, { QStringLiteral("label"), QStringLiteral("Snes9x") },
                { QStringLiteral("type"), QStringLiteral("retroarch") }, { QStringLiteral("core"), QStringLiteral("snes9x") } } } },
        };
    }
    static QByteArray json(const QJsonObject &o) { return QJsonDocument(o).toJson(); }
    static void write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    }
    static void touchExecutable(const QString &path)
    {
        write(path, "#!/bin/false\n");
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
};

void TestSystems::builtinsCoverTargets()
{
    SystemRegistry registry;
    registry.load({ SystemRegistry::kBuiltinDir }, QStringLiteral("/home/u"));
    QVERIFY2(registry.errors().isEmpty(), qPrintable(registry.errors().join(QLatin1Char('\n'))));
    // PLAN D9 + D12.
    for (const char *id : { "nes", "snes", "n64", "gb", "gbc", "gba", "genesis", "mastersystem", "saturn",
                            "dreamcast", "arcade", "ps1", "ps2", "psp", "gamecube", "wii", "nds", "3ds", "steam" }) {
        const SystemDef *s = registry.find(QString::fromLatin1(id));
        QVERIFY2(s, id);
        if (s->kind == SystemDef::Kind::Roms) {
            QVERIFY2(!s->emulators.isEmpty(), id);
            QCOMPARE(s->romDirs, QStringList { QStringLiteral("/home/u/ROMs/") + s->id });
        }
    }
    QCOMPARE(registry.find(QStringLiteral("steam"))->category, QStringLiteral("games"));
    // Sorted by "order", then label: NES (10) before SNES (20) before PS1 (90).
    QStringList order;
    for (const SystemDef &s : registry.systems())
        order.append(s.id);
    QVERIFY(order.indexOf(QStringLiteral("nes")) < order.indexOf(QStringLiteral("snes")));
    QVERIFY(order.indexOf(QStringLiteral("snes")) < order.indexOf(QStringLiteral("ps1")));
    QVERIFY(!registry.find(QStringLiteral("../etc")));
}

void TestSystems::rejects_data()
{
    QTest::addColumn<QByteArray>("data");
    auto with = [](const char *key, const QJsonValue &v) {
        QJsonObject o = valid();
        o.insert(QLatin1String(key), v);
        return json(o);
    };
    auto emu = [](const QJsonObject &e) {
        QJsonObject o = valid();
        o.insert(QStringLiteral("emulators"), QJsonArray { e });
        o.remove(QStringLiteral("defaultEmulator"));
        return json(o);
    };
    const QJsonObject fp { { QStringLiteral("id"), QStringLiteral("d") }, { QStringLiteral("label"), QStringLiteral("D") },
                           { QStringLiteral("type"), QStringLiteral("flatpak") },
                           { QStringLiteral("app"), QStringLiteral("org.DolphinEmu.dolphin-emu") } };
    QTest::newRow("not json") << QByteArray("{");
    QTest::newRow("version 2") << with("version", 2);
    QTest::newRow("path id") << with("id", QStringLiteral("../x"));
    QTest::newRow("upper id") << with("id", QStringLiteral("SNES"));
    QTest::newRow("unknown key") << with("exec", QStringLiteral("sh -c x"));
    QTest::newRow("bad category") << with("category", QStringLiteral("apps"));
    QTest::newRow("control in label") << with("label", QString::fromUtf8("SN\xE2\x80\xAE" "ES"));
    QTest::newRow("bad tint") << with("tint", QStringLiteral("red"));
    QTest::newRow("bad icon") << with("icon", QStringLiteral("../../x"));
    QTest::newRow("bad ext") << with("extensions", QJsonArray { QStringLiteral(".sfc") });
    QTest::newRow("no ext") << with("extensions", QJsonArray {});
    QTest::newRow("relative rom dir") << with("romDirs", QJsonArray { QStringLiteral("ROMs/snes") });
    QTest::newRow("dotdot rom dir") << with("romDirs", QJsonArray { QStringLiteral("~/ROMs/../..") });
    QTest::newRow("root rom dir") << with("romDirs", QJsonArray { QStringLiteral("/") });
    QTest::newRow("order float") << with("order", 1.5);
    QTest::newRow("label number") << with("label", 5);
    QTest::newRow("default unknown") << with("defaultEmulator", QStringLiteral("nope"));
    QTest::newRow("flatpak without args") << emu(fp);
    QJsonObject partial = fp;
    partial.insert(QStringLiteral("args"), QJsonArray { QStringLiteral("--file={rom}") });
    QTest::newRow("partial placeholder") << emu(partial);
    QJsonObject twice = fp;
    twice.insert(QStringLiteral("args"), QJsonArray { QStringLiteral("{rom}"), QStringLiteral("{rom}") });
    QTest::newRow("rom twice") << emu(twice);
    QJsonObject unknownPh = fp;
    unknownPh.insert(QStringLiteral("args"), QJsonArray { QStringLiteral("{rom}"), QStringLiteral("{home}") });
    QTest::newRow("unknown placeholder") << emu(unknownPh);
    QJsonObject badApp = fp;
    badApp.insert(QStringLiteral("app"), QStringLiteral("dolphin; rm"));
    badApp.insert(QStringLiteral("args"), QJsonArray { QStringLiteral("{rom}") });
    QTest::newRow("bad flatpak id") << emu(badApp);
    QJsonObject native { { QStringLiteral("id"), QStringLiteral("n") }, { QStringLiteral("label"), QStringLiteral("N") },
                         { QStringLiteral("type"), QStringLiteral("native") }, { QStringLiteral("program"), QStringLiteral("./x") },
                         { QStringLiteral("args"), QJsonArray { QStringLiteral("{rom}") } } };
    QTest::newRow("relative program") << emu(native);
    QJsonObject raArgs { { QStringLiteral("id"), QStringLiteral("r") }, { QStringLiteral("label"), QStringLiteral("R") },
                         { QStringLiteral("type"), QStringLiteral("retroarch") }, { QStringLiteral("core"), QStringLiteral("x") },
                         { QStringLiteral("args"), QJsonArray { QStringLiteral("--verbose") } } };
    QTest::newRow("retroarch args") << emu(raArgs);
    QJsonObject raCore = raArgs;
    raCore.remove(QStringLiteral("args"));
    raCore.insert(QStringLiteral("core"), QStringLiteral("../../lib/evil"));
    QTest::newRow("retroarch core path") << emu(raCore);
    QJsonObject steam { { QStringLiteral("version"), 1 }, { QStringLiteral("id"), QStringLiteral("steam") },
                        { QStringLiteral("label"), QStringLiteral("Steam") }, { QStringLiteral("category"), QStringLiteral("games") },
                        { QStringLiteral("kind"), QStringLiteral("steam") },
                        { QStringLiteral("extensions"), QJsonArray { QStringLiteral("exe") } } };
    QTest::newRow("steam with extensions") << json(steam);
    QTest::newRow("too large") << QByteArray(SystemRegistry::kMaxFileBytes + 1, ' ');
}

void TestSystems::rejects()
{
    QFETCH(QByteArray, data);
    QString error;
    QVERIFY(!SystemRegistry::parse(data, QStringLiteral("/home/u"), &error));
    QVERIFY(!error.isEmpty());
}

void TestSystems::argTemplates()
{
    QVERIFY(SystemRegistry::isValidArgTemplate({ QStringLiteral("-b"), QStringLiteral("-e"), QStringLiteral("{rom}") }, true));
    QVERIFY(SystemRegistry::isValidArgTemplate({ QStringLiteral("--"), QStringLiteral("{rom}"), QStringLiteral("{saves}") }, true));
    QVERIFY(!SystemRegistry::isValidArgTemplate({ QStringLiteral("-b") }, true));
    QVERIFY(!SystemRegistry::isValidArgTemplate({ QStringLiteral("{rom}x") }, true));
    QVERIFY(!SystemRegistry::isValidArgTemplate({ QStringLiteral("{rom}"), QStringLiteral("a\nb") }, true));
    // Substitution is whole-element only: a ROM name full of shell syntax
    // stays one argument.
    const QString evil = QStringLiteral("/r/$(rm -rf ~); `x` {saves}.sfc");
    const QStringList out = EmulatorLaunch::substitute({ QStringLiteral("-e"), QStringLiteral("{rom}") },
                                                       { { QStringLiteral("{rom}"), evil } });
    QCOMPARE(out, (QStringList { QStringLiteral("-e"), evil }));
}

void TestSystems::layersOverrideAndDisable()
{
    QTemporaryDir tmp;
    const QString system = tmp.path() + QStringLiteral("/share/mightyos/systems.d");
    const QString user = tmp.path() + QStringLiteral("/config/mightyos/systems.d");
    QJsonObject custom = valid();
    custom.insert(QStringLiteral("label"), QStringLiteral("Super NES"));
    custom.insert(QStringLiteral("romDirs"), QJsonArray { QStringLiteral("/mnt/games/snes/") });
    write(user + QStringLiteral("/snes.json"), json(custom));
    write(user + QStringLiteral("/n64.json"), R"({"version": 1, "id": "n64", "disabled": true})");
    write(user + QStringLiteral("/broken.json"), "{ nope");
    QJsonObject renamed = valid();
    renamed.insert(QStringLiteral("id"), QStringLiteral("sfc"));
    write(user + QStringLiteral("/other-name.json"), json(renamed));   // file name must be <id>.json
    QJsonObject extra = valid();
    extra.insert(QStringLiteral("id"), QStringLiteral("msx"));
    extra.insert(QStringLiteral("label"), QStringLiteral("MSX"));
    write(system + QStringLiteral("/msx.json"), json(extra));

    SystemRegistry registry;
    registry.load({ SystemRegistry::kBuiltinDir, system, user }, tmp.path());
    QCOMPARE(registry.find(QStringLiteral("snes"))->label, QStringLiteral("Super NES"));
    QCOMPARE(registry.find(QStringLiteral("snes"))->romDirs, QStringList { QStringLiteral("/mnt/games/snes") });
    QVERIFY(!registry.find(QStringLiteral("n64")));
    QVERIFY(registry.find(QStringLiteral("msx")));
    QVERIFY(!registry.find(QStringLiteral("sfc")));
    QVERIFY(registry.find(QStringLiteral("nes")));   // builtins stay
    QCOMPARE(registry.errors().size(), 2);
}

void TestSystems::symlinkedDefinitionIgnored()
{
    QTemporaryDir tmp;
    const QString user = tmp.path() + QStringLiteral("/systems.d");
    QJsonObject custom = valid();
    custom.insert(QStringLiteral("label"), QStringLiteral("Linked"));
    write(tmp.path() + QStringLiteral("/elsewhere.json"), json(custom));
    QDir().mkpath(user);
    QVERIFY(QFile::link(tmp.path() + QStringLiteral("/elsewhere.json"), user + QStringLiteral("/snes.json")));
    SystemRegistry registry;
    registry.load({ SystemRegistry::kBuiltinDir, user }, tmp.path());
    QCOMPARE(registry.find(QStringLiteral("snes"))->label, QStringLiteral("SNES"));   // builtin kept
    QCOMPARE(registry.errors().size(), 1);
}

// S4: a definition decides which program runs, so it must be ours (or
// root's) and not writable by anybody else, and so must its directory.
void TestSystems::untrustedDefinitionsIgnored()
{
    QTemporaryDir tmp;
    const QString user = tmp.path() + QStringLiteral("/systems.d");
    QJsonObject custom = valid();
    custom.insert(QStringLiteral("label"), QStringLiteral("Evil"));
    write(user + QStringLiteral("/snes.json"), json(custom));
    QFile::setPermissions(user + QStringLiteral("/snes.json"),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther | QFile::WriteOther);
    SystemRegistry registry;
    registry.load({ SystemRegistry::kBuiltinDir, user }, tmp.path());
    QCOMPARE(registry.find(QStringLiteral("snes"))->label, QStringLiteral("SNES"));
    QCOMPARE(registry.errors().size(), 1);

    QFile::setPermissions(user + QStringLiteral("/snes.json"), QFile::ReadOwner | QFile::WriteOwner);
    QFile::setPermissions(user, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::WriteOther
                                    | QFile::ExeOther);
    registry.load({ SystemRegistry::kBuiltinDir, user }, tmp.path());
    QCOMPARE(registry.find(QStringLiteral("snes"))->label, QStringLiteral("SNES"));   // world-writable dir

    // Group-writable by our own (Ubuntu: private) primary group: fine; the
    // temp files are created with our primary gid.
    QFile::setPermissions(user, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup
                                    | QFile::WriteGroup | QFile::ExeGroup);
    QFile::setPermissions(user + QStringLiteral("/snes.json"),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::WriteGroup);
    registry.load({ SystemRegistry::kBuiltinDir, user }, tmp.path());
    QCOMPARE(registry.find(QStringLiteral("snes"))->label, QStringLiteral("Evil"));
}

void TestSystems::planRetroArch()
{
    QTemporaryDir tmp;
    LaunchEnv env;
    env.home = tmp.path() + QStringLiteral("/home");
    env.dataHome = env.home + QStringLiteral("/.local/share");
    env.stateHome = env.home + QStringLiteral("/.local/state");
    env.pathDirs = { tmp.path() + QStringLiteral("/bin") };
    env.flatpakInstallations = { tmp.path() + QStringLiteral("/flatpak") };
    env.retroarchCoreDirs = { tmp.path() + QStringLiteral("/cores") };
    const auto def = SystemRegistry::parse(json(valid()), env.home);
    QVERIFY(def);
    const EmulatorDef &snes9x = def->emulators.constFirst();
    const QString rom = env.home + QStringLiteral("/ROMs/snes/Game.sfc");

    // Nothing installed: install hint.
    LaunchPlan p = EmulatorLaunch::plan(*def, snes9x, rom, QStringLiteral("default"), env);
    QVERIFY(!p.ok());
    QCOMPARE(p.install, QStringLiteral("RetroArch"));
    QVERIFY(!EmulatorLaunch::available(snes9x, env));

    // Native RetroArch without the core: a core hint, no install.
    touchExecutable(tmp.path() + QStringLiteral("/bin/retroarch"));
    p = EmulatorLaunch::plan(*def, snes9x, rom, QStringLiteral("default"), env);
    QVERIFY(!p.ok());
    QVERIFY(p.install.isEmpty());
    QVERIFY(p.error.contains(QLatin1String("snes9x")));

    // Native with the core.
    write(tmp.path() + QStringLiteral("/cores/snes9x_libretro.so"), "x");
    QVERIFY(EmulatorLaunch::available(snes9x, env));
    p = EmulatorLaunch::plan(*def, snes9x, rom, QStringLiteral("default"), env);
    QVERIFY(p.ok());
    const QString cfg = env.stateHome + QStringLiteral("/mightyos/profiles/default/retroarch/snes.cfg");
    QCOMPARE(p.argv, (QStringList { tmp.path() + QStringLiteral("/bin/retroarch"), QStringLiteral("-L"),
                                     tmp.path() + QStringLiteral("/cores/snes9x_libretro.so"),
                                     QStringLiteral("--appendconfig"), cfg, rom }));
    QCOMPARE(p.configPath, cfg);
    QVERIFY(p.configContent.contains("savefile_directory = \"" + env.dataHome.toUtf8()
                                     + "/mightyos/profiles/default/saves/snes\""));
    // PLAN section 4: network features off for everybody; kiosk only for kids.
    QVERIFY(p.configContent.contains("network_cmd_enable = \"false\""));
    QVERIFY(p.configContent.contains("stdin_cmd_enable = \"false\""));
    QVERIFY(!p.configContent.contains("kiosk_mode_enable"));
    const LaunchPlan kids = EmulatorLaunch::plan(*def, snes9x, rom, QStringLiteral("default"), env, true);
    QVERIFY(kids.configContent.contains("kiosk_mode_enable = \"true\""));
    QVERIFY(kids.configContent.contains("menu_show_online_updater = \"false\""));
    QVERIFY(kids.configContent.contains("menu_show_load_content = \"false\""));

    // Flatpak RetroArch (native gone): saves inside its sandbox dir.
    QFile::remove(tmp.path() + QStringLiteral("/bin/retroarch"));
    touchExecutable(tmp.path() + QStringLiteral("/bin/flatpak"));
    QDir().mkpath(tmp.path() + QStringLiteral("/flatpak/app/org.libretro.RetroArch/current/active/files"));
    const QString raHome = env.home + QStringLiteral("/.var/app/org.libretro.RetroArch");
    write(raHome + QStringLiteral("/config/retroarch/cores/snes9x_libretro.so"), "x");
    QVERIFY(QDir().mkpath(env.home + QStringLiteral("/ROMs/snes")));
    p = EmulatorLaunch::plan(*def, snes9x, rom, QStringLiteral("kid1"), env);
    QVERIFY(p.ok());
    // S1: the cfg is ours (state dir), mounted read-only into the sandbox;
    // never written into the sandbox-writable ~/.var/app tree.
    // M6: strict sandbox (GPU + input only, no host/home), ROM folder read-only.
    const QString kidCfgDir = env.stateHome + QStringLiteral("/mightyos/profiles/kid1/retroarch");
    QCOMPARE(p.argv.mid(0, 19), (QStringList { tmp.path() + QStringLiteral("/bin/flatpak"), QStringLiteral("run"),
                                               QStringLiteral("--die-with-parent"), QStringLiteral("--nofilesystem=host:reset"),
                                               QStringLiteral("--filesystem=/run/udev:ro"),
                                               QStringLiteral("--nodevice=all"), QStringLiteral("--device=dri"),
                                               QStringLiteral("--device=input"), QStringLiteral("--nosocket=session-bus"),
                                               QStringLiteral("--nosocket=system-bus"), QStringLiteral("--nosocket=ssh-auth"),
                                               QStringLiteral("--nosocket=x11"), QStringLiteral("--nosocket=fallback-x11"),
                                               QStringLiteral("--unshare=network"),
                                               QStringLiteral("--filesystem=") + QFileInfo(env.home + QStringLiteral("/ROMs/snes")).canonicalFilePath() + QStringLiteral(":ro"),
                                               QStringLiteral("--filesystem=") + kidCfgDir + QStringLiteral(":ro"),
                                               QStringLiteral("org.libretro.RetroArch"), QStringLiteral("-L"),
                                               raHome + QStringLiteral("/config/retroarch/cores/snes9x_libretro.so") }));
    QCOMPARE(p.configPath, kidCfgDir + QStringLiteral("/snes.cfg"));
    QVERIFY(!p.configPath.startsWith(raHome));
    QCOMPARE(p.configArgv, (QStringList { QStringLiteral("--filesystem=") + kidCfgDir + QStringLiteral(":ro"),
                                          QStringLiteral("--appendconfig"), kidCfgDir + QStringLiteral("/snes.cfg") }));
    QCOMPARE(p.argv.last(), rom);
    QVERIFY(p.configContent.contains(raHome.toUtf8() + "/data/mightyos/profiles/kid1/saves/snes"));

    // Invalid profile ids never become paths.
    QVERIFY(!EmulatorLaunch::plan(*def, snes9x, rom, QStringLiteral("../x"), env).ok());
    QVERIFY(!EmulatorLaunch::plan(*def, snes9x, QStringLiteral("rel/Game.sfc"), QStringLiteral("default"), env).ok());
}

void TestSystems::planFlatpakAndNative()
{
    QTemporaryDir tmp;
    LaunchEnv env;
    env.home = tmp.path();
    env.dataHome = tmp.path() + QStringLiteral("/data");
    env.pathDirs = { tmp.path() + QStringLiteral("/bin") };
    env.flatpakInstallations = { tmp.path() + QStringLiteral("/flatpak") };
    SystemRegistry registry;
    registry.load({ SystemRegistry::kBuiltinDir }, tmp.path());
    const SystemDef *gc = registry.find(QStringLiteral("gamecube"));
    QVERIFY(gc);
    const EmulatorDef *dolphin = gc->emulator(QStringLiteral("dolphin"));
    const QString rom = tmp.path() + QStringLiteral("/ROMs/gamecube/G.rvz");
    LaunchPlan p = EmulatorLaunch::plan(*gc, *dolphin, rom, QStringLiteral("default"), env);
    QVERIFY(!p.ok());
    QCOMPARE(p.install, QStringLiteral("Dolphin"));
    touchExecutable(tmp.path() + QStringLiteral("/bin/flatpak"));
    QDir().mkpath(tmp.path() + QStringLiteral("/flatpak/app/org.DolphinEmu.dolphin-emu/current/active/files"));
    p = EmulatorLaunch::plan(*gc, *dolphin, rom, QStringLiteral("default"), env);
    QVERIFY(p.ok());
    // M8: Dolphin's user folder per profile, inside its own sandbox data dir.
    // No ROM folder on disk yet: nothing to mount, the sandbox is strict anyway.
    // Built-in Dolphin: "sandbox": "app" (GameCube adapter, Wiimotes).
    QVERIFY(dolphin->appSandbox);
    QCOMPARE(p.argv, (QStringList { tmp.path() + QStringLiteral("/bin/flatpak"), QStringLiteral("run"),
                                     QStringLiteral("--die-with-parent"),
                                     QStringLiteral("org.DolphinEmu.dolphin-emu"), QStringLiteral("-u"),
                                     tmp.path() + QStringLiteral("/.var/app/org.DolphinEmu.dolphin-emu/data/mightyos/profiles/default"),
                                     QStringLiteral("-b"), QStringLiteral("-e"), rom }));

    QJsonObject o = valid();
    o.insert(QStringLiteral("emulators"), QJsonArray { QJsonObject {
        { QStringLiteral("id"), QStringLiteral("x") }, { QStringLiteral("label"), QStringLiteral("X Emu") },
        { QStringLiteral("type"), QStringLiteral("native") }, { QStringLiteral("program"), QStringLiteral("xemu") },
        { QStringLiteral("args"), QJsonArray { QStringLiteral("--saves"), QStringLiteral("{saves}"), QStringLiteral("--"),
                                               QStringLiteral("{rom}") } } } });
    const auto def = SystemRegistry::parse(json(o), tmp.path());
    QVERIFY(def);
    p = EmulatorLaunch::plan(*def, def->emulators.constFirst(), rom, QStringLiteral("default"), env);
    QCOMPARE(p.install, QStringLiteral("X Emu"));
    touchExecutable(tmp.path() + QStringLiteral("/bin/xemu"));
    p = EmulatorLaunch::plan(*def, def->emulators.constFirst(), rom, QStringLiteral("default"), env);
    QCOMPARE(p.argv, (QStringList { tmp.path() + QStringLiteral("/bin/xemu"), QStringLiteral("--saves"),
                                     env.dataHome + QStringLiteral("/mightyos/profiles/default/saves/snes"),
                                     QStringLiteral("--"), rom }));
}

void TestSystems::planSteam()
{
    QTemporaryDir tmp;
    LaunchEnv env;
    env.home = tmp.path();
    env.pathDirs = { tmp.path() + QStringLiteral("/bin") };
    env.flatpakInstallations = { tmp.path() + QStringLiteral("/flatpak") };
    QCOMPARE(EmulatorLaunch::planSteam(QStringLiteral("620"), false, env).install, QStringLiteral("Steam"));
    QVERIFY(!EmulatorLaunch::planSteam(QStringLiteral("620; x"), false, env).ok());
    touchExecutable(tmp.path() + QStringLiteral("/bin/steam"));
    QCOMPARE(EmulatorLaunch::planSteam(QStringLiteral("620"), false, env).argv,
             (QStringList { tmp.path() + QStringLiteral("/bin/steam"), QStringLiteral("steam://rungameid/620") }));
    touchExecutable(tmp.path() + QStringLiteral("/bin/flatpak"));
    QDir().mkpath(tmp.path() + QStringLiteral("/flatpak/app/com.valvesoftware.Steam/current/active/files"));
    QCOMPARE(EmulatorLaunch::planSteam(QStringLiteral("620"), true, env).argv,
             (QStringList { tmp.path() + QStringLiteral("/bin/flatpak"), QStringLiteral("run"),
                            QStringLiteral("com.valvesoftware.Steam"), QStringLiteral("steam://rungameid/620") }));
    // S3: kids profiles close the client after the game.
    QCOMPARE(EmulatorLaunch::steamShutdownArgv(true, env),
             (QStringList { tmp.path() + QStringLiteral("/bin/flatpak"), QStringLiteral("run"),
                            QStringLiteral("com.valvesoftware.Steam"), QStringLiteral("-shutdown") }));
    QCOMPARE(EmulatorLaunch::steamShutdownArgv(false, env),
             (QStringList { tmp.path() + QStringLiteral("/bin/steam"), QStringLiteral("-shutdown") }));
}

void TestSystems::flatpakSandboxArgs()
{
    QTemporaryDir tmp;
    const QString base = QFileInfo(tmp.path()).canonicalFilePath();
    LaunchEnv env;
    env.home = base + QStringLiteral("/home/u");
    env.dataHome = env.home + QStringLiteral("/.local/share");
    env.stateHome = env.home + QStringLiteral("/.local/state");
    env.runtimeDir = env.home + QStringLiteral("/ROMs/run");   // pretend: inside the home
    env.user = QStringLiteral("u");
    SystemDef system;
    system.id = QStringLiteral("nes");
    const QString roms = env.home + QStringLiteral("/ROMs/nes");
    const QString usb = env.home + QStringLiteral("/USB stick/nes");
    for (const QString &d : { roms, usb, env.home + QStringLiteral("/odd:dir"), env.home + QStringLiteral("/.config/roms"),
                              env.runtimeDir, env.home + QStringLiteral("/ROMs/.hidden") })
        QVERIFY(QDir().mkpath(d));
    // A symlink to a forbidden place resolves to it and is refused (S2).
    QVERIFY(QFile::link(env.home + QStringLiteral("/.config"), env.home + QStringLiteral("/ROMs/cfg")));
    system.romDirs = { roms, usb, roms, env.home + QStringLiteral("/missing"), env.home + QStringLiteral("/odd:dir"),
                       env.home + QStringLiteral("/.config/roms"), env.home, env.runtimeDir,
                       env.home + QStringLiteral("/ROMs/.hidden"), env.home + QStringLiteral("/ROMs/cfg"),
                       QStringLiteral("/usr/share"), QStringLiteral("/tmp"), QStringLiteral("/") };
    QStringList rejected;
    const QStringList strict = EmulatorLaunch::flatpakSandboxArgs(system, roms + QStringLiteral("/Game.nes"), true, env, {},
                                                                  &rejected);
    QCOMPARE(strict, (QStringList { QStringLiteral("--die-with-parent"), QStringLiteral("--nofilesystem=host:reset"),
                                               QStringLiteral("--filesystem=/run/udev:ro"),
                                               QStringLiteral("--nodevice=all"), QStringLiteral("--device=dri"),
                                               QStringLiteral("--device=input"), QStringLiteral("--nosocket=session-bus"),
                                               QStringLiteral("--nosocket=system-bus"), QStringLiteral("--nosocket=ssh-auth"),
                                               QStringLiteral("--nosocket=x11"), QStringLiteral("--nosocket=fallback-x11"),
                                               QStringLiteral("--unshare=network"),
                                    QStringLiteral("--filesystem=") + roms + QStringLiteral(":ro"),
                                    QStringLiteral("--filesystem=") + usb + QStringLiteral(":ro") }));
    QCOMPARE(rejected.size(), 9);   // all but the two good ones and the missing one
    QString problem;
    QVERIFY(EmulatorLaunch::allowedRomDir(env.home, env, &problem).isEmpty());
    QVERIFY(problem.contains(QLatin1String("home folder")));
    QVERIFY(EmulatorLaunch::allowedRomDir(env.runtimeDir, env, &problem).isEmpty());
    QCOMPARE(problem, QStringLiteral("inside XDG_RUNTIME_DIR"));
    QVERIFY(EmulatorLaunch::allowedRomDir(QStringLiteral("/tmp"), env, &problem).isEmpty());
    QCOMPARE(EmulatorLaunch::allowedRomDir(roms + QStringLiteral("/../nes"), env, &problem), QString());   // not clean
    // "sandbox": "app": only the read-only ROM folders.
    QCOMPARE(EmulatorLaunch::flatpakSandboxArgs(system, roms + QStringLiteral("/Game.nes"), false, env),
             (QStringList { QStringLiteral("--die-with-parent"),
                            QStringLiteral("--filesystem=") + roms + QStringLiteral(":ro"),
                            QStringLiteral("--filesystem=") + usb + QStringLiteral(":ro") }));
    // A ROM outside every ROM folder: its own folder, if allowed (S2).
    const QString elsewhere = env.home + QStringLiteral("/Downloads");
    QVERIFY(QDir().mkpath(elsewhere));
    system.romDirs = { roms };
    QCOMPARE(EmulatorLaunch::flatpakSandboxArgs(system, elsewhere + QStringLiteral("/x.nes"), false, env),
             (QStringList { QStringLiteral("--die-with-parent"),
                            QStringLiteral("--filesystem=") + roms + QStringLiteral(":ro"),
                            QStringLiteral("--filesystem=") + elsewhere + QStringLiteral(":ro") }));
    rejected.clear();
    QVERIFY(!EmulatorLaunch::flatpakSandboxArgs(system, env.home + QStringLiteral("/.config/roms/x.nes"), false, env, {}, &rejected)
                 .join(u' ').contains(QLatin1String(".config")));
    QCOMPARE(rejected.size(), 1);
    // A prefix that isn't a parent folder doesn't count as covering it.
    const QString sibling = roms + QStringLiteral("2");
    QVERIFY(QDir().mkpath(sibling));
    QVERIFY(EmulatorLaunch::flatpakSandboxArgs(system, sibling + QStringLiteral("/x.nes"), false, env)
                .contains(QStringLiteral("--filesystem=") + sibling + QStringLiteral(":ro")));
    // Read-write dirs: only our own.
    const QString saves = env.dataHome + QStringLiteral("/mightyos/profiles/default/saves/nes");
    const QStringList rw = EmulatorLaunch::flatpakSandboxArgs(system, roms + QStringLiteral("/G.nes"), true, env,
                                                              { saves, env.home + QStringLiteral("/Documents") });
    QVERIFY(rw.contains(QStringLiteral("--filesystem=") + saves));
    QVERIFY(!rw.join(u' ').contains(QLatin1String("Documents")));
    // /media, /run/media/<user>, /mnt, /srv are fine too.
    if (QFileInfo(QStringLiteral("/mnt")).isDir() && QFileInfo(QStringLiteral("/mnt")).canonicalFilePath() == QLatin1String("/mnt"))
        QVERIFY(EmulatorLaunch::allowedRomDir(QStringLiteral("/mnt"), env, &problem).isEmpty());   // not itself
}

void TestSystems::sandboxKey()
{
    QTemporaryDir tmp;
    const auto withEmulator = [&](const QJsonObject &emulator) {
        QJsonObject o = valid();
        o.insert(QStringLiteral("emulators"), QJsonArray { emulator });
        return SystemRegistry::parse(json(o), tmp.path());
    };
    QJsonObject dolphin { { QStringLiteral("id"), QStringLiteral("d") }, { QStringLiteral("label"), QStringLiteral("D") },
                          { QStringLiteral("type"), QStringLiteral("flatpak") },
                          { QStringLiteral("app"), QStringLiteral("org.DolphinEmu.dolphin-emu") },
                          { QStringLiteral("args"), QJsonArray { QStringLiteral("{rom}") } } };
    auto def = withEmulator(dolphin);
    QVERIFY(def);
    QVERIFY(!def->emulators.constFirst().appSandbox);   // strict by default
    dolphin.insert(QStringLiteral("sandbox"), QStringLiteral("app"));
    def = withEmulator(dolphin);
    QVERIFY(def);
    QVERIFY(def->emulators.constFirst().appSandbox);   // parse() accepts it...
    // ...but a user layer may not relax the sandbox (S4).
    const QString userDir = tmp.path() + QStringLiteral("/config/mightyos/systems.d");
    QVERIFY(QDir().mkpath(userDir));
    QJsonObject o = valid();
    o.insert(QStringLiteral("emulators"), QJsonArray { dolphin });
    QFile file(userDir + QStringLiteral("/snes.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(json(o));
    file.close();
    SystemRegistry registry;
    registry.load({ SystemRegistry::kBuiltinDir, userDir }, tmp.path());
    const SystemDef *snes = registry.find(QStringLiteral("snes"));
    QVERIFY(snes && snes->source.startsWith(userDir));
    QVERIFY(!snes->emulators.constFirst().appSandbox);
    QVERIFY(!SystemRegistry::sandboxLayerTrusted(userDir, userDir + QStringLiteral("/snes.json"), tmp.path()));
    QVERIFY(SystemRegistry::sandboxLayerTrusted(SystemRegistry::kBuiltinDir, QString(), tmp.path()));
    registry.load({ SystemRegistry::kBuiltinDir }, tmp.path());
    QVERIFY(registry.find(QStringLiteral("gamecube"))->emulator(QStringLiteral("dolphin"))->appSandbox);
    dolphin.insert(QStringLiteral("sandbox"), QStringLiteral("none"));
    QVERIFY(!withEmulator(dolphin));
    dolphin.insert(QStringLiteral("sandbox"), true);
    QVERIFY(!withEmulator(dolphin));
    const QJsonObject native { { QStringLiteral("id"), QStringLiteral("n") }, { QStringLiteral("label"), QStringLiteral("N") },
                               { QStringLiteral("type"), QStringLiteral("native") }, { QStringLiteral("program"), QStringLiteral("n") },
                               { QStringLiteral("args"), QJsonArray { QStringLiteral("{rom}") } },
                               { QStringLiteral("sandbox"), QStringLiteral("app") } };
    QVERIFY(!withEmulator(native));   // Flatpak only
}

QTEST_GUILESS_MAIN(TestSystems)
#include "tst_systems.moc"
