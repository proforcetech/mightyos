#include "envguard.h"
#include "launcher.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>

#include <csignal>
#include <unistd.h>

class TestLauncher : public QObject
{
    Q_OBJECT

private slots:
    void environmentIsAllowlisted();
    void pathDropsRelativeEntries();
    void resolveProgramRejectsRelativePaths();
    void launchAndExit();
    void exitCodeIsReported();
    void crashIsReported();
    void childSeesFilteredEnvironment();
    void missingProgramFails();
    void secondLaunchWhileRunningFails();
    void emptyArgvFails();
    void terminateEndsTheProcessGroup();
    void terminateKillsWhatIgnoresSigterm();
    void terminateReachesMembersOutlivingTheLeader();
    void terminateRefusesTrackedLaunches();
    // M6: systemd user scopes.
    void scopeUnitNameIsSanitised();
    void scopeArgumentsShape();
    void acceptedScopeUnitIsOursOrFlatpaks();
    void launchRunsInAScope();
    void terminateKillsTheScope();
    void terminateEscalatesThroughTheScope();
    void staleScopesAreStopped();
    void flatpakAppIdOf();
    void terminateFollowsTheFlatpakScope();
    void anotherFlatpaksScopeIsNotTrusted();
    void failedSystemctlFallsBackToTheGroup();
    void systemdRunFailureLaunchesUnscoped();
    void realUserScope();
};

namespace {
// Never sees the app: the launch stays tracked until stopTracking().
class IdleTracker : public LaunchTracker
{
public:
    void start() override {}
    bool seen() const override { return false; }
};
} // namespace

static const QString kTestApp = QStringLiteral(MIGHTYOS_TESTAPP_PATH);

static QStringList testAppArgv(int quitAfterMs, int exitCode = 0)
{
    QStringList argv { kTestApp, QStringLiteral("-platform"), QStringLiteral("offscreen"),
                       QStringLiteral("--quit-after"), QString::number(quitAfterMs) };
    if (exitCode != 0)
        argv << QStringLiteral("--exit-code") << QString::number(exitCode);
    return argv;
}

void TestLauncher::environmentIsAllowlisted()
{
    QProcessEnvironment src;
    src.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    src.insert(QStringLiteral("HOME"), QStringLiteral("/home/u"));
    src.insert(QStringLiteral("USER"), QStringLiteral("u"));
    src.insert(QStringLiteral("LANG"), QStringLiteral("en_US.UTF-8"));
    src.insert(QStringLiteral("LC_TIME"), QStringLiteral("de_DE.UTF-8"));
    src.insert(QStringLiteral("XDG_RUNTIME_DIR"), QStringLiteral("/run/user/1000"));
    src.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-0"));
    src.insert(QStringLiteral("DISPLAY"), QStringLiteral(":0"));
    src.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=/x"));
    src.insert(QStringLiteral("PULSE_SERVER"), QStringLiteral("unix:/p"));
    src.insert(QStringLiteral("PIPEWIRE_REMOTE"), QStringLiteral("pw-0"));
    src.insert(QStringLiteral("LD_PRELOAD"), QStringLiteral("/tmp/evil.so"));
    src.insert(QStringLiteral("LD_LIBRARY_PATH"), QStringLiteral("/tmp"));
    src.insert(QStringLiteral("LD_AUDIT"), QStringLiteral("/tmp/a.so"));
    src.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    src.insert(QStringLiteral("SSH_AUTH_SOCK"), QStringLiteral("/tmp/agent"));
    src.insert(QStringLiteral("SOME_TOKEN"), QStringLiteral("secret"));

    const QProcessEnvironment env = Launcher::buildChildEnvironment(src);
    const QStringList expected {
        QStringLiteral("PATH"), QStringLiteral("HOME"), QStringLiteral("USER"),
        QStringLiteral("LANG"), QStringLiteral("LC_TIME"), QStringLiteral("XDG_RUNTIME_DIR"),
        QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("DISPLAY"),
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("PULSE_SERVER"),
        QStringLiteral("PIPEWIRE_REMOTE"),
    };
    QStringList keys = env.keys();
    keys.sort();
    QStringList want = expected;
    want.sort();
    QCOMPARE(keys, want);
    QCOMPARE(env.value(QStringLiteral("HOME")), QStringLiteral("/home/u"));
}

