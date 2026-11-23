// mightyos-inputtest: hands-on check of the gamepad (and TV remote) input
// path without a GUI. Runs GamepadService (and optionally a CEC backend)
// into an InputRouter and prints one line per device change and per action
// that reaches the router:
//
//   connected id=3 name="Xbox Series X Controller" type=xboxone style=Xbox guid=... rumble=yes path=/dev/input/event4
//   action Accept source=Gamepad repeat=no
//   action Navigate right source=Gamepad repeat=yes
//   guideHeld source=Gamepad
//
// Works over SSH with no display (QCoreApplication, SDL_INIT_GAMEPAD only).
#include "cecservice.h"
#include "gamepadservice.h"
#include "inputrouter.h"
#include "textsanitize.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

// Device-provided strings are escaped (no control or bidi characters reach
// the terminal).
QString esc(const QString &text)
{
    return TextSanitize::escapeForTerminal(text);
}

void out(const QString &line)
{
    std::fputs(qPrintable(line + QLatin1Char('\n')), stdout);
    std::fflush(stdout);
}

QString styleName(InputRouter::PadStyle style)
{
    switch (style) {
    case InputRouter::PadStyle::PlayStation: return QStringLiteral("PlayStation");
    case InputRouter::PadStyle::Nintendo:    return QStringLiteral("Nintendo");
    case InputRouter::PadStyle::Xbox:        break;
    }
    return QStringLiteral("Xbox");
}

QString sourceName(InputRouter::Source source)
{
    switch (source) {
    case InputRouter::Source::Gamepad:  return QStringLiteral("Gamepad");
    case InputRouter::Source::Remote:   return QStringLiteral("Remote");
    case InputRouter::Source::Keyboard: return QStringLiteral("Keyboard");
    case InputRouter::Source::Mouse:    return QStringLiteral("Mouse");
    }
    return {};
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mightyos-inputtest"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Prints gamepad / TV remote devices and the actions they produce."));
    parser.addHelpOption();
    const QCommandLineOption secondsOption(QStringLiteral("seconds"),
        QStringLiteral("Exit after N seconds (default 10)."), QStringLiteral("N"), QStringLiteral("10"));
    const QCommandLineOption runningOption(QStringLiteral("running"),
        QStringLiteral("Behave as if an app were running: only Guide (held 1 s) gets through."));
    const QCommandLineOption rumbleOption(QStringLiteral("rumble"),
        QStringLiteral("Rumble the controller on every Accept (A / Cross / B on Nintendo)."));
    const QCommandLineOption cecOption(QStringLiteral("cec"),
        QStringLiteral("Also use the first /dev/cecN adapter, if any."));
    const QCommandLineOption tvRemoteOption(QStringLiteral("tv-remote"),
        QStringLiteral("TV remote trust level as in Settings: off, navigation (default) or full."),
        QStringLiteral("mode"), QStringLiteral("navigation"));
    parser.addOptions({ secondsOption, runningOption, rumbleOption, cecOption, tvRemoteOption });
#ifdef MIGHTYOS_DEV_TOOLS
    const QCommandLineOption fakeCecOption(QStringLiteral("fake-cec"),
        QStringLiteral("Also read fake TV remote keys from stdin (up, select, back, +up, -up, ...)."));
    parser.addOption(fakeCecOption);
