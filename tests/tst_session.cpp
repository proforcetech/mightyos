// M6 session pieces: the crash counter (SessionState), the group check,
// SignalWatcher, ProcessHardening (and that launched apps get their core
// limit back), the Session singleton's dormant / group-warning state and
// the Guide overlay's fallback off Wayland.
#include "guideoverlayhost.h"
#include "launcher.h"
#include "processhardening.h"
#include "session.h"
#include "sessionstate.h"
#include "signalwatcher.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QXmlStreamReader>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

class TestSession : public QObject
{
    Q_OBJECT

private slots:
    void crashesAreWhatTheLoopSaw();
    void evaluateDropsOldFutureAndOtherBoots();
    void recordStartAndUp();
    void safeModeAfterRepeatedCrashes();
    void logoutIsNotACrash();
    void corruptStateIsMovedAside();
    void stateFileIsPrivateAndNoSymlinks();
    void warnedGroupsAndUnitsPersist();
    void riskyGroups();
    void sessionLockAndSignal();
    void signalsReachTheEventLoop();
    void sessionDormantReleasesCaches();
    void groupWarningIsAcknowledgedOnce();
    void overlayFallsBackOffWayland();
    void labwcConfigIsLockedDown();
    void sessionScriptRefusesCommands();
    void restartLoopPassesTheStatus();
    void restartLoopGivesUp();
    void mightyosCountsThePreviousStatus();
    // Last: it makes the test process non-dumpable.
    void coreDumpsOffAndRestoredForChildren();
};

const QString kBoot = QStringLiteral("boot-a");

void TestSession::crashesAreWhatTheLoopSaw()
{
    // H1: only the restart loop's view counts.
    QVERIFY(!SessionState::isCrash(-1, false));   // first start of a session
    QVERIFY(!SessionState::isCrash(0, false));
    QVERIFY(SessionState::isCrash(139, true));    // SIGSEGV
    QVERIFY(SessionState::isCrash(137, true));    // SIGKILL / OOM
    QVERIFY(SessionState::isCrash(1, false));     // failed during startup
    QVERIFY(!SessionState::isCrash(1, true));     // Qt's _exit(1): the compositor went away
    QJsonObject state;
    SessionState::Start start = SessionState::evaluate(state, 1000, kBoot, -1);
    QVERIFY(!start.previousCrashed);
    QCOMPARE(start.recentCrashes, 0);
    QCOMPARE(state.value(QStringLiteral("up")).toBool(true), false);
    QCOMPARE(state.value(QStringLiteral("boot")).toString(), kBoot);
    QCOMPARE(state.value(QStringLiteral("version")).toInt(), SessionState::kVersion);
    start = SessionState::evaluate(state, 1010, kBoot, 134);
    QVERIFY(start.previousCrashed);
    QCOMPARE(start.recentCrashes, 1);
    QCOMPARE(state.value(QStringLiteral("crashes")).toArray().at(0).toDouble(), 1010.0);
}

void TestSession::evaluateDropsOldFutureAndOtherBoots()
{
    const qint64 now = 100000;
    QJsonObject state {
        { QStringLiteral("version"), SessionState::kVersion }, { QStringLiteral("boot"), kBoot },
        { QStringLiteral("crashes"), QJsonArray { double(now - SessionState::kWindowSecs - 1), double(now - 10),
                                                  double(now + 3600), QStringLiteral("x"), -5.0 } },
    };
    QJsonObject copy = state;
    SessionState::Start start = SessionState::evaluate(state, now, kBoot, -1);
    QCOMPARE(start.recentCrashes, 1);
    QCOMPARE(state.value(QStringLiteral("crashes")).toArray(), (QJsonArray { double(now - 10) }));
    // Another boot: the old list is meaningless (boot-relative times).
    start = SessionState::evaluate(copy, now, QStringLiteral("boot-b"), -1);
    QCOMPARE(start.recentCrashes, 0);
    // Capped, oldest dropped.
    QJsonArray many;
    for (int i = 0; i < 40; ++i)
        many.append(double(now - 40 + i));
    QJsonObject big { { QStringLiteral("version"), SessionState::kVersion }, { QStringLiteral("boot"), kBoot },
                      { QStringLiteral("crashes"), many } };
    SessionState::evaluate(big, now, kBoot, -1);
    QCOMPARE(big.value(QStringLiteral("crashes")).toArray().size(), SessionState::kMaxCrashEntries);
    QCOMPARE(big.value(QStringLiteral("crashes")).toArray().last().toDouble(), double(now - 1));
}

