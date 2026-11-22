#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <cstdio>
#include <cstdlib>

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

    return QGuiApplication::exec();
}
