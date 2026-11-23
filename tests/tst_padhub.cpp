// PadHub, the SDL-free core of GamepadService, with pipes standing in for
// the pads' device nodes: wake-up fd indexing, hang-up handling and
// reopening, the SDL-wait fallback, latching on app start/exit, the
// service-thread rumble check, style-before-action, keyboard refusal.
#include "padhub.h"

#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <linux/input.h>

#include <fcntl.h>
#include <map>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace PadInput;
using Action = InputRouter::Action;

class TestPadHub : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void wakeFdIndexing();
    void controlFd();
    void hangUpClosesAndReopens();
    void noWakeFdMeansSdlWait();
    void latchOnStartAndExit();
    void rumbleTargetOnServiceThread();
    void styleIsPostedBeforeTheAction();
    void keyboardLikeNodesAreRefused();
    void openWakeFdRejectsOtherPaths();

private:
    // path -> write end of the pipe whose read end the opener hands out
    std::map<QString, int> m_writers;
    int m_eventFd = -1;
    PadHub::Opener opener();
    void write(const QString &path);
};

PadHub::Opener TestPadHub::opener()
{
    return [this](const QString &path, QString *why) {
        if (path.startsWith(QLatin1String("none"))) {
            if (why)
                *why = QStringLiteral("test: no fd");
            return -1;
        }
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
            return -1;
        if (m_writers.count(path))
            ::close(m_writers[path]);
        m_writers[path] = fds[1];
        return fds[0];   // the hub owns (and closes) the read end
    };
}

void TestPadHub::write(const QString &path)
{
    QVERIFY(m_writers.count(path));
    QCOMPARE(::write(m_writers[path], "x", 1), ssize_t(1));
}

void TestPadHub::init()
{
    m_eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    QVERIFY(m_eventFd >= 0);
}

void TestPadHub::cleanup()
{
    for (auto &[path, fd] : m_writers) {
        if (fd >= 0)
            ::close(fd);
    }
    m_writers.clear();
    ::close(m_eventFd);
}

void TestPadHub::wakeFdIndexing()
{
    InputRouter router;
    PadHub hub(&router);
    hub.setOpener(opener());
    hub.setControlFds(m_eventFd, -1);
    hub.add(11, QStringLiteral("one"), QStringLiteral("p1"), InputRouter::PadStyle::Xbox, true);
    hub.add(22, QStringLiteral("two"), QStringLiteral("p2"), InputRouter::PadStyle::Xbox, true);
    hub.add(33, QStringLiteral("three"), QStringLiteral("p3"), InputRouter::PadStyle::Xbox, true);
    QVERIFY(hub.canBlock());

    write(QStringLiteral("p2"));
    auto result = hub.wait(1000);
    QCOMPARE(result.input, std::vector<quint32> { 22 });
    QVERIFY(!result.control);

    write(QStringLiteral("p1"));
    write(QStringLiteral("p3"));
    result = hub.wait(1000);
    QCOMPARE(result.input, (std::vector<quint32> { 11, 33 }));

    // Drained: nothing is pending any more.
    result = hub.wait(0);
    QVERIFY(result.input.empty());

    // Removing a pad keeps the others' indices right.
    QVERIFY(hub.remove(22));
    write(QStringLiteral("p3"));
    result = hub.wait(1000);
    QCOMPARE(result.input, std::vector<quint32> { 33 });
}

void TestPadHub::controlFd()
{
    InputRouter router;
    PadHub hub(&router);
    hub.setControlFds(m_eventFd, -1);
    const uint64_t one = 1;
    QCOMPARE(::write(m_eventFd, &one, sizeof one), ssize_t(sizeof one));
    QVERIFY(hub.wait(1000).control);
    QVERIFY(!hub.wait(0).control);   // drained
}

