#include "launcher.h"

#include <QSignalSpy>
#include <QTest>

class TestLauncher : public QObject
{
    Q_OBJECT

private slots:
    void environmentIsAllowlisted();
    void pathDropsRelativeEntries();
    void resolveProgramRejectsRelativePaths();
    void launchAndExit();
    void exitCodeIsReported();
    void childSeesFilteredEnvironment();
    void missingProgramFails();
    void secondLaunchWhileRunningFails();
    void emptyArgvFails();
};

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

void TestLauncher::childSeesFilteredEnvironment()
{
    qputenv("LD_LIBRARY_PATH", "/nonexistent-mightyos-test");
    qputenv("MIGHTYOS_TEST_SECRET", "hunter2");

    Launcher launcher;
    QSignalSpy exited(&launcher, &Launcher::exited);
    // The test itself uses sh as a probe; the Launcher never adds a shell.
    QVERIFY(launcher.launch(QStringLiteral("test:env"), {
        QStringLiteral("sh"), QStringLiteral("-c"),
        QStringLiteral("[ -z \"${LD_LIBRARY_PATH+x}\" ] && [ -z \"${MIGHTYOS_TEST_SECRET+x}\" ] && [ -n \"$PATH\" ]")
    }));
    QVERIFY(exited.wait(5000));
    QCOMPARE(exited.first().at(1).toInt(), 0);

    qunsetenv("LD_LIBRARY_PATH");
    qunsetenv("MIGHTYOS_TEST_SECRET");
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

QTEST_GUILESS_MAIN(TestLauncher)
#include "tst_launcher.moc"
