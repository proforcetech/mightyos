#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QWindow>
#include <QtQml/qqmlextensionplugin.h>

#include "artworkprovider.h"
#include "emulatorlaunch.h"
#include "cecservice.h"
#include "feedback.h"
#include "gamepadservice.h"
#include "guideoverlayhost.h"
#include "inputrouter.h"
#include "launcher.h"
#include "processhardening.h"
#include "session.h"
#include "sessionstate.h"
#include "signalwatcher.h"
#include "systemregistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QStandardPaths>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/stat.h>

Q_LOGGING_CATEGORY(lcMain, "mightyos.session")

namespace {

// SIGRTMIN is a libc function, not a constant.
int quitSignal()
{
    return SIGRTMIN + 1;
}

// `mightyos --signal=guide|close|quit`: what the labwc panic keybinds run
// (session/labwc/rc.xml.in). Signals the session instance through its
// locked pid file in $XDG_RUNTIME_DIR (SessionLock::signalSession), before
// any Qt setup: no display needed, nothing else happens.
int sendSignal(const char *what)
{
    int signo = 0;
    if (std::strcmp(what, "guide") == 0)
        signo = SIGUSR1;
    else if (std::strcmp(what, "close") == 0)
        signo = SIGUSR2;
    else if (std::strcmp(what, "quit") == 0)
        signo = quitSignal();
    if (signo == 0) {
        std::fputs("mightyos: --signal takes guide, close or quit\n", stderr);
        return 2;
    }
    const QString error = SessionLock::signalSession(qEnvironmentVariable("XDG_RUNTIME_DIR"), signo);
    if (!error.isEmpty()) {
        std::fprintf(stderr, "mightyos: %s\n", qPrintable(error));
        return 1;
    }
    return 0;
}

bool argvHas(int argc, char **argv, const char *option)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], option) == 0)
            return true;
    }
    return false;
}

QString readSmallFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.read(4096)).trimmed();
}

// The last NAutoVTs= in logind.conf and its drop-ins (systemd's order:
// /usr/lib, /etc, then the drop-in dirs sorted). Default 6.
QString logindValue(const char *key)
{
    QStringList files { QStringLiteral("/usr/lib/systemd/logind.conf"), QStringLiteral("/etc/systemd/logind.conf") };
    for (const char *dir : { "/usr/lib/systemd/logind.conf.d", "/etc/systemd/logind.conf.d" }) {
        QDir d(QString::fromLatin1(dir));
        for (const QString &name : d.entryList({ QStringLiteral("*.conf") }, QDir::Files, QDir::Name))
            files.append(d.filePath(name));
    }
    QString value;
    const QString prefix = QLatin1String(key) + QLatin1Char('=');
    for (const QString &path : std::as_const(files)) {
        for (const QString &line : readSmallFile(path).split(QLatin1Char('\n'))) {
            const QString t = line.trimmed();
            if (t.startsWith(prefix))
                value = t.mid(prefix.size()).trimmed();
        }
    }
    return value;
}

// Where autologin is configured (getty, GDM, SDDM, greetd), for the report.
QStringList autologinSources()
{
    QStringList found;
    QDir getty(QStringLiteral("/etc/systemd/system/getty@tty1.service.d"));
    for (const QString &name : getty.entryList({ QStringLiteral("*.conf") }, QDir::Files)) {
        if (readSmallFile(getty.filePath(name)).contains(QLatin1String("--autologin")))
            found.append(QStringLiteral("getty@tty1"));
    }
    if (readSmallFile(QStringLiteral("/etc/gdm3/custom.conf")).contains(QLatin1String("AutomaticLoginEnable=true")))
        found.append(QStringLiteral("GDM"));
    QDir sddm(QStringLiteral("/etc/sddm.conf.d"));
    QStringList sddmFiles { QStringLiteral("/etc/sddm.conf") };
    for (const QString &name : sddm.entryList({ QStringLiteral("*.conf") }, QDir::Files))
        sddmFiles.append(sddm.filePath(name));
    for (const QString &path : std::as_const(sddmFiles)) {
        const QString text = readSmallFile(path);
        if (text.contains(QLatin1String("[Autologin]")) && text.contains(QLatin1String("User=")))
            found.append(QStringLiteral("SDDM"));
    }
    if (readSmallFile(QStringLiteral("/etc/greetd/config.toml")).contains(QLatin1String("initial_session")))
        found.append(QStringLiteral("greetd"));
    found.removeDuplicates();
    return found;
}

