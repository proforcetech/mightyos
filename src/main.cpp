#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QWindow>
#include <QtQml/qqmlextensionplugin.h>

#include "inputrouter.h"

#include <cstdio>
#include <cstdlib>

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
    parser.addOption(windowedOption);
    parser.addOption(fullscreenOption);
    parser.process(app);

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

    return QGuiApplication::exec();
}
