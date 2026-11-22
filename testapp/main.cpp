// mightyos-testapp: a harmless GUI program used as the launch target in M0
// when no xterm/foot/... is installed. Not part of the shipped product.
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <cstdlib>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("mightyos-testapp"));

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption quitAfter(QStringLiteral("quit-after"),
        QStringLiteral("Quit automatically after <ms> milliseconds."), QStringLiteral("ms"));
    const QCommandLineOption exitCode(QStringLiteral("exit-code"),
        QStringLiteral("Exit with <code> instead of 0."), QStringLiteral("code"));
    parser.addOption(quitAfter);
    parser.addOption(exitCode);
    parser.process(app);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.setInitialProperties({ { QStringLiteral("quitAfter"), parser.value(quitAfter).toInt() } });
    engine.loadFromModule("MightyOS.TestApp", "TestApp");

    const int rc = QGuiApplication::exec();
    return (rc == 0 && parser.isSet(exitCode)) ? parser.value(exitCode).toInt() : rc;
}