// Root-owned and not group/other writable (the dir and its files).
bool rootOwnedTree(const QString &dir)
{
    const auto ok = [](const QString &path) {
        struct stat st {};
        return ::lstat(QFile::encodeName(path).constData(), &st) == 0 && st.st_uid == 0 && !(st.st_mode & 022);
    };
    if (!ok(dir))
        return false;
    for (const QString &name : QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot)) {
        if (!ok(dir + QLatin1Char('/') + name))
            return false;
    }
    return true;
}

// `mightyos --check-session`: what an installer (or the admin) should know.
// Warnings only; never fails.
int checkSession()
{
    const QStringList groups = SessionState::currentGroupNames();
    const QStringList risky = SessionState::riskyGroups(groups);
    const QStringList info = SessionState::informationalGroups(groups);
    std::printf("MightyOS session check\n");
    std::printf("  groups:       %s\n", risky.isEmpty() ? "ok (not in sudo/video/input/lxd/docker/...)"
                                    : qPrintable(QStringLiteral("WARNING: in %1 (a kiosk user should not be; PLAN D8/D16)")
                                                     .arg(risky.join(QStringLiteral(", ")))));
    if (!info.isEmpty())
        std::printf("                (also in %s: can read all system logs)\n", qPrintable(info.join(QStringLiteral(", "))));
    const QStringList autologin = autologinSources();
    std::printf("  autologin:    %s\n", autologin.isEmpty() ? "none found"
                    : qPrintable(QStringLiteral("%1 (anyone at the keyboard is this user: lock its password, see README)")
                                     .arg(autologin.join(QStringLiteral(", ")))));
    const QString vts = logindValue("NAutoVTs");
    std::printf("  NAutoVTs:     %s%s\n", vts.isEmpty() ? "6 (default)" : qPrintable(vts),
                vts == QLatin1String("0") ? "" : " (spare text consoles; see share/mightyos/system/logind.conf.d)");
    const QString labwcDir = QStringLiteral(MIGHTYOS_INSTALLED_LABWC_DIR);
    std::printf("  labwc config: %s %s\n", qPrintable(labwcDir),
                !QFileInfo(labwcDir).isDir() ? "(not installed)"
                : rootOwnedTree(labwcDir) ? "(root-owned)" : "(WARNING: not root-owned; the user could change the panic keys)");
    const ScopeTools scopes = ScopeTools::detect();
    std::printf("  app scopes:   %s\n", scopes.isUsable() ? "systemd user manager found (systemd-run --user --scope)"
                                                       : "no systemd user manager: process groups only");
    std::printf("  overlay:      %s\n", !GuideOverlayHost::compiledIn()
                    ? "built without LayerShellQt: Guide overlay only where MightyOS is visible"
                    : GuideOverlayHost::compositorHasLayerShell() ? "wlr-layer-shell available"
                    : "LayerShellQt built in; no wlr-layer-shell compositor reachable now");
    const QString sysrq = readSmallFile(QStringLiteral("/proc/sys/kernel/sysrq"));
    std::printf("  kernel.sysrq: %s%s\n", sysrq.isEmpty() ? "?" : qPrintable(sysrq),
                sysrq == QLatin1String("0") ? "" : " (see share/mightyos/system/sysctl.d)");
    const bool udev = QFileInfo::exists(QStringLiteral("/etc/udev/rules.d/70-mightyos-cec.rules"))
                      || QFileInfo::exists(QStringLiteral("/usr/lib/udev/rules.d/70-mightyos-cec.rules"));
    std::printf("  CEC udev rule: %s\n", udev ? "installed" : "not installed (only needed with a CEC adapter)");
    // ROM folders Flatpak emulators won't get (D30).
    SystemRegistry systems;
    systems.load(SystemRegistry::defaultDirs(), QDir::homePath());
    const LaunchEnv env = LaunchEnv::fromEnvironment();
    QStringList rejected;
    for (const SystemDef &system : systems.systems()) {
        for (const QString &dir : system.romDirs) {
            QString problem;
            if (EmulatorLaunch::allowedRomDir(dir, env, &problem).isEmpty() && problem != QLatin1String("missing"))
                rejected.append(QStringLiteral("%1: %2 (%3)").arg(system.id, dir, problem));
        }
    }
    std::printf("  ROM folders:  %s\n", rejected.isEmpty() ? "ok (all usable by Flatpak emulators)"
                                                          : "WARNING: not passed to Flatpak emulators:");
    for (const QString &line : std::as_const(rejected))
        std::printf("                %s\n", qPrintable(line));
    return EXIT_SUCCESS;
}

} // namespace

// Both QML modules are static libraries (shared with the tests).
Q_IMPORT_QML_PLUGIN(MightyOS_CorePlugin)
Q_IMPORT_QML_PLUGIN(MightyOS_UIPlugin)