#endif
    parser.process(app);

    bool ok = false;
    const int seconds = parser.value(secondsOption).toInt(&ok);
    if (!ok || seconds < 1 || seconds > 3600) {
        std::fputs("mightyos-inputtest: --seconds takes 1..3600\n", stderr);
        return EXIT_FAILURE;
    }
    const bool running = parser.isSet(runningOption);

    const QString tvRemote = parser.value(tvRemoteOption);
    if (tvRemote != QLatin1String("off") && tvRemote != QLatin1String("navigation")
        && tvRemote != QLatin1String("full")) {
        std::fputs("mightyos-inputtest: --tv-remote takes off, navigation or full\n", stderr);
        return EXIT_FAILURE;
    }

    InputRouter router;
    router.setBlocked(running);   // as Main.qml binds it to Launcher.running
    router.setRemoteMode(tvRemote == QLatin1String("off") ? InputRouter::RemoteMode::RemoteOff
                         : tvRemote == QLatin1String("full") ? InputRouter::RemoteMode::RemoteFull
                                                              : InputRouter::RemoteMode::RemoteNavigation);

    QObject::connect(&router, &InputRouter::received, &app,
                     [](InputRouter::Action action, const QString &arg, bool isRepeat,
                        InputRouter::Source source, bool delivered) {
        out(QStringLiteral("action %1%2 source=%3 repeat=%4%5")
                .arg(InputRouter::nameOf(action), arg.isEmpty() ? QString() : QLatin1Char(' ') + esc(arg),
                     sourceName(source), isRepeat ? QStringLiteral("yes") : QStringLiteral("no"),
                     delivered ? QString() : QStringLiteral(" (dropped)")));
    });
    QObject::connect(&router, &InputRouter::guideHeld, &app, [](InputRouter::Source source) {
        out(QStringLiteral("guideHeld source=%1").arg(sourceName(source)));
    });
    QObject::connect(&router, &InputRouter::gamepadStyleChanged, &app, [&router] {
        out(QStringLiteral("style %1").arg(styleName(router.gamepadStyle())));
    });

    GamepadService gamepads(&router);
    QObject::connect(&gamepads, &GamepadService::ready, &app, [](bool ok, const QString &detail) {
        out(ok ? QStringLiteral("gamepad service ready (%1)").arg(detail)
               : QStringLiteral("gamepad service FAILED: %1").arg(esc(detail)));
    });
    QObject::connect(&gamepads, &GamepadService::deviceAdded, &app,
                     [](const GamepadService::DeviceInfo &info) {
        out(QStringLiteral("connected id=%1 name=\"%2\" type=%3 style=%4 guid=%5 rumble=%6 path=%7 wake=%8%9")
                .arg(info.id).arg(esc(info.name), esc(info.type), styleName(info.style), esc(info.guid),
                                  info.rumble ? QStringLiteral("yes") : QStringLiteral("no"),
                                  info.path.isEmpty() ? QStringLiteral("-") : esc(info.path),
                                  info.wakeFd ? QStringLiteral("fd") : QStringLiteral("sdl-poll"),
                                  info.initial ? QStringLiteral(" (at start)") : QString()));
    });
    QObject::connect(&gamepads, &GamepadService::deviceRemoved, &app,
                     [](const GamepadService::DeviceInfo &info) {
        out(QStringLiteral("disconnected id=%1 name=\"%2\"").arg(info.id).arg(esc(info.name)));
    });
    if (parser.isSet(rumbleOption)) {
        QObject::connect(&router, &InputRouter::triggered, &app,
                         [&router](InputRouter::Action action, const QString &) {
            if (action == InputRouter::Action::Accept)
                out(router.rumble(InputRouter::Feedback::Bump) ? QStringLiteral("rumble requested")
                                                               : QStringLiteral("rumble suppressed"));
        });
        QObject::connect(&router, &InputRouter::rumbleRequested, &gamepads,
                         [&gamepads](InputRouter::Feedback kind) { gamepads.requestRumble(kind); });
    }

    std::unique_ptr<CecService> cec;
    std::unique_ptr<CecBackend> backend;
#ifdef MIGHTYOS_DEV_TOOLS
    if (parser.isSet(fakeCecOption))
        backend = FakeCecBackend::fromStdin();
    else
#endif
    if (parser.isSet(cecOption))
        backend = CecService::detectKernelBackend();
    if (backend) {
        const QString name = backend->name();
        cec = std::make_unique<CecService>(&router, std::move(backend));
        cec->setAppRunning(running);
        out(cec->start() ? QStringLiteral("remote ready (%1, tv-remote=%2)").arg(esc(name), tvRemote)
                         : QStringLiteral("remote FAILED (%1)").arg(esc(name)));
    } else if (parser.isSet(cecOption)) {
        out(QStringLiteral("remote: no /dev/cec* device"));
    }

    gamepads.stopOnAboutToQuit();
    if (cec)
        cec->stopOnAboutToQuit();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] {
        out(QStringLiteral("done after %1 s, %2 navigate repeats coalesced")
                .arg(seconds).arg(gamepads.coalescedRepeats()));
    });

    gamepads.setAppRunning(running);
    gamepads.start();
    out(QStringLiteral("listening for %1 s%2 - press buttons now")
            .arg(seconds).arg(running ? QStringLiteral(" (app-running mode: hold Guide)") : QString()));
    QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
    return QCoreApplication::exec();
}