void TestSession::recordStartAndUp()
{
    QTemporaryDir tmp;
    SessionState state(tmp.path());
    QCOMPARE(state.filePath(), tmp.path() + QStringLiteral("/mightyos/session.json"));
    QVERIFY(!state.recordStart(500, kBoot, -1).previousCrashed);
    // Exit 1 during startup (never marked up): a crash.
    QVERIFY(state.recordStart(510, kBoot, 1).previousCrashed);
    QVERIFY(state.markUp());
    // Exit 1 after startup: not one.
    const SessionState::Start start = state.recordStart(520, kBoot, 1);
    QVERIFY(!start.previousCrashed);
    QCOMPARE(start.recentCrashes, 1);
    // The real clock and boot id work.
    QVERIFY(SessionState::bootSeconds() > 0);
    QVERIFY(!SessionState::bootId().isEmpty());
    QVERIFY(!state.recordStart(-1).previousCrashed);
}

void TestSession::safeModeAfterRepeatedCrashes()
{
    QTemporaryDir tmp;
    SessionState state(tmp.path());
    qint64 t = 10000;
    state.recordStart(t, kBoot, -1);
    for (int i = 1; i < SessionState::kSafeModeCrashes; ++i) {
        state.markUp();
        QVERIFY(!state.recordStart(t += 2, kBoot, 139).safeMode);
    }
    const SessionState::Start start = state.recordStart(t += 2, kBoot, 139);
    QCOMPARE(start.recentCrashes, SessionState::kSafeModeCrashes);
    QVERIFY(start.safeMode);
    // A clean restart in the same window stays in safe mode...
    QVERIFY(state.recordStart(t += 2, kBoot, -1).safeMode);
    // ...until the crashes expire.
    QVERIFY(!state.recordStart(t + SessionState::kWindowSecs + 1, kBoot, -1).safeMode);
}

void TestSession::logoutIsNotACrash()
{
    // H1: every logout ends MightyOS with Qt's _exit(1) after startup; the
    // next session starts without --previous-status. Many logouts: no safe mode.
    QTemporaryDir tmp;
    SessionState state(tmp.path());
    qint64 t = 5000;
    for (int i = 0; i < 6; ++i) {
        const SessionState::Start start = state.recordStart(t += 30, kBoot, -1);
        QVERIFY(!start.previousCrashed);
        QVERIFY(!start.safeMode);
        state.markUp();
    }
}

void TestSession::corruptStateIsMovedAside()
{
    QTemporaryDir tmp;
    QVERIFY(QDir().mkpath(tmp.path() + QStringLiteral("/mightyos")));
    const QString path = tmp.path() + QStringLiteral("/mightyos/session.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ not json");
    file.close();
    SessionState state(tmp.path());
    const SessionState::Start start = state.recordStart(1000, kBoot, -1);
    QVERIFY(!start.previousCrashed);
    QVERIFY(QFile::exists(path + QStringLiteral(".corrupt")));
    QVERIFY(QFile::exists(path));
}