void TestPadHub::hangUpClosesAndReopens()
{
    InputRouter router;
    PadHub hub(&router);
    hub.setOpener(opener());
    hub.setControlFds(m_eventFd, -1);
    hub.add(1, QStringLiteral("pad"), QStringLiteral("p1"), InputRouter::PadStyle::Xbox, false);
    hub.add(2, QStringLiteral("other"), QStringLiteral("p2"), InputRouter::PadStyle::Xbox, false);

    // The node goes away (writer closed = POLLHUP).
    ::close(m_writers[QStringLiteral("p1")]);
    m_writers[QStringLiteral("p1")] = -1;
    const auto result = hub.wait(1000);
    QCOMPARE(result.hungUp, std::vector<quint32> { 1 });
    QVERIFY(result.input.empty());
    QCOMPARE(hub.find(1)->fd, -1);
    QVERIFY(hub.find(1)->lostFd);
    QVERIFY(!hub.canBlock());            // SDL's wait until it is back
    QVERIFY(hub.find(2)->fd >= 0);

    // Hotplug settle: the node is back under the same path.
    QCOMPARE(hub.reopenLost(), 1);
    QVERIFY(hub.canBlock());
    write(QStringLiteral("p1"));
    QCOMPARE(hub.wait(1000).input, std::vector<quint32> { 1 });
}

void TestPadHub::noWakeFdMeansSdlWait()
{
    InputRouter router;
    PadHub hub(&router);
    hub.setOpener(opener());
    hub.setControlFds(m_eventFd, -1);
    QVERIFY(hub.canBlock());             // no pads
    QTest::ignoreMessage(QtInfoMsg, QRegularExpression(QStringLiteral("no wake-up fd")));
    hub.add(1, QStringLiteral("odd pad"), QStringLiteral("none"), InputRouter::PadStyle::Xbox, false);
    QVERIFY(!hub.canBlock());
    hub.remove(1);
    QVERIFY(hub.canBlock());
    // Without an eventfd nothing could wake poll(): SDL's wait.
    hub.setControlFds(-1, -1);
    QVERIFY(!hub.canBlock());
    // A hidraw node counts as a wake-up fd too (and is remembered).
    hub.setControlFds(m_eventFd, -1);
    hub.add(2, QStringLiteral("DualSense"), QStringLiteral("/dev/hidraw7"), InputRouter::PadStyle::PlayStation, true);
    QVERIFY(hub.canBlock());
    QVERIFY(hub.hasHidraw());
}

void TestPadHub::latchOnStartAndExit()
{
    InputRouter router;
    QSignalSpy received(&router, &InputRouter::received);
    PadHub hub(&router);
    hub.setOpener(opener());
    hub.add(1, QStringLiteral("pad"), QStringLiteral("p1"), InputRouter::PadStyle::Xbox, false);

    hub.button(1, Button::DpadRight, true, 0);
    QCoreApplication::processEvents();
    QCOMPARE(received.count(), 1);

    // An app starts while right is held: latched, nothing more.
    hub.setAppRunning(true);
    QVERIFY(hub.applyPendingLatch());
    QVERIFY(!hub.applyPendingLatch());   // once per change
    for (qint64 t = 0; t < 2000; t += 10)
        hub.tick(t);
    // The app exits with right still held: still nothing (latched again).
    hub.setAppRunning(false);
    QVERIFY(hub.applyPendingLatch());
    for (qint64 t = 2000; t < 4000; t += 10)
        hub.tick(t);
    QCoreApplication::processEvents();
    QCOMPARE(received.count(), 1);
    // Released and pressed again: normal.
    hub.button(1, Button::DpadRight, false, 4000);
    hub.button(1, Button::DpadRight, true, 4010);
    QCoreApplication::processEvents();
    QCOMPARE(received.count(), 2);

    // A pad connected while an app runs starts latched too.
    hub.setAppRunning(true);
    hub.add(2, QStringLiteral("late"), QStringLiteral("p2"), InputRouter::PadStyle::Xbox, false);
    hub.button(2, Button::South, true, 5000);
    QCoreApplication::processEvents();
    QCOMPARE(received.count(), 2);
}