void TestLauncher::pathDropsRelativeEntries()
{
    QProcessEnvironment src;
    src.insert(QStringLiteral("PATH"), QStringLiteral(".:/usr/bin::bin:/usr/bin:/bin"));
    QCOMPARE(Launcher::buildChildEnvironment(src).value(QStringLiteral("PATH")),
             QStringLiteral("/usr/bin:/bin"));

    QProcessEnvironment none;
    QCOMPARE(Launcher::buildChildEnvironment(none).value(QStringLiteral("PATH")),
             QStringLiteral("/usr/local/bin:/usr/bin:/bin"));
}

void TestLauncher::resolveProgramRejectsRelativePaths()
{
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    QVERIFY(!Launcher::resolveProgram(QStringLiteral("sh"), env).isEmpty());
    QVERIFY(Launcher::resolveProgram(QStringLiteral("./sh"), env).isEmpty());
    QVERIFY(Launcher::resolveProgram(QStringLiteral("bin/sh"), env).isEmpty());
    QVERIFY(Launcher::resolveProgram(QStringLiteral("/nonexistent/prog"), env).isEmpty());
    QVERIFY(Launcher::resolveProgram(QStringLiteral("/etc/hostname"), env).isEmpty()); // not executable
    QVERIFY(Launcher::resolveProgram(QString(), env).isEmpty());
}

void TestLauncher::launchAndExit()
{
    Launcher launcher;
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    QSignalSpy failed(&launcher, &Launcher::failed);

    QVERIFY(launcher.launch(QStringLiteral("test:a"), testAppArgv(200)));
    QVERIFY(launcher.isRunning());
    QCOMPARE(launcher.currentId(), QStringLiteral("test:a"));

    QVERIFY(exited.wait(10000));
    QCOMPARE(launched.count(), 1);
    QCOMPARE(launched.first().at(0).toString(), QStringLiteral("test:a"));
    QCOMPARE(exited.first().at(0).toString(), QStringLiteral("test:a"));
    QCOMPARE(exited.first().at(1).toInt(), 0);
    QCOMPARE(failed.count(), 0);
    QVERIFY(!launcher.isRunning());
    QVERIFY(launcher.currentId().isEmpty());
}

void TestLauncher::exitCodeIsReported()
{
    Launcher launcher;
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("test:code"), testAppArgv(50, 3)));
    QVERIFY(exited.wait(10000));
    QCOMPARE(exited.first().at(1).toInt(), 3);
}

void TestLauncher::crashIsReported()
{
    Launcher launcher;
    QSignalSpy exited(&launcher, &Launcher::exited);
    QSignalSpy failed(&launcher, &Launcher::failed);
    // --crash calls abort() (with core dumps disabled): CrashExit -> -1.
    QVERIFY(launcher.launch(QStringLiteral("test:crash"), {
        kTestApp, QStringLiteral("-platform"), QStringLiteral("offscreen"), QStringLiteral("--crash")
    }));
    QVERIFY(exited.wait(10000));
    QCOMPARE(exited.first().at(0).toString(), QStringLiteral("test:crash"));
    QCOMPARE(exited.first().at(1).toInt(), -1);
    QCOMPARE(failed.count(), 0);
    QVERIFY(!launcher.isRunning());
}

void TestLauncher::childSeesFilteredEnvironment()
{
    const EnvGuard ldPath("LD_LIBRARY_PATH", "/nonexistent-mightyos-test");
    const EnvGuard secret("MIGHTYOS_TEST_SECRET", "hunter2");

    Launcher launcher;
    QSignalSpy exited(&launcher, &Launcher::exited);
    // The test itself uses sh as a probe; the Launcher never adds a shell.
    QVERIFY(launcher.launch(QStringLiteral("test:env"), {
        QStringLiteral("sh"), QStringLiteral("-c"),
        QStringLiteral("[ -z \"${LD_LIBRARY_PATH+x}\" ] && [ -z \"${MIGHTYOS_TEST_SECRET+x}\" ] && [ -n \"$PATH\" ]")
    }));
    QVERIFY(exited.wait(5000));
    QCOMPARE(exited.first().at(1).toInt(), 0);
}

void TestLauncher::missingProgramFails()
{
    Launcher launcher;
    QSignalSpy failed(&launcher, &Launcher::failed);
    QVERIFY(!launcher.launch(QStringLiteral("test:missing"),
                             { QStringLiteral("mightyos-definitely-not-installed") }));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.first().at(0).toString(), QStringLiteral("test:missing"));
    QVERIFY(!launcher.isRunning());
}