void TestSession::stateFileIsPrivateAndNoSymlinks()
{
    QTemporaryDir tmp;
    SessionState state(tmp.path());
    state.recordStart(1000, kBoot, -1);
    struct stat st {};
    QCOMPARE(::stat(QFile::encodeName(state.filePath()).constData(), &st), 0);
    QCOMPARE(int(st.st_mode & 0777), 0600);
    // A symlink planted at the name is never followed (read or written).
    QTemporaryDir other;
    const QString target = other.path() + QStringLiteral("/victim.json");
    QFile victim(target);
    QVERIFY(victim.open(QIODevice::WriteOnly));
    victim.write(R"({"version":2,"boot":"boot-a","crashes":[995,996,997]})");
    victim.close();
    QVERIFY(QFile::remove(state.filePath()));
    QVERIFY(QFile::link(target, state.filePath()));
    QVERIFY(!state.recordStart(1000, kBoot, -1).safeMode);   // the target's crashes aren't read
    QVERIFY(victim.open(QIODevice::ReadOnly));
    QVERIFY(victim.readAll().contains("995"));               // and it isn't written through
    QVERIFY(!QFileInfo(state.filePath()).isSymLink());
}

void TestSession::warnedGroupsAndUnitsPersist()
{
    QTemporaryDir tmp;
    SessionState state(tmp.path());
    QVERIFY(state.warnedGroups().isEmpty());
    QVERIFY(state.setWarnedGroups({ QStringLiteral("sudo"), QStringLiteral("video") }));
    QCOMPARE(state.warnedGroups(), (QStringList { QStringLiteral("sudo"), QStringLiteral("video") }));
    QVERIFY(state.setLaunchUnits({ QStringLiteral("app-mightyos-a-00000001.scope") }));
    state.recordStart(1, kBoot, -1);   // keeps the other keys
    QCOMPARE(state.warnedGroups().size(), 2);
    QCOMPARE(state.launchUnits(), QStringList { QStringLiteral("app-mightyos-a-00000001.scope") });
    QStringList many;
    for (int i = 0; i < 20; ++i)
        many.append(QStringLiteral("u%1").arg(i));
    state.setLaunchUnits(many);
    QCOMPARE(state.launchUnits().size(), SessionState::kMaxUnits);
}

void TestSession::riskyGroups()
{
    QCOMPARE(SessionState::riskyGroups({ QStringLiteral("aaron"), QStringLiteral("video"), QStringLiteral("sudo"),
                                         QStringLiteral("plugdev"), QStringLiteral("input"), QStringLiteral("sudo"),
                                         QStringLiteral("render"), QStringLiteral("audio"), QStringLiteral("lxd"),
                                         QStringLiteral("docker"), QStringLiteral("libvirt"), QStringLiteral("incus-admin"),
                                         QStringLiteral("adm") }),
             (QStringList { QStringLiteral("docker"), QStringLiteral("incus-admin"), QStringLiteral("input"),
                            QStringLiteral("libvirt"), QStringLiteral("lxd"), QStringLiteral("sudo"),
                            QStringLiteral("video") }));
    QCOMPARE(SessionState::informationalGroups({ QStringLiteral("adm"), QStringLiteral("systemd-journal"),
                                                 QStringLiteral("sudo") }),
             (QStringList { QStringLiteral("adm"), QStringLiteral("systemd-journal") }));
    QVERIFY(SessionState::riskyGroups({ QStringLiteral("users") }).isEmpty());
    // This process has at least its primary group.
    QVERIFY(!SessionState::currentGroupNames().isEmpty());
}

