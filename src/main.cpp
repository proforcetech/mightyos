#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QWindow>
#include <QtQml/qqmlextensionplugin.h>

#include "artworkprovider.h"
#include "cecservice.h"
#include "feedback.h"
#include "gamepadservice.h"
#include "inputrouter.h"
#include "launcher.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

// Both QML modules are static libraries (shared with the tests).
Q_IMPORT_QML_PLUGIN(MightyOS_CorePlugin)
Q_IMPORT_QML_PLUGIN(MightyOS_UIPlugin)

int main(int argc, char *argv[])
{
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
#ifdef MIGHTYOS_DEV_TOOLS
    const QCommandLineOption fakeCecOption(QStringLiteral("fake-cec"),
        QStringLiteral("Development: a fake TV remote reading key names (up, select, back, ...) "
                       "from 'stdin' or 'fifo' ($XDG_RUNTIME_DIR/mightyos-fake-cec)."),
        QStringLiteral("stdin|fifo"));
#endif
    parser.addOption(windowedOption);
    parser.addOption(fullscreenOption);
    parser.addOption(noGamepadOption);
    parser.addOption(noCecOption);
#ifdef MIGHTYOS_DEV_TOOLS
    parser.addOption(fakeCecOption);
#endif
    parser.process(app);

    std::unique_ptr<CecBackend> cecBackend;
#ifdef MIGHTYOS_DEV_TOOLS
    if (parser.isSet(fakeCecOption)
        && qEnvironmentVariable("XDG_SESSION_DESKTOP").compare(QLatin1String("mightyos"), Qt::CaseInsensitive) == 0) {
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
    if (!parser.isSet(noCecOption)) {
        cecBackend = CecService::detectKernelBackend();   // nullptr without /dev/cec*
    }

    if (parser.isSet(windowedOption) && parser.isSet(fullscreenOption)) {
        std::fputs("mightyos: --windowed and --fullscreen are mutually exclusive\n", stderr);
        return EXIT_FAILURE;
    }

    // TODO(M6): default to fullscreen once MightyOS runs as its own cage
    // session; keep --windowed for development.
    const bool fullscreen = parser.isSet(fullscreenOption);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    // image://art/... (app icons, decoded off the GUI thread, disk-cached).
    ArtworkProvider::install(&engine);
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
    // Bumps and error toasts rumble (InputRouter applies the policy).
    Feedback::wire(&engine);
    auto *launcher = engine.singletonInstance<Launcher *>("MightyOS.Core", "Launcher");

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
    const auto syncRunning = [&] {
        const bool running = launcher && launcher->isRunning();
        gamepads.setAppRunning(running);
        if (cec)
            cec->setAppRunning(running);
    };
    if (launcher)
        QObject::connect(launcher, &Launcher::runningChanged, &gamepads, syncRunning);
    QObject::connect(router, &InputRouter::rumbleRequested, &gamepads,
                     [&](InputRouter::Feedback kind) { gamepads.requestRumble(kind); });
    syncRunning();
    if (!parser.isSet(noGamepadOption))
        gamepads.start();
    if (cec && !cec->start())
        cec.reset();   // no usable adapter: stay silent

    return QGuiApplication::exec();
}