void TestLauncher::secondLaunchWhileRunningFails()
{
    Launcher launcher;
    QSignalSpy exited(&launcher, &Launcher::exited);
    QSignalSpy failed(&launcher, &Launcher::failed);
    QVERIFY(launcher.launch(QStringLiteral("test:first"), testAppArgv(300)));
    QVERIFY(!launcher.launch(QStringLiteral("test:second"), testAppArgv(10)));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.first().at(0).toString(), QStringLiteral("test:second"));
    QCOMPARE(launcher.currentId(), QStringLiteral("test:first"));
    QVERIFY(exited.wait(10000));
    QCOMPARE(exited.first().at(0).toString(), QStringLiteral("test:first"));
}

void TestLauncher::emptyArgvFails()
{
    Launcher launcher;
    QSignalSpy failed(&launcher, &Launcher::failed);
    QVERIFY(!launcher.launch(QStringLiteral("x"), {}));
    QVERIFY(!launcher.launch(QString(), { QStringLiteral("sh") }));
    QCOMPARE(failed.count(), 2);
}


void TestLauncher::terminateEndsTheProcessGroup()
{
    // Guide held on a web app / stream (M5): the whole group goes, and the
    // end counts as a normal exit, not a crash. (Without scopes, as on a
    // box without a systemd user manager.)
    Launcher launcher;
    launcher.setScopeTools({});
    QVERIFY(!launcher.terminate());   // nothing runs
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    // A wrapper with a child in the same group, like flatpak run -> bwrap.
    QVERIFY(launcher.launch(QStringLiteral("t"), { QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                                   QStringLiteral("sleep 60 & wait") }));
    QVERIFY(launched.wait(5000));
    QTest::qWait(200);
    QVERIFY(launcher.terminate());
    QVERIFY(exited.wait(5000));
    QCOMPARE(exited.at(0).at(0).toString(), QStringLiteral("t"));
    QCOMPARE(exited.at(0).at(1).toInt(), 0);
    QVERIFY(!launcher.isRunning());
}

void TestLauncher::terminateKillsWhatIgnoresSigterm()
{
    Launcher launcher;
    launcher.setScopeTools({});
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("stubborn"), { QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                                          QStringLiteral("trap '' TERM; sleep 60") }));
    QVERIFY(launched.wait(5000));
    QTest::qWait(200);
    QElapsedTimer timer;
    timer.start();
    QVERIFY(launcher.terminate());
    QVERIFY(exited.wait(Launcher::kTerminateGraceMs + 4000));
    QVERIFY2(timer.elapsed() >= Launcher::kTerminateGraceMs * 9 / 10, qPrintable(QString::number(timer.elapsed())));   // SIGKILL after the grace period
    QCOMPARE(exited.at(0).at(1).toInt(), 0);
}