void TestSession::sessionLockAndSignal()
{
    // S9 / M3: $XDG_RUNTIME_DIR/mightyos/session.pid, flock()ed; only one
    // holder; `mightyos --signal` reaches only a locked pid that is a
    // process named mightyos.
    QTemporaryDir runtime;
    QVERIFY(QFile::setPermissions(runtime.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(!SessionLock::signalSession(runtime.path(), SIGUSR1).isEmpty());   // no session
    {
        SessionLock lock;
        QString error;
        QVERIFY2(lock.acquire(runtime.path(), QCoreApplication::applicationPid(), &error), qPrintable(error));
        QVERIFY(lock.isHeld());
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(SessionLock::pidFilePath(runtime.path())).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        QCOMPARE(::stat(QFile::encodeName(runtime.path() + QStringLiteral("/mightyos")).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0700);
        SessionLock second;
        QVERIFY(!second.acquire(runtime.path(), 1234, &error));   // the second instance
        QVERIFY(error.contains(QLatin1String("another")));
        // Locked, but this test process isn't named mightyos: refused.
        QVERIFY(SessionLock::signalSession(runtime.path(), SIGUSR1).contains(QLatin1String("not MightyOS")));
    }
    // Released: the file stays, stale.
    QVERIFY(QFile::exists(SessionLock::pidFilePath(runtime.path())));
    QVERIFY(SessionLock::signalSession(runtime.path(), SIGUSR1).contains(QLatin1String("stale")));
    // A symlink instead of the file is never followed.
    QVERIFY(QFile::remove(SessionLock::pidFilePath(runtime.path())));
    QTemporaryDir other;
    QVERIFY(QFile::link(other.path() + QStringLiteral("/x"), SessionLock::pidFilePath(runtime.path())));
    SessionLock viaLink;
    QVERIFY(!viaLink.acquire(runtime.path(), 1));
    QVERIFY(!QFile::exists(other.path() + QStringLiteral("/x")));
    // A group/other-accessible session dir is refused.
    QVERIFY(QFile::setPermissions(runtime.path() + QStringLiteral("/mightyos"),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther | QFile::ExeOther));
    SessionLock open;
    QVERIFY(!open.acquire(runtime.path(), 1));
}

void TestSession::signalsReachTheEventLoop()
{
    {
        SignalWatcher watcher({ SIGUSR1, SIGUSR2, SIGTERM });
        QVERIFY(watcher.isActive());
        // One per process.
        SignalWatcher second({ SIGHUP });
        QVERIFY(!second.isActive());
        QSignalSpy spy(&watcher, &SignalWatcher::received);
        ::raise(SIGUSR1);
        ::raise(SIGTERM);   // handled, so the test survives it
        ::raise(SIGUSR2);
        QTRY_COMPARE(spy.count(), 3);
        QCOMPARE(spy.at(0).at(0).toInt(), SIGUSR1);
        QCOMPARE(spy.at(1).at(0).toInt(), SIGTERM);
        QCOMPARE(spy.at(2).at(0).toInt(), SIGUSR2);
    }
    // Gone: the previous disposition (the default) is back.
    struct sigaction current {};
    QCOMPARE(::sigaction(SIGUSR1, nullptr, &current), 0);
    QVERIFY(current.sa_handler == SIG_DFL);
    SignalWatcher again({ SIGUSR1 });
    QVERIFY(again.isActive());
}

void TestSession::sessionDormantReleasesCaches()
{
    Session session;
    QSignalSpy dormant(&session, &Session::dormantChanged);
    session.setDormant(true);
    QVERIFY(session.isDormant());
    session.setDormant(true);
    QCOMPARE(dormant.count(), 1);
    session.setDormant(false);
    QCOMPARE(dormant.count(), 2);
    QCOMPARE(session.releaseCaches(), qsizetype(0));   // nothing attached: harmless
}

void TestSession::groupWarningIsAcknowledgedOnce()
{
    Session session;
    QSignalSpy acknowledged(&session, &Session::groupWarningAcknowledged);
    Session::Options options;
    options.sessionMode = true;
    options.riskyGroups = { QStringLiteral("sudo") };
    options.groupWarning = true;
    session.configure(options);
    QVERIFY(session.groupWarning());
    session.acknowledgeGroupWarning();
    QVERIFY(!session.groupWarning());
    session.acknowledgeGroupWarning();
    QCOMPARE(acknowledged.count(), 1);
    QCOMPARE(acknowledged.at(0).at(0).toStringList(), QStringList { QStringLiteral("sudo") });
}

void TestSession::overlayFallsBackOffWayland()
{
    // Offscreen (tests): no Wayland, so the in-window menu.
    QVERIFY(!GuideOverlayHost::compositorHasLayerShell());
    GuideOverlayHost host(nullptr, nullptr, nullptr);
    QVERIFY(!host.isAvailable());
    QVERIFY(!host.show());
    QVERIFY(!host.isShown());
    Session session;
    session.setOverlayHost(&host);
    QVERIFY(!session.isOverlayAvailable());
    QVERIFY(!session.showOverlay());
}

namespace {

// Checks one generated rc.xml; returns the Execute commands.
QSet<QString> lockedDownCommands(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return { QStringLiteral("<missing>") };
    QXmlStreamReader xml(&file);
    int keybinds = 0, mousebinds = 0, maximizeRules = 0;
    QStringList stack;
    QSet<QString> commands;
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::EndElement) {
            stack.removeLast();
            continue;
        }
        if (!xml.isStartElement())
            continue;
        const QString name = xml.name().toString();
        if (name == QLatin1String("default"))
            commands.insert(QStringLiteral("<default>"));
        if (name == QLatin1String("keybind"))
            ++keybinds;
        if (name == QLatin1String("mousebind"))
            ++mousebinds;
        if (name == QLatin1String("action")) {
            const QString action = xml.attributes().value(QStringLiteral("name")).toString();
            if (stack.contains(QLatin1String("windowRule"))) {
                if (action != QLatin1String("Maximize"))
                    commands.insert(QStringLiteral("<rule action %1>").arg(action));
                ++maximizeRules;
            } else if (action == QLatin1String("Execute")) {
                commands.insert(xml.attributes().value(QStringLiteral("command")).toString());
            } else if (action != QLatin1String("Focus") && action != QLatin1String("Raise")) {
                commands.insert(QStringLiteral("<action %1>").arg(action));   // Exit, Close, ShowMenu, ...
            }
        }
        stack.append(name);
    }
    if (xml.hasError())
        commands.insert(QStringLiteral("<xml error %1>").arg(xml.errorString()));
    if (keybinds < 1 || mousebinds < 1 || maximizeRules != 1)
        commands.insert(QStringLiteral("<defaults would load>"));
    return commands;
}

} // namespace

void TestSession::labwcConfigIsLockedDown()
{
    // The generated rc.xml files: well-formed (labwc silently falls back to
    // its defaults on a parse error), no <default/>, at least one keybind and
    // one mousebind (none at all also loads the defaults), Focus/Raise only,
    // and exactly the three panic commands, by absolute path to this
    // build's (or the installed) mightyos.
    const QString build = QStringLiteral(MIGHTYOS_BUILD_DIR);
    const auto expected = [](const QString &bin) {
        return QSet<QString> { bin + QStringLiteral(" --signal=guide"), bin + QStringLiteral(" --signal=close"),
                               bin + QStringLiteral(" --signal=quit") };
    };
    QCOMPARE(lockedDownCommands(build + QStringLiteral("/session/labwc/rc.xml")), expected(build + QStringLiteral("/mightyos")));
    QCOMPARE(lockedDownCommands(build + QStringLiteral("/session/install/labwc/rc.xml")),
             expected(QStringLiteral(MIGHTYOS_INSTALLED_BIN)));

    QFile menu(QStringLiteral(MIGHTYOS_SOURCE_DIR "/session/labwc/menu.xml"));
    QVERIFY(menu.open(QIODevice::ReadOnly));
    QXmlStreamReader menuXml(&menu);
    while (!menuXml.atEnd()) {
        menuXml.readNext();
        if (menuXml.isStartElement())
            QVERIFY2(menuXml.name() != QLatin1String("item"), "the compositor menus stay empty");
    }
    QVERIFY(!menuXml.hasError());
}

namespace {

const QString kTestScript = QStringLiteral(MIGHTYOS_BUILD_DIR "/session/test/mightyos-session");

QString fakeDir()
{
    return qEnvironmentVariable("XDG_STATE_HOME") + QStringLiteral("/fake-systemd");
}

QList<QStringList> fakeLog(const char *tool)
{
    QList<QStringList> calls;
    QFile file(fakeDir() + QLatin1Char('/') + QLatin1String(tool) + QStringLiteral(".log"));
    if (file.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (!line.isEmpty())
                calls.append(QString::fromUtf8(line).split(QChar(0x1f)));
        }
    }
    return calls;
}

int runScript(const QStringList &args, int timeoutMs = 20000)
{
    QProcess sh;
    sh.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    sh.start(kTestScript, args);
    if (!sh.waitForFinished(timeoutMs)) {
        sh.kill();
        return -100;
    }
    return sh.exitStatus() == QProcess::NormalExit ? sh.exitCode() : -1;
}

} // namespace