void TestPadHub::rumbleTargetOnServiceThread()
{
    InputRouter router;
    PadHub hub(&router);
    hub.setOpener(opener());
    QVERIFY(!hub.rumbleTarget());
    hub.add(1, QStringLiteral("rumbles"), QStringLiteral("p1"), InputRouter::PadStyle::Xbox, true);
    hub.add(2, QStringLiteral("no motors"), QStringLiteral("p2"), InputRouter::PadStyle::Xbox, false);
    QCOMPARE(hub.rumbleTarget(), std::optional<quint32>(1));    // first pad until one is used
    hub.button(2, Button::South, true, 0);
    QCOMPARE(hub.lastActive(), quint32(2));
    QVERIFY(!hub.rumbleTarget());                                // the pad in use can't rumble
    hub.button(1, Button::South, true, 10);
    QCOMPARE(hub.rumbleTarget(), std::optional<quint32>(1));
    // A request queued just before a launch is dropped on the service thread.
    hub.setAppRunning(true);
    QVERIFY(!hub.rumbleTarget());
    hub.setAppRunning(false);
    QCOMPARE(hub.rumbleTarget(), std::optional<quint32>(1));
    // The last active pad goes away: the next one takes over.
    hub.remove(1);
    QCOMPARE(hub.lastActive(), quint32(2));
}

void TestPadHub::styleIsPostedBeforeTheAction()
{
    InputRouter router;
    QList<InputRouter::PadStyle> styleAtAction;
    connect(&router, &InputRouter::triggered, this, [&] { styleAtAction.append(router.gamepadStyle()); });
    PadHub hub(&router);
    hub.setOpener(opener());
    hub.add(1, QStringLiteral("Pro Controller"), QStringLiteral("p1"), InputRouter::PadStyle::Nintendo, false);
    hub.add(2, QStringLiteral("DualSense"), QStringLiteral("p2"), InputRouter::PadStyle::PlayStation, false);
    hub.button(1, Button::South, true, 0);
    hub.button(2, Button::South, true, 0);
    hub.button(1, Button::East, true, 0);
    QCoreApplication::processEvents();
    QCOMPARE(styleAtAction, (QList<InputRouter::PadStyle> { InputRouter::PadStyle::Nintendo,
                                                           InputRouter::PadStyle::PlayStation,
                                                           InputRouter::PadStyle::Nintendo }));
}

void TestPadHub::keyboardLikeNodesAreRefused()
{
    unsigned char bits[KEY_MAX / 8 + 1] = {};
    const auto set = [&](int key) { bits[key / 8] |= (1u << (key % 8)); };
    set(BTN_SOUTH);
    set(BTN_EAST);
    set(BTN_MODE);
    set(KEY_RECORD);   // the Xbox Series Share button
    QVERIFY(!PadHub::looksLikeKeyboard(bits, sizeof bits));
    set(KEY_1);
    set(KEY_ESC);
    QVERIFY(!PadHub::looksLikeKeyboard(bits, sizeof bits));
    for (int key : { KEY_A, KEY_Q, KEY_M, KEY_Z, KEY_P, KEY_L }) {
        unsigned char one[KEY_MAX / 8 + 1] = {};
        one[key / 8] |= (1u << (key % 8));
        QVERIFY2(PadHub::looksLikeKeyboard(one, sizeof one), qPrintable(QString::number(key)));
    }
    QVERIFY(!PadHub::looksLikeKeyboard(bits, 1));   // truncated bitmap: no letters visible
}

void TestPadHub::openWakeFdRejectsOtherPaths()
{
    QString why;
    QCOMPARE(PadHub::openWakeFd(QString(), &why), -1);
    QCOMPARE(why, QStringLiteral("no device path"));
    QCOMPARE(PadHub::openWakeFd(QStringLiteral("/etc/passwd"), &why), -1);
    QCOMPARE(why, QStringLiteral("not an evdev or hidraw node"));
    QCOMPARE(PadHub::openWakeFd(QStringLiteral("/dev/input/event-does-not-exist"), &why), -1);
    QVERIFY(!why.isEmpty());
}

QTEST_GUILESS_MAIN(TestPadHub)
#include "tst_padhub.moc"