void TestLauncher::terminateReachesMembersOutlivingTheLeader()
{
    // M4 (review): the leader exits on SIGTERM, a child that ignores it is
    // still in the group; the escalation is keyed by the pgid, not the
    // (already finished) QProcess.
    QTemporaryDir tmp;
    const QString pidFile = tmp.path() + QStringLiteral("/child.pid");
    Launcher launcher;
    launcher.setScopeTools({});
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    const QString script = QStringLiteral("sh -c 'trap \"\" TERM; echo $$ > %1; exec sleep 60' & wait").arg(pidFile);
    QVERIFY(launcher.launch(QStringLiteral("wrapper"), { QStringLiteral("/bin/sh"), QStringLiteral("-c"), script }));
    QVERIFY(launched.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(pidFile).size() > 0, 5000);
    QFile f(pidFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const pid_t child = static_cast<pid_t>(f.readAll().trimmed().toLongLong());
    QVERIFY(child > 0);
    QVERIFY(launcher.terminate());
    QVERIFY(launcher.terminate());                      // second call: no-op
    QVERIFY(exited.wait(2000));                         // the leader is gone at once
    QCOMPARE(exited.count(), 1);
    QCOMPARE(exited.at(0).at(1).toInt(), 0);
    QCOMPARE(::kill(child, 0), 0);                      // the child ignored SIGTERM
    // A new launch meanwhile doesn't cancel or receive the escalation.
    QVERIFY(launcher.launch(QStringLiteral("next"), { QStringLiteral("/bin/sleep"), QStringLiteral("10") }));
    QTRY_VERIFY_WITH_TIMEOUT(::kill(child, 0) != 0, Launcher::kTerminateGraceMs + 3000);
    QVERIFY(launcher.isRunning());
    QCOMPARE(launcher.currentId(), QStringLiteral("next"));
    QVERIFY(launcher.terminate());
    QTRY_VERIFY_WITH_TIMEOUT(!launcher.isRunning(), 5000);
}

void TestLauncher::terminateRefusesTrackedLaunches()
{
    // M3 (review): a tracked launch's process may be the Steam client.
    Launcher launcher;
    QSignalSpy launched(&launcher, &Launcher::launched);
    QVERIFY(launcher.launch(QStringLiteral("steam:1"), { QStringLiteral("/bin/sleep"), QStringLiteral("10") },
                            new IdleTracker));
    QVERIFY(launched.wait(5000));
    QVERIFY(launcher.isTracked());
    QVERIFY(!launcher.terminate());
    QVERIFY(launcher.isRunning());
    launcher.stopTracking();
    QVERIFY(!launcher.isRunning());
}

namespace {

const QString kFakeRun = QStringLiteral(MIGHTYOS_FAKE_SYSTEMD_RUN);
const QString kFakeCtl = QStringLiteral(MIGHTYOS_FAKE_SYSTEMCTL);

QString fakeDir()
{
    return qEnvironmentVariable("XDG_STATE_HOME") + QStringLiteral("/fake-systemd");
}

void clearFake()
{
    QDir(fakeDir()).removeRecursively();
}

// Each logged call as its argument list.
QList<QStringList> fakeLog(const char *tool)
{
    QList<QStringList> calls;
    QFile file(fakeDir() + QLatin1Char('/') + QLatin1String(tool) + QStringLiteral(".log"));
    if (!file.open(QIODevice::ReadOnly))
        return calls;
    for (const QByteArray &line : file.readAll().split('\n')) {
        if (!line.isEmpty())
            calls.append(QString::fromUtf8(line).split(QChar(0x1f)));
    }
    return calls;
}

const QString kFakeFlatpak = QStringLiteral(MIGHTYOS_FAKE_FLATPAK);

ScopeTools fakeTools()
{
    return ScopeTools { kFakeRun, kFakeCtl };
}

// The fake systemd-run creates no cgroup: /proc/<pid>/cgroup as the real
// one would read once the process is in `unit` (own: the launch's).
struct CgroupHook {
    explicit CgroupHook(const QByteArray &unit = QByteArray())
    {
        Launcher::setCgroupReaderForTests([unit](qint64, const QString &own) {
            return QByteArray("0::/user.slice/user-1000.slice/user@1000.service/app.slice/")
                   + (unit.isEmpty() ? own.toUtf8() : unit) + '\n';
        });
    }
    ~CgroupHook() { Launcher::setCgroupReaderForTests({}); }
};

void touchFlag(const char *name)
{
    QDir().mkpath(fakeDir());
    QFile flag(fakeDir() + QLatin1Char('/') + QLatin1String(name));
    QVERIFY(flag.open(QIODevice::WriteOnly));
}

} // namespace

void TestLauncher::scopeUnitNameIsSanitised()
{
    QCOMPARE(Launcher::scopeUnitName(QStringLiteral("flatpak:tv.kodi.Kodi"), 0xdeadbeef),
             QStringLiteral("app-mightyos-flatpak_tv.kodi.Kodi-deadbeef.scope"));
    // Separators, slashes, spaces, "-" and non-ASCII become "_" (a unit name is no path).
    QCOMPARE(Launcher::scopeUnitName(QStringLiteral("rom:nes:a/../b c-dé"), 1),
             QStringLiteral("app-mightyos-rom_nes_a_.._b_c_d_-00000001.scope"));
    QCOMPARE(Launcher::scopeUnitName(QString(), 0x10), QStringLiteral("app-mightyos-app-00000010.scope"));
    // Tracked (Steam) launches: a prefix the leftover cleanup never matches.
    QCOMPARE(Launcher::scopeUnitName(QStringLiteral("steam:620"), 3, true),
             QStringLiteral("app-mightyos.tracked-steam_620-00000003.scope"));
    QVERIFY(!Launcher::isRestartableUnit(Launcher::scopeUnitName(QStringLiteral("steam:620"), 3, true)));
    QVERIFY(Launcher::isRestartableUnit(Launcher::scopeUnitName(QStringLiteral("rom:nes:x"), 3)));
    const QString longName = Launcher::scopeUnitName(QString(200, QLatin1Char('x')), 2);
    QCOMPARE(longName, QStringLiteral("app-mightyos-") + QString(64, QLatin1Char('x')) + QStringLiteral("-00000002.scope"));
}

void TestLauncher::scopeArgumentsShape()
{
    const QStringList args = Launcher::scopeArguments(QStringLiteral("app-mightyos-x-1.scope"),
                                                      QStringLiteral("/usr/bin/prog"),
                                                      { QStringLiteral("--unit=evil"), QStringLiteral("a b") });
    QCOMPARE(args, (QStringList { QStringLiteral("--user"), QStringLiteral("--scope"), QStringLiteral("--quiet"),
                                  QStringLiteral("--collect"), QStringLiteral("--unit=app-mightyos-x-1.scope"),
                                  QStringLiteral("--description=MightyOS launch"),
                                  QStringLiteral("--property=TimeoutStopSec=3"), QStringLiteral("--"),
                                  QStringLiteral("/usr/bin/prog"), QStringLiteral("--unit=evil"),
                                  QStringLiteral("a b") }));   // the app's args come after "--"
}

void TestLauncher::acceptedScopeUnitIsOursOrFlatpaks()
{
    const QString own = QStringLiteral("app-mightyos-x-0000000a.scope");
    const QByteArray base = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/";
    QString path;
    QCOMPARE(Launcher::acceptedScopeUnit(base + own.toUtf8() + "\n", own, QString(), &path), own);
    QCOMPARE(path, QString::fromUtf8(base.mid(3)) + own);
    // Where `flatpak run <id>` moves itself (seen on the test PC): that app only (S6).
    const QByteArray ra = base + "app-flatpak-org.libretro.RetroArch-4112151741.scope\n";
    QCOMPARE(Launcher::acceptedScopeUnit(ra, own, QStringLiteral("org.libretro.RetroArch")),
             QStringLiteral("app-flatpak-org.libretro.RetroArch-4112151741.scope"));
    QVERIFY(Launcher::acceptedScopeUnit(ra, own).isEmpty());                                  // not a flatpak launch
    QVERIFY(Launcher::acceptedScopeUnit(ra, own, QStringLiteral("tv.kodi.Kodi")).isEmpty());  // another app
    QVERIFY(Launcher::acceptedScopeUnit(ra, own, QStringLiteral("org.libretro")).isEmpty());  // a prefix of it
    QCOMPARE(Launcher::acceptedScopeUnit(base + "app-flatpak-com.github.iwalton3.jellyfin-media-player-12.scope", own,
                                         QStringLiteral("com.github.iwalton3.jellyfin-media-player")),
             QStringLiteral("app-flatpak-com.github.iwalton3.jellyfin-media-player-12.scope"));
    QVERIFY(Launcher::acceptedScopeUnit(base + "app-flatpak-tv.kodi.Kodi-x1.scope", own, QStringLiteral("tv.kodi.Kodi"))
                .isEmpty());   // instance must be digits
    // Anything else is not ours to kill.
    QVERIFY(Launcher::acceptedScopeUnit(base + "app-mightyos-other-00000001.scope\n", own).isEmpty());
    QVERIFY(Launcher::acceptedScopeUnit("0::/user.slice/user-1000.slice/session-2.scope\n", own).isEmpty());
    QVERIFY(Launcher::acceptedScopeUnit(base + "labwc.scope\n", own).isEmpty());
    QVERIFY(Launcher::acceptedScopeUnit(base + "../app-flatpak-x.y.z-1.scope\n", own, QStringLiteral("x.y.z")).isEmpty());
    QVERIFY(Launcher::acceptedScopeUnit("0::/a/../" + own.toUtf8(), own).isEmpty());
    QVERIFY(Launcher::acceptedScopeUnit("1:name=systemd:/x/" + own.toUtf8() + "\n", own).isEmpty());
    QVERIFY(Launcher::acceptedScopeUnit(QByteArray(), own).isEmpty());
    QVERIFY(Launcher::acceptedScopeUnit("0::" + own.toUtf8(), QString()).isEmpty());   // not absolute
}

void TestLauncher::flatpakAppIdOf()
{
    QCOMPARE(Launcher::flatpakAppIdOf({ QStringLiteral("/usr/bin/flatpak"), QStringLiteral("run"),
                                        QStringLiteral("--die-with-parent"), QStringLiteral("--filesystem=/a:ro"),
                                        QStringLiteral("org.libretro.RetroArch"), QStringLiteral("-L"), QStringLiteral("x") }),
             QStringLiteral("org.libretro.RetroArch"));
    QCOMPARE(Launcher::flatpakAppIdOf({ QStringLiteral("flatpak"), QStringLiteral("run"), QStringLiteral("--"),
                                        QStringLiteral("tv.kodi.Kodi") }),
             QStringLiteral("tv.kodi.Kodi"));
    QVERIFY(Launcher::flatpakAppIdOf({ QStringLiteral("/usr/bin/flatpak"), QStringLiteral("info"), QStringLiteral("a.b.c") }).isEmpty());
    QVERIFY(Launcher::flatpakAppIdOf({ QStringLiteral("/usr/bin/kodi"), QStringLiteral("run"), QStringLiteral("a.b.c") }).isEmpty());
    QVERIFY(Launcher::flatpakAppIdOf({ QStringLiteral("flatpak"), QStringLiteral("run"), QStringLiteral("nodots") }).isEmpty());
    QVERIFY(Launcher::flatpakAppIdOf({ QStringLiteral("flatpak"), QStringLiteral("run"), QStringLiteral("a.b*.c") }).isEmpty());
}

void TestLauncher::launchRunsInAScope()
{
    clearFake();
    const CgroupHook hook;
    Launcher launcher;
    launcher.setScopeTools(fakeTools());
    QVERIFY(launcher.usesScopes());
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("flatpak:tv.kodi.Kodi"), testAppArgv(300, 3)));
    const QString unit = launcher.currentScopeUnit();
    QVERIFY(unit.startsWith(QStringLiteral("app-mightyos-flatpak_tv.kodi.Kodi-")));
    QVERIFY(unit.endsWith(QStringLiteral(".scope")));
    QVERIFY(launched.wait(5000));
    QVERIFY(exited.wait(10000));
    QCOMPARE(exited.at(0).at(1).toInt(), 3);   // the app's exit code, through systemd-run's exec
    QVERIFY(launcher.currentScopeUnit().isEmpty());
    const QList<QStringList> runs = fakeLog("systemd-run");
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.at(0), Launcher::scopeArguments(unit, kTestApp, testAppArgv(300, 3).mid(1)));
    // A second launch gets a fresh unit.
    QVERIFY(launcher.launch(QStringLiteral("flatpak:tv.kodi.Kodi"), testAppArgv(50)));
    QVERIFY(launcher.currentScopeUnit() != unit);
    QVERIFY(exited.wait(10000));
}

