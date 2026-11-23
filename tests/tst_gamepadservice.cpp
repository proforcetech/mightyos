// GamepadService (SDL3 glue): thread lifecycle without hangs (works with no
// controller attached, e.g. on WSL), joining on aboutToQuit, rumble
// suppression, style selection from SDL's gamepad types, Steam virtual pad
// detection, the user mapping file. Pressing buttons needs a human: see the
// hands-on checklist in README.md (mightyos-inputtest).
#include "gamepadservice.h"

#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <SDL3/SDL.h>

#include <sys/stat.h>

using Style = InputRouter::PadStyle;

class TestGamepadService : public QObject
{
    Q_OBJECT

private slots:
    void styleForSdlType_data();
    void styleForSdlType();
    void steamVirtualPad();
    void startStopJoinsPromptly();
    void stopIsIdempotentAndRestartable();
    void destructorJoins();
    void aboutToQuitJoins();
    void rumbleSuppressedWhileRunning();
    void userMappingFile();
    void unusableMappingFilesAreIgnored();
};

namespace {

bool startAndWaitReady(GamepadService &service)
{
    QSignalSpy ready(&service, &GamepadService::ready);
    if (!service.start())
        return false;
    if (!ready.wait(10000))
        return false;
    return ready.first().at(0).toBool();
}

} // namespace

void TestGamepadService::styleForSdlType_data()
{
    QTest::addColumn<int>("type");
    QTest::addColumn<Style>("style");
    QTest::newRow("unknown") << int(SDL_GAMEPAD_TYPE_UNKNOWN) << Style::Xbox;
    QTest::newRow("standard") << int(SDL_GAMEPAD_TYPE_STANDARD) << Style::Xbox;
    QTest::newRow("xbox360") << int(SDL_GAMEPAD_TYPE_XBOX360) << Style::Xbox;
    QTest::newRow("xboxone") << int(SDL_GAMEPAD_TYPE_XBOXONE) << Style::Xbox;
    QTest::newRow("ps3") << int(SDL_GAMEPAD_TYPE_PS3) << Style::PlayStation;
    QTest::newRow("ps4") << int(SDL_GAMEPAD_TYPE_PS4) << Style::PlayStation;
    QTest::newRow("ps5") << int(SDL_GAMEPAD_TYPE_PS5) << Style::PlayStation;
    QTest::newRow("switch-pro") << int(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO) << Style::Nintendo;
    QTest::newRow("joycon-pair") << int(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) << Style::Nintendo;
    QTest::newRow("joycon-left") << int(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT) << Style::Nintendo;
    QTest::newRow("joycon-right") << int(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT) << Style::Nintendo;
}

void TestGamepadService::styleForSdlType()
{
    QFETCH(int, type);
    QFETCH(Style, style);
    QCOMPARE(GamepadService::styleForSdlType(type), style);
}

void TestGamepadService::steamVirtualPad()
{
    QVERIFY(GamepadService::isSteamVirtual(0x28de, 0x11ff));
    QVERIFY(!GamepadService::isSteamVirtual(0x28de, 0x1102));   // a real Steam Controller
    QVERIFY(!GamepadService::isSteamVirtual(0x045e, 0x0b12));   // Xbox Series X
}

void TestGamepadService::startStopJoinsPromptly()
{
    InputRouter router;
    GamepadService service(&router);
    service.setUserMappingFile({});
    QVERIFY(startAndWaitReady(service));
    QVERIFY(service.isStarted());
    QVERIFY(!service.start());   // already running

    // The thread is blocked in its wait (up to 2 s): stop() must wake it.
    QTest::qWait(50);
    QElapsedTimer timer;
    timer.start();
    service.stop();
    QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(!service.isStarted());
}

void TestGamepadService::stopIsIdempotentAndRestartable()
{
    InputRouter router;
    GamepadService service(&router);
    service.setUserMappingFile({});
    service.stop();               // never started
    QVERIFY(startAndWaitReady(service));
    service.stop();
    service.stop();
    QVERIFY(startAndWaitReady(service));   // SDL can be initialised again
    service.stop();
}