int main(int argc, char *argv[])
{
    // The labwc panic keys: signal the session and leave.
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--signal=", 9) == 0)
            return sendSignal(argv[i] + 9);
    }
    // The check runs fine over SSH, without a display.
    if (argvHas(argc, argv, "--check-session") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
        && qEnvironmentVariableIsEmpty("DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("mightyos"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("MightyOS"));
    QGuiApplication::setApplicationVersion(QStringLiteral(MIGHTYOS_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("mightyos"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("MightyOS - console-style launcher"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption windowedOption(QStringLiteral("windowed"),
        QStringLiteral("Run in a 1280x720 window (default)."));
    const QCommandLineOption fullscreenOption(QStringLiteral("fullscreen"),
        QStringLiteral("Run fullscreen."));
    const QCommandLineOption noGamepadOption(QStringLiteral("no-gamepad"),
        QStringLiteral("Don't read gamepads (SDL3)."));
    const QCommandLineOption noCecOption(QStringLiteral("no-cec"),
        QStringLiteral("Don't use an HDMI-CEC adapter (/dev/cec*) for the TV remote."));
    // Not "session": QGuiApplication itself takes -session/--session <id>
    // (X11 session management) and would eat the argument after it.
    const QCommandLineOption sessionOption(QStringLiteral("session-mode"),
        QStringLiteral("Run as the MightyOS login session (mightyos-session): fullscreen, crash counter "
                       "and safe mode, leftover app scopes stopped."));
    const QCommandLineOption safeModeOption(QStringLiteral("safe-mode"),
        QStringLiteral("Start in safe mode (no artwork downloads, default theme)."));
    const QCommandLineOption checkSessionOption(QStringLiteral("check-session"),
        QStringLiteral("Print warnings about the session setup (groups, autologin, VTs, scopes, overlay, "
                       "SysRq, ROM folders) and exit."));
    const QCommandLineOption previousStatusOption(QStringLiteral("previous-status"),
        QStringLiteral("Session restart loop: how the previous run ended (exit status; 128+N: signal N)."),
        QStringLiteral("status"));
    const QCommandLineOption signalOption(QStringLiteral("signal"),
        QStringLiteral("Send guide, close or quit to the running session (the labwc panic keys)."),
        QStringLiteral("guide|close|quit"));
#ifdef MIGHTYOS_DEV_TOOLS
    const QCommandLineOption fakeCecOption(QStringLiteral("fake-cec"),
        QStringLiteral("Development: a fake TV remote reading key names (up, select, back, ...) "
                       "from 'stdin' or 'fifo' ($XDG_RUNTIME_DIR/mightyos-fake-cec)."),
        QStringLiteral("stdin|fifo"));
    const QCommandLineOption mockLibraryOption(QStringLiteral("mock-library"),
        QStringLiteral("Development: show the bundled mock library instead of scanning ROMs and Steam; "
                       "games launch the bundled test app."));
    const QCommandLineOption allowCoreDumpsOption(QStringLiteral("allow-core-dumps"),
        QStringLiteral("Development: keep core dumps and ptrace access (debugging)."));
#endif
    parser.addOption(windowedOption);
    parser.addOption(fullscreenOption);
    parser.addOption(noGamepadOption);
    parser.addOption(noCecOption);
    parser.addOption(sessionOption);
    parser.addOption(safeModeOption);
    parser.addOption(checkSessionOption);
    parser.addOption(previousStatusOption);
    parser.addOption(signalOption);   // handled before Qt starts (sendSignal)
#ifdef MIGHTYOS_DEV_TOOLS
    parser.addOption(fakeCecOption);
    parser.addOption(mockLibraryOption);
    parser.addOption(allowCoreDumpsOption);
#endif
    parser.process(app);

    if (parser.isSet(checkSessionOption))
        return checkSession();

    // PLAN section 4: no core dumps of the GUI (secrets in memory), no
    // ptrace by other processes of the same user.
#ifdef MIGHTYOS_DEV_TOOLS
    if (!parser.isSet(allowCoreDumpsOption))
#endif
        ProcessHardening::disableCoreDumps();

    // Clean quit on SIGTERM (logind ending the session), SIGINT, SIGHUP:
    // the event loop ends, aboutToQuit flushes navstate, the engine's
    // Launcher stops the running app's scope. SIGUSR1/2 and SIGRTMIN+1: the
    // labwc panic keybinds (Guide menu / close the app / leave the session,
    // which MightyOS confirms). Installed before the pid file exists.
    SignalWatcher signalWatcher({ SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2, quitSignal() });

    const bool sessionMode = parser.isSet(sessionOption);
    Session::Options sessionOptions;
    sessionOptions.sessionMode = sessionMode;
    sessionOptions.safeMode = parser.isSet(safeModeOption);
    sessionOptions.riskyGroups = SessionState::riskyGroups(SessionState::currentGroupNames());
    if (!sessionOptions.riskyGroups.isEmpty())
        qCWarning(lcMain).noquote() << "the session user is in" << sessionOptions.riskyGroups.join(QStringLiteral(", "))
                                    << "- a MightyOS kiosk user should not be (PLAN D8, D16)";
    std::unique_ptr<SessionState> sessionState;
    SessionLock sessionLock;
    QStringList previousUnits;
    if (sessionMode) {
        bool statusOk = false;
        const int previousStatus = parser.isSet(previousStatusOption)
            ? parser.value(previousStatusOption).toInt(&statusOk) : -1;
        sessionState = std::make_unique<SessionState>(
            QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation));
        const SessionState::Start start = sessionState->recordStart(statusOk || previousStatus < 0 ? previousStatus : -1);
        sessionOptions.recentCrashes = start.recentCrashes;
        if (start.previousCrashed)
            qCWarning(lcMain) << "the previous MightyOS run crashed (status" << previousStatus << ");"
                              << start.recentCrashes << "crash(es) in the last" << SessionState::kWindowSecs / 60 << "minutes";
        if (start.safeMode) {
            sessionOptions.safeMode = true;
            qCWarning(lcMain) << "starting in safe mode after repeated crashes";
        }
        sessionOptions.groupWarning = !sessionOptions.riskyGroups.isEmpty()
                                      && sessionState->warnedGroups() != sessionOptions.riskyGroups;
        QString lockError;
        if (sessionLock.acquire(qEnvironmentVariable("XDG_RUNTIME_DIR"), QCoreApplication::applicationPid(), &lockError))
            previousUnits = sessionState->launchUnits();
        else
            qCWarning(lcMain).noquote() << "not the session instance:" << lockError
                                        << "- panic keys and scope cleanup stay with the other one";
    }

    // The Library singleton reads this when QML first uses it.
#ifdef MIGHTYOS_DEV_TOOLS
    if (parser.isSet(mockLibraryOption))
        qputenv("MIGHTYOS_MOCK_LIBRARY", "1");
#else
    qunsetenv("MIGHTYOS_MOCK_LIBRARY");   // packaged builds always show the real library
#endif

    std::unique_ptr<CecBackend> cecBackend;
#ifdef MIGHTYOS_DEV_TOOLS
    if (parser.isSet(fakeCecOption)
        && (sessionMode
            || qEnvironmentVariable("XDG_SESSION_DESKTOP").compare(QLatin1String("mightyos"), Qt::CaseInsensitive) == 0)) {
        // The kiosk session never takes input from a FIFO or stdin.
        std::fputs("mightyos: --fake-cec is not available in the MightyOS session\n", stderr);
        return EXIT_FAILURE;
    }
    if (parser.isSet(fakeCecOption)) {
        const QString source = parser.value(fakeCecOption);
        if (source == QLatin1String("stdin")) {
            cecBackend = FakeCecBackend::fromStdin();
        } else if (source == QLatin1String("fifo")) {
            QString error;
            const QString path = FakeCecBackend::defaultFifoPath();
            cecBackend = FakeCecBackend::fromFifo(path, &error);
            if (!cecBackend) {
                std::fprintf(stderr, "mightyos: --fake-cec fifo: %s\n", qPrintable(error));
                return EXIT_FAILURE;
            }
            std::fprintf(stderr, "mightyos: fake TV remote: echo up > %s\n", qPrintable(path));
        } else {
            std::fputs("mightyos: --fake-cec takes 'stdin' or 'fifo'\n", stderr);
            return EXIT_FAILURE;
        }
    } else
#endif
    if (!parser.isSet(noCecOption) && !sessionOptions.safeMode) {
        cecBackend = CecService::detectKernelBackend();   // nullptr without /dev/cec*
    } else if (sessionOptions.safeMode) {
        qCInfo(lcMain) << "safe mode: no HDMI-CEC";
    }

    if (parser.isSet(windowedOption) && parser.isSet(fullscreenOption)) {
        std::fputs("mightyos: --windowed and --fullscreen are mutually exclusive\n", stderr);
        return EXIT_FAILURE;
    }

    // The session is fullscreen; development runs stay windowed.
    const bool fullscreen = parser.isSet(fullscreenOption) || (sessionMode && !parser.isSet(windowedOption));

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    // image://art/... (app icons, decoded off the GUI thread, disk-cached).
    ArtworkProvider *artwork = ArtworkProvider::install(&engine);
    // Safe mode: no image decoding at all (a bad cached file may be what
    // crashed us); tiles show their vector icons.
    artwork->setDisabled(sessionOptions.safeMode);
    auto *session = engine.singletonInstance<Session *>("MightyOS.Core", "Session");
    if (!session) {
        std::fputs("mightyos: Session singleton missing\n", stderr);
        return EXIT_FAILURE;
    }
    session->configure(sessionOptions);
    session->setArtworkProvider(artwork);
    if (sessionState) {
        SessionState *state = sessionState.get();
        QObject::connect(session, &Session::groupWarningAcknowledged, session,
                         [state](const QStringList &groups) { state->setWarnedGroups(groups); });
    }
    QObject::connect(&signalWatcher, &SignalWatcher::received, session, [session](int signo) {
        switch (signo) {
        case SIGUSR1:
            qCInfo(lcMain) << "panic key: Guide";
            session->requestGuide();
            break;
        case SIGUSR2:
            qCInfo(lcMain) << "panic key: close the running app";
            session->requestClose();
            break;
        default:
            if (signo == quitSignal()) {
                qCInfo(lcMain) << "panic key: leave the session";
                session->requestQuit();
                break;
            }
            qCInfo(lcMain) << "signal" << signo << "- quitting";
            QCoreApplication::quit();
            break;
        }
    });
    engine.setInitialProperties({ { QStringLiteral("startFullscreen"), fullscreen } });
    engine.loadFromModule("MightyOS.UI", "Main");
    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;

    // All keyboard and mouse-button input enters through InputRouter, which
    // filters the window's events before Qt Quick delivers them.
    auto *window = qobject_cast<QWindow *>(engine.rootObjects().constFirst());
    auto *router = engine.singletonInstance<InputRouter *>("MightyOS.Core", "InputRouter");
    if (!window || !router) {
        std::fputs("mightyos: root window or InputRouter missing\n", stderr);
        return EXIT_FAILURE;
    }
    router->attachToWindow(window);
    session->setMainWindow(qobject_cast<QQuickWindow *>(window));
    // Bumps and error toasts rumble (InputRouter applies the policy).
    Feedback::wire(&engine);
    auto *launcher = engine.singletonInstance<Launcher *>("MightyOS.Core", "Launcher");
    // Destroyed before the engine (declared after it).
    auto overlay = std::make_unique<GuideOverlayHost>(&engine, window, router);
    session->setOverlayHost(overlay.get());
    // Only the session instance (the lock holder) cleans up after a crashed
    // run and records the running launch's scopes for the next one.
    if (sessionMode && sessionLock.isHeld() && launcher) {
        launcher->stopStaleScopes(previousUnits);
        SessionState *state = sessionState.get();
        state->setLaunchUnits({});
        QObject::connect(launcher, &Launcher::activeUnitsChanged, launcher,
                         [state, launcher] { state->setLaunchUnits(launcher->activeUnits()); });
    }
    // Startup is over: from here an exit code is not a crash (D29).
    if (sessionState)
        sessionState->markUp();

    // Input threads. Declared after the engine, so they are destroyed (and
    // joined) before it; aboutToQuit joins them even earlier, while the
    // event loop's objects are all still alive (see GamepadService).
    GamepadService gamepads(router);
    std::unique_ptr<CecService> cec;
    if (cecBackend)
        cec = std::make_unique<CecService>(router, std::move(cecBackend));
    gamepads.stopOnAboutToQuit();
    if (cec)
        cec->stopOnAboutToQuit();
    // While the Guide menu is open over a running app, pads and the remote
    // navigate it like any screen.
    const auto syncRunning = [&] {
        const bool running = launcher && launcher->isRunning() && !session->isGuideOpen();
        gamepads.setAppRunning(running);
        if (cec)
            cec->setAppRunning(running);
    };
    if (launcher)
        QObject::connect(launcher, &Launcher::runningChanged, &gamepads, syncRunning);
    QObject::connect(session, &Session::guideOpenChanged, &gamepads, syncRunning);
    QObject::connect(router, &InputRouter::rumbleRequested, &gamepads,
                     [&](InputRouter::Feedback kind) { gamepads.requestRumble(kind); });
    syncRunning();
    if (!parser.isSet(noGamepadOption))
        gamepads.start();
    if (cec && !cec->start())
        cec.reset();   // no usable adapter: stay silent

    const int code = QGuiApplication::exec();
    overlay.reset();
    session->setOverlayHost(nullptr);
    return code;
}