void TestSession::sessionScriptRefusesCommands()
{
    for (const QString &script : { QStringLiteral(MIGHTYOS_BUILD_DIR "/session/mightyos-session"),
                                   QStringLiteral(MIGHTYOS_BUILD_DIR "/session/install/mightyos-session"), kTestScript }) {
        QProcess sh;
        sh.start(QStringLiteral("/bin/sh"), { QStringLiteral("-n"), script });
        QVERIFY(sh.waitForFinished(5000));
        QCOMPARE(sh.exitCode(), 0);
        QFile file(script);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray text = file.readAll();
        QVERIFY2(!text.contains("@MIGHTYOS_") && !text.contains("@CMAKE_"), qPrintable(script));
        QVERIFY(text.contains("PATH=/usr/bin:/bin"));
        // The installed script takes no overrides from the environment (S10).
        if (script.contains(QLatin1String("/install/")))
            QVERIFY(!text.contains("MIGHTYOS_LABWC_DIR"));
    }
    // As a login shell it never runs a command it is given (S1).
    QDir(fakeDir()).removeRecursively();
    QCOMPARE(runScript({ QStringLiteral("-c"), QStringLiteral("touch /tmp/x") }), 1);
    QCOMPARE(runScript({ QStringLiteral("--bogus") }), 1);
    QVERIFY(fakeLog("labwc").isEmpty());
}