void TestLauncher::terminateKillsTheScope()
{
    clearFake();
    const CgroupHook hook;
    Launcher launcher;
    launcher.setScopeTools(fakeTools());
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("webapp:x"), { QStringLiteral("/bin/sleep"), QStringLiteral("60") }));
    const QString unit = launcher.currentScopeUnit();
    QVERIFY(launched.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(fakeDir() + QStringLiteral("/units/") + unit), 5000);
    QVERIFY(launcher.terminate());
    QVERIFY(exited.wait(5000));
    QCOMPARE(exited.at(0).at(1).toInt(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!fakeLog("systemctl").isEmpty(), 3000);
    const QStringList kill = fakeLog("systemctl").constFirst();
    QCOMPARE(kill, (QStringList { QStringLiteral("--user"), QStringLiteral("kill"), QStringLiteral("--signal=SIGTERM"),
                                  QStringLiteral("--"), unit }));
}

void TestLauncher::terminateEscalatesThroughTheScope()
{
    clearFake();
    const CgroupHook hook;
    Launcher launcher;
    launcher.setScopeTools(fakeTools());
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("stubborn"), { QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                                          QStringLiteral("trap '' TERM; sleep 60") }));
    const QString unit = launcher.currentScopeUnit();
    QVERIFY(launched.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(fakeDir() + QStringLiteral("/units/") + unit), 5000);
    QTest::qWait(200);
    QElapsedTimer timer;
    timer.start();
    QVERIFY(launcher.terminate());
    QVERIFY(exited.wait(Launcher::kTerminateGraceMs + 4000));
    QVERIFY2(timer.elapsed() >= Launcher::kTerminateGraceMs * 9 / 10, qPrintable(QString::number(timer.elapsed())));
    QTRY_VERIFY_WITH_TIMEOUT(fakeLog("systemctl").size() >= 2, 3000);
    const QList<QStringList> calls = fakeLog("systemctl");
    QCOMPARE(calls.at(0).at(2), QStringLiteral("--signal=SIGTERM"));
    QCOMPARE(calls.at(1), (QStringList { QStringLiteral("--user"), QStringLiteral("kill"),
                                         QStringLiteral("--signal=SIGKILL"), QStringLiteral("--"), unit }));
}

