#include "envguard.h"
#include "launcher.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>

#include <csignal>

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
    // end counts as a normal exit, not a crash.
    Launcher launcher;
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

QTEST_GUILESS_MAIN(TestLauncher)
#include "tst_launcher.moc"