void TestSession::restartLoopPassesTheStatus()
{
    // H1: fake labwc runs the script's --run; fake mightyos exits 1, then
    // 139 (a crash), then 0. Each restart says how the last run ended; 0
    // ends the session.
    QDir(fakeDir()).removeRecursively();
    QVERIFY(QDir().mkpath(fakeDir()));
    QFile plan(fakeDir() + QStringLiteral("/plan"));
    QVERIFY(plan.open(QIODevice::WriteOnly));
    plan.write("1\n139\n0\n");
    plan.close();
    QCOMPARE(runScript({}), 0);
    const QList<QStringList> labwc = fakeLog("labwc");
    QCOMPARE(labwc.size(), 1);
    QCOMPARE(labwc.at(0).at(0), QStringLiteral("-C"));
    QCOMPARE(labwc.at(0).at(3), QStringLiteral("'") + kTestScript + QStringLiteral("' --run"));
    const QList<QStringList> runs = fakeLog("mightyos");
    QCOMPARE(runs.size(), 3);
    QCOMPARE(runs.at(0), QStringList { QStringLiteral("--session-mode") });
    QCOMPARE(runs.at(1), (QStringList { QStringLiteral("--session-mode"), QStringLiteral("--previous-status=1") }));
    QCOMPARE(runs.at(2), (QStringList { QStringLiteral("--session-mode"), QStringLiteral("--previous-status=139") }));
}

void TestSession::restartLoopGivesUp()
{
    // Ten quick failures in a row end the session with the last status.
    QDir(fakeDir()).removeRecursively();
    QVERIFY(QDir().mkpath(fakeDir()));
    QFile plan(fakeDir() + QStringLiteral("/plan"));
    QVERIFY(plan.open(QIODevice::WriteOnly));
    for (int i = 0; i < 12; ++i)
        plan.write("3\n");
    plan.close();
    QCOMPARE(runScript({}), 3);
    QCOMPARE(fakeLog("mightyos").size(), 10);
}