void TestLauncher::staleScopesAreStopped()
{
    clearFake();
    Launcher plain;
    plain.setScopeTools({});
    QVERIFY(!plain.stopStaleScopes());
    Launcher launcher;
    launcher.setScopeTools(fakeTools());
    // The recorded units of the last run, if they look like ours (S7);
    // never a tracked (Steam) launch's or anything else.
    QVERIFY(launcher.stopStaleScopes({ QStringLiteral("app-mightyos-rom_nes_x-0123abcd.scope"),
                                       QStringLiteral("app-flatpak-org.libretro.RetroArch-42.scope"),
                                       QStringLiteral("app-mightyos.tracked-steam_620-00000001.scope"),
                                       QStringLiteral("labwc.service"), QStringLiteral("--force") }));
    QTRY_VERIFY_WITH_TIMEOUT(!fakeLog("systemctl").isEmpty(), 3000);
    QCOMPARE(fakeLog("systemctl").constFirst(),
             (QStringList { QStringLiteral("--user"), QStringLiteral("stop"), QStringLiteral("--no-block"),
                            QStringLiteral("--"), QStringLiteral("app-mightyos-*.scope"),
                            QStringLiteral("app-mightyos-rom_nes_x-0123abcd.scope"),
                            QStringLiteral("app-flatpak-org.libretro.RetroArch-42.scope") }));
}