void TestGamepadService::destructorJoins()
{
    InputRouter router;
    QElapsedTimer timer;
    {
        GamepadService service(&router);
        service.setUserMappingFile({});
        QVERIFY(startAndWaitReady(service));
        timer.start();
    }
    QVERIFY(timer.elapsed() < 500);
}

void TestGamepadService::aboutToQuitJoins()
{
    InputRouter router;
    GamepadService service(&router);
    service.setUserMappingFile({});
    service.stopOnAboutToQuit();
    QVERIFY(startAndWaitReady(service));
    // What QCoreApplication::exec() emits on quit, before main() returns
    // and destroys the engine (and the router).
    QVERIFY(QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit", Qt::DirectConnection));
    QVERIFY(!service.isStarted());
}

void TestGamepadService::rumbleSuppressedWhileRunning()
{
    InputRouter router;
    GamepadService service(&router);
    service.setUserMappingFile({});
    QVERIFY(!service.requestRumble(InputRouter::Feedback::Bump));   // not started

    QVERIFY(startAndWaitReady(service));
    QVERIFY(service.requestRumble(InputRouter::Feedback::Bump));    // queued (no pad: no-op)
    service.setAppRunning(true);
    QVERIFY(service.isAppRunning());
    QVERIFY(!service.requestRumble(InputRouter::Feedback::Bump));
    QVERIFY(!service.requestRumble(InputRouter::Feedback::Error));
    service.setAppRunning(false);
    QVERIFY(service.requestRumble(InputRouter::Feedback::Error));
    service.stop();
}

void TestGamepadService::userMappingFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("gamecontrollerdb.txt"));
    const char *guid = "03000000aa1100002233000001000000";
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("# test mapping\n");
        file.write(guid);
        file.write(",MightyOS Test Pad,a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1,platform:Linux,\n");
    }
    InputRouter router;
    GamepadService service(&router);
    service.setUserMappingFile(path);
    QVERIFY(startAndWaitReady(service));
    // The mapping database is global in SDL (and SDL is up while the
    // service runs).
    char *mapping = SDL_GetGamepadMappingForGUID(SDL_StringToGUID(guid));
    QVERIFY(mapping);
    QVERIFY(QByteArray(mapping).contains("MightyOS Test Pad"));
    SDL_free(mapping);
    service.stop();
}

void TestGamepadService::unusableMappingFilesAreIgnored()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    InputRouter router;

    // Oversized: not loaded.
    const QString big = dir.filePath(QStringLiteral("big.txt"));
    const char *guid = "03000000bb1100002233000001000000";
    {
        QFile file(big);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(guid);
        file.write(",Too Big Pad,a:b0,platform:Linux,\n");
        file.write(QByteArray(int(GamepadService::kMaxMappingFileBytes), '#'));
    }
    const QRegularExpression unusable(QStringLiteral("ignoring unusable mapping file"));
    QTest::ignoreMessage(QtWarningMsg, unusable);
    {
        GamepadService service(&router);
        service.setUserMappingFile(big);
        QVERIFY(startAndWaitReady(service));
        QVERIFY(!SDL_GetGamepadMappingForGUID(SDL_StringToGUID(guid)));
    }

    // A FIFO must not hang the service thread.
    const QString fifo = dir.filePath(QStringLiteral("fifo"));
    QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    QTest::ignoreMessage(QtWarningMsg, unusable);
    {
        GamepadService service(&router);
        service.setUserMappingFile(fifo);
        QVERIFY(startAndWaitReady(service));
    }

    // Missing: fine.
    GamepadService service(&router);
    service.setUserMappingFile(dir.filePath(QStringLiteral("missing.txt")));
    QVERIFY(startAndWaitReady(service));
}

QTEST_GUILESS_MAIN(TestGamepadService)
#include "tst_gamepadservice.moc"