void TestSession::mightyosCountsThePreviousStatus()
{
    // End to end with the real binary (offscreen): the status the loop
    // passes reaches the crash counter. (QGuiApplication eats "--session
    // <id>", which once swallowed --previous-status; hence --session-mode.)
    QTemporaryDir tmp;
    const QString runtime = tmp.path() + QStringLiteral("/run");
    QVERIFY(QDir().mkpath(runtime));
    QVERIFY(QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_STATE_HOME"), tmp.path());
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    QByteArray before;
    const auto runOnce = [&](const QStringList &args) {
        QProcess app;
        app.setProcessEnvironment(env);
        app.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        app.start(QStringLiteral(MIGHTYOS_BIN), QStringList { QStringLiteral("--session-mode"), QStringLiteral("--no-gamepad"),
                                                              QStringLiteral("--no-cec") } + args);
        if (!app.waitForStarted(5000))
            return QByteArray();
        // Up (the window exists) and holding the lock: then quit cleanly.
        QElapsedTimer timer;
        timer.start();
        QByteArray state;
        while (timer.elapsed() < 20000) {
            QFile file(tmp.path() + QStringLiteral("/mightyos/session.json"));
            if (file.open(QIODevice::ReadOnly) && (state = file.readAll()) != before && state.contains("\"up\":true"))
                break;
            QTest::qWait(100);
        }
        app.terminate();
        app.waitForFinished(10000);
        QFile file(tmp.path() + QStringLiteral("/mightyos/session.json"));
        before = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        return state;
    };
    QByteArray state = runOnce({});
    QVERIFY2(state.contains("\"crashes\":[]"), state.constData());
    state = runOnce({ QStringLiteral("--previous-status=137") });
    QVERIFY2(!state.contains("\"crashes\":[]"), state.constData());   // one crash recorded
    QVERIFY(QFile::exists(runtime + QStringLiteral("/mightyos/session.pid")));
}

void TestSession::coreDumpsOffAndRestoredForChildren()
{
    rlimit limit {};
    QCOMPARE(::getrlimit(RLIMIT_CORE, &limit), 0);
    if (limit.rlim_max == 0)
        QSKIP("hard core limit is 0 here: nothing to restore");
    // A distinctive soft limit to see in the child.
    limit.rlim_cur = limit.rlim_max == RLIM_INFINITY ? rlim_t(4096) * 1024 : std::min<rlim_t>(limit.rlim_max, 4096 * 1024);
    QCOMPARE(::setrlimit(RLIMIT_CORE, &limit), 0);
    const rlim_t expected = limit.rlim_cur;

    QVERIFY(ProcessHardening::disableCoreDumps());
    QVERIFY(ProcessHardening::isNonDumpable());
    rlimit now {};
    QCOMPARE(::getrlimit(RLIMIT_CORE, &now), 0);
    QCOMPARE(now.rlim_cur, rlim_t(0));

    QTemporaryDir tmp;
    const QString out = tmp.path() + QStringLiteral("/limit");
    Launcher launcher;
    launcher.setScopeTools({});
    QSignalSpy exited(&launcher, &Launcher::exited);
    // The child's own limits, in bytes ("Max core file size  <soft> <hard> bytes").
    QVERIFY(launcher.launch(QStringLiteral("t"), { QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                                   QStringLiteral("grep 'Max core file size' /proc/self/limits > ") + out }));
    QVERIFY(exited.wait(5000));
    QFile file(out);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QList<QByteArray> fields = file.readAll().simplified().split(' ');
    QVERIFY(fields.size() >= 5);
    QCOMPARE(fields.at(4), QByteArray::number(qulonglong(expected)));
}

QTEST_MAIN(TestSession)
#include "tst_session.moc"