void TestLauncher::terminateFollowsTheFlatpakScope()
{
    // M2/S6: `flatpak run org.example.Emu` moved itself into
    // app-flatpak-org.example.Emu-77.scope; Close signals that unit, and the
    // escalation looks again and kills what is left.
    clearFake();
    const CgroupHook hook("app-flatpak-org.example.Emu-77.scope");
    Launcher launcher;
    launcher.setScopeTools(fakeTools());
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("rom:x:1"), { kFakeFlatpak, QStringLiteral("run"), QStringLiteral("--die-with-parent"),
                                                         QStringLiteral("org.example.Emu"), QStringLiteral("/bin/sh"),
                                                         QStringLiteral("-c"), QStringLiteral("trap '' TERM; sleep 60") }));
    const QString own = launcher.currentScopeUnit();
    QVERIFY(launched.wait(5000));
    // Both scopes are recorded for the next session start (S7).
    QTRY_COMPARE_WITH_TIMEOUT(launcher.activeUnits(),
                              (QStringList { own, QStringLiteral("app-flatpak-org.example.Emu-77.scope") }), 2000);
    QTest::qWait(200);
    QVERIFY(launcher.terminate());
    QVERIFY(exited.wait(Launcher::kTerminateGraceMs + 4000));
    QCOMPARE(exited.at(0).at(1).toInt(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(fakeLog("systemctl").size() >= 2, 3000);   // the helper may log after the exit
    const QList<QStringList> calls = fakeLog("systemctl");
    QCOMPARE(calls.at(0), (QStringList { QStringLiteral("--user"), QStringLiteral("kill"), QStringLiteral("--signal=SIGTERM"),
                                         QStringLiteral("--"), QStringLiteral("app-flatpak-org.example.Emu-77.scope") }));
    QCOMPARE(calls.at(1).at(2), QStringLiteral("--signal=SIGKILL"));
    QCOMPARE(calls.at(1).last(), QStringLiteral("app-flatpak-org.example.Emu-77.scope"));
    QVERIFY(launcher.activeUnits().isEmpty());
}

void TestLauncher::anotherFlatpaksScopeIsNotTrusted()
{
    // The process claims to be in another app's scope: never killed by
    // unit; our own unit and the process group instead.
    clearFake();
    const CgroupHook hook("app-flatpak-org.other.App-5.scope");
    Launcher launcher;
    launcher.setScopeTools(fakeTools());
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("rom:x:2"), { kFakeFlatpak, QStringLiteral("run"), QStringLiteral("org.example.Emu"),
                                                         QStringLiteral("/bin/sleep"), QStringLiteral("60") }));
    const QString own = launcher.currentScopeUnit();
    QVERIFY(launched.wait(5000));
    QTest::qWait(200);
    QVERIFY(launcher.terminate());
    QVERIFY(exited.wait(3000));   // the group's SIGTERM, at once
    QTRY_VERIFY_WITH_TIMEOUT(!fakeLog("systemctl").isEmpty(), 3000);
    for (const QStringList &call : fakeLog("systemctl"))
        QVERIFY2(!call.contains(QStringLiteral("app-flatpak-org.other.App-5.scope")), qPrintable(call.join(u' ')));
    QCOMPARE(fakeLog("systemctl").constFirst().last(), own);
}

void TestLauncher::failedSystemctlFallsBackToTheGroup()
{
    // M2: `systemctl kill` failing (manager gone) must not make Close a no-op.
    clearFake();
    touchFlag("fail-systemctl");
    const CgroupHook hook;
    Launcher launcher;
    launcher.setScopeTools(fakeTools());
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("webapp:y"), { QStringLiteral("/bin/sleep"), QStringLiteral("60") }));
    QVERIFY(launched.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(fakeDir() + QStringLiteral("/units/") + launcher.currentScopeUnit()), 5000);
    QElapsedTimer timer;
    timer.start();
    QVERIFY(launcher.terminate());
    QVERIFY(exited.wait(3000));
    QVERIFY2(timer.elapsed() < Launcher::kTerminateGraceMs, qPrintable(QString::number(timer.elapsed())));   // no wait for SIGKILL
    QCOMPARE(exited.at(0).at(1).toInt(), 0);
    clearFake();
}

void TestLauncher::systemdRunFailureLaunchesUnscoped()
{
    // systemd-run failing (never seen in its scope, non-zero exit): the app
    // never ran, so it is launched once more without a scope.
    clearFake();
    touchFlag("fail-systemd-run");
    Launcher launcher;
    launcher.setScopeTools(fakeTools());
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    QVERIFY(launcher.launch(QStringLiteral("t:fallback"), testAppArgv(200, 4)));
    QVERIFY(exited.wait(10000));
    QCOMPARE(exited.at(0).at(1).toInt(), 4);   // the app's own exit code: it did run
    QCOMPARE(launched.count(), 1);
    QVERIFY(!launcher.usesScopes());          // off for the rest of the run
    clearFake();
}

void TestLauncher::realUserScope()
{
    // On a box with a systemd user manager (the test PC): the real thing.
    const ScopeTools tools = ScopeTools::detect();
    if (!tools.isUsable())
        QSKIP("no systemd user manager (systemd-run --user unavailable)");
    Launcher launcher;
    QVERIFY(launcher.usesScopes());
    QSignalSpy launched(&launcher, &Launcher::launched);
    QSignalSpy exited(&launcher, &Launcher::exited);
    // The leader and its child both ignore SIGTERM: only the scope's
    // SIGKILL ends them.
    QVERIFY(launcher.launch(QStringLiteral("test:real"), { QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                                           QStringLiteral("trap '' TERM; sleep 60 & wait") }));
    const QString unit = launcher.currentScopeUnit();
    QVERIFY(launched.wait(5000));
    const auto activeState = [&] {
        QProcess show;
        show.start(tools.systemctl, { QStringLiteral("--user"), QStringLiteral("show"), QStringLiteral("-p"),
                                      QStringLiteral("ActiveState"), QStringLiteral("--value"), QStringLiteral("--"), unit });
        show.waitForFinished(3000);
        return QString::fromUtf8(show.readAllStandardOutput()).trimmed();
    };
    QTRY_COMPARE_WITH_TIMEOUT(activeState(), QStringLiteral("active"), 3000);
    QVERIFY(launcher.terminate());
    QVERIFY(exited.wait(Launcher::kTerminateGraceMs + 5000));
    QCOMPARE(exited.at(0).at(1).toInt(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(activeState() != QLatin1String("active"), 5000);
}

QTEST_GUILESS_MAIN(TestLauncher)
#include "tst_launcher.moc"
