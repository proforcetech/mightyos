// HDMI-CEC remote: UI command mapping, message parsing (broadcast key frames
// dropped), follower replies (active source, deck status, feature abort,
// never for broadcasts), press/hold/repeat handling, rate limiting, and the
// fake backend end to end into InputRouter with a fake clock (no CEC
// hardware, no sleeps).
#include "cecservice.h"

#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using Action = InputRouter::Action;
using Kind = PadInput::Event::Kind;
using Bytes = std::vector<quint8>;

class TestCec : public QObject
{
    Q_OBJECT

private slots:
    void mapping_data();
    void mapping();
    void names();
    void parseKeyMessages();
    void followerDirectedReplies();
    void followerActiveSource();
    void followerNeverAnswersBroadcasts();
    void remoteRepeatAndHold();
    void remoteLostRelease();
    void remoteLateRefreshResumes();
    void remoteRootMenu();
    void remoteLatch();
    void fakeBackendEndToEnd();
    void fakeBackendWhileRunning();
    void pressesAreRateLimited();
    void lostAndStopNotices();
    void fifoChecks();
    void kernelBackendWithoutDevice();
    void libcecIsAStub();
};

void TestCec::mapping_data()
{
    QTest::addColumn<int>("code");
    QTest::addColumn<Action>("action");
    QTest::addColumn<QString>("arg");
    QTest::newRow("select") << int(Cec::Select) << Action::Accept << QString();
    QTest::newRow("enter") << int(Cec::Enter) << Action::Accept << QString();
    QTest::newRow("up") << int(Cec::Up) << Action::Navigate << QStringLiteral("up");
    QTest::newRow("down") << int(Cec::Down) << Action::Navigate << QStringLiteral("down");
    QTest::newRow("left") << int(Cec::Left) << Action::Navigate << QStringLiteral("left");
    QTest::newRow("right") << int(Cec::Right) << Action::Navigate << QStringLiteral("right");
    QTest::newRow("exit") << int(Cec::Exit) << Action::Back << QString();
    QTest::newRow("root-menu") << int(Cec::RootMenu) << Action::Guide << QString();
    QTest::newRow("setup-menu") << int(Cec::SetupMenu) << Action::QuickPanel << QString();
    QTest::newRow("contents-menu") << int(Cec::ContentsMenu) << Action::Options << QString();
    QTest::newRow("media-context") << int(Cec::MediaContextMenu) << Action::Options << QString();
    QTest::newRow("red") << int(Cec::F2Red) << Action::Options << QString();
    QTest::newRow("green") << int(Cec::F3Green) << Action::QuickPanel << QString();
    QTest::newRow("yellow") << int(Cec::F4Yellow) << Action::Search << QString();
    QTest::newRow("blue") << int(Cec::F1Blue) << Action::Favorite << QString();
    QTest::newRow("ch-up") << int(Cec::ChannelUp) << Action::PageUp << QString();
    QTest::newRow("ch-down") << int(Cec::ChannelDown) << Action::PageDown << QString();
    QTest::newRow("rewind") << int(Cec::Rewind) << Action::PrevCategory << QString();
    QTest::newRow("backward") << int(Cec::Backward) << Action::PrevCategory << QString();
    QTest::newRow("fast-forward") << int(Cec::FastForward) << Action::NextCategory << QString();
    QTest::newRow("forward") << int(Cec::Forward) << Action::NextCategory << QString();
    QTest::newRow("number-5") << int(Cec::Number0 + 5) << Action::None << QString();
    QTest::newRow("right-up diagonal") << 0x05 << Action::None << QString();
    QTest::newRow("power") << 0x40 << Action::None << QString();
}

void TestCec::mapping()
{
    QFETCH(int, code);
    QFETCH(Action, action);
    QFETCH(QString, arg);
    QCOMPARE(Cec::bindingFor(quint8(code)), (PadInput::Binding { action, arg }));
}

void TestCec::names()
{
    QCOMPARE(Cec::codeForName(QStringLiteral("select")), std::optional<quint8>(Cec::Select));
    QCOMPARE(Cec::codeForName(QStringLiteral(" OK ")), std::optional<quint8>(Cec::Select));
    QCOMPARE(Cec::codeForName(QStringLiteral("back")), std::optional<quint8>(Cec::Exit));
    QCOMPARE(Cec::codeForName(QStringLiteral("home")), std::optional<quint8>(Cec::RootMenu));
    QCOMPARE(Cec::codeForName(QStringLiteral("Yellow")), std::optional<quint8>(Cec::F4Yellow));
    QCOMPARE(Cec::codeForName(QStringLiteral("7")), std::optional<quint8>(Cec::Number0 + 7));
    QVERIFY(!Cec::codeForName(QStringLiteral("rm -rf")));
    QVERIFY(!Cec::codeForName(QString()));
    for (const char *name : { "select", "up", "down", "left", "right", "exit", "root-menu", "red",
                              "channel-up", "rewind", "fast-forward", "3" }) {
        const auto code = Cec::codeForName(QString::fromLatin1(name));
        QVERIFY(code);
        QCOMPARE(Cec::codeForName(Cec::nameForCode(*code)), code);
    }
    QCOMPARE(Cec::nameForCode(0x40), QStringLiteral("0x40"));
}

void TestCec::parseKeyMessages()
{
    const quint8 pressed[] = { 0x04, Cec::OpUserControlPressed, Cec::Up };
    QCOMPARE(Cec::parseKeyMessage(pressed, 3), (std::optional<Cec::KeyEvent>(Cec::KeyEvent { true, Cec::Up })));
    const quint8 released[] = { 0x04, Cec::OpUserControlReleased };
    QCOMPARE(Cec::parseKeyMessage(released, 2), (std::optional<Cec::KeyEvent>(Cec::KeyEvent { false, 0 })));
    QVERIFY(!Cec::parseKeyMessage(pressed, 2));             // operand missing
    QVERIFY(!Cec::parseKeyMessage(pressed, 1));             // poll message
    QVERIFY(!Cec::parseKeyMessage(nullptr, 3));
    const quint8 other[] = { 0x04, Cec::OpGiveOsdName };
    QVERIFY(!Cec::parseKeyMessage(other, 2));
    // D18: broadcast-addressed key frames come from "any device on the bus".
    const quint8 broadcastPress[] = { 0x0F, Cec::OpUserControlPressed, Cec::Select };
    QVERIFY(!Cec::parseKeyMessage(broadcastPress, 3));
    const quint8 broadcastRelease[] = { 0x4F, Cec::OpUserControlReleased };
    QVERIFY(!Cec::parseKeyMessage(broadcastRelease, 2));
}

namespace {

Cec::FollowerState us()
{
    Cec::FollowerState state;
    state.address = 4;                 // Playback 1
    state.physicalAddress = 0x1000;    // HDMI 1 of the TV
    return state;
}

Bytes reply(Cec::FollowerState &state, std::initializer_list<quint8> msg)
{
    const Bytes in(msg);
    return Cec::followerReply(in.data(), int(in.size()), state, QByteArrayLiteral("MightyOS"));
}

} // namespace

void TestCec::followerDirectedReplies()
{
    auto state = us();
    // TV (0) asks us (4) for our name.
    QCOMPARE(reply(state, { 0x04, Cec::OpGiveOsdName }),
             (Bytes { 0x40, Cec::OpSetOsdName, 'M', 'i', 'g', 'h', 't', 'y', 'O', 'S' }));
    const Bytes osd { 0x04, Cec::OpGiveOsdName };
    QCOMPARE(Cec::followerReply(osd.data(), 2, state, QByteArrayLiteral("A very long name\x01 indeed")).size(),
             size_t(2 + 14));
    QCOMPARE(reply(state, { 0x04, Cec::OpGiveDevicePowerStatus }), (Bytes { 0x40, Cec::OpReportPowerStatus, 0x00 }));
    QCOMPARE(reply(state, { 0x04, Cec::OpMenuRequest, 0x02 }), (Bytes { 0x40, Cec::OpMenuStatus, 0x00 }));
    // Give Deck Status (mandatory for Playback devices): Stop; "off" needs no reply.
    QCOMPARE(reply(state, { 0x04, Cec::OpGiveDeckStatus, 0x01 }), (Bytes { 0x40, Cec::OpDeckStatus, 0x1A }));
    QVERIFY(reply(state, { 0x04, Cec::OpGiveDeckStatus, 0x02 }).empty());
    // Unknown directed opcode: Feature Abort, "unrecognized opcode".
    QCOMPARE(reply(state, { 0x04, 0x41, 0x24 }), (Bytes { 0x40, Cec::OpFeatureAbort, 0x41, 0x00 }));
    QCOMPARE(reply(state, { 0x54, 0x89, 0x01 }), (Bytes { 0x45, Cec::OpFeatureAbort, 0x89, 0x00 }));
    // ... but never to replies, Feature Abort itself, Standby, Abort, keys.
    for (const quint8 op : { quint8(Cec::OpFeatureAbort), quint8(Cec::OpReportPowerStatus), quint8(Cec::OpSetOsdName),
                             quint8(Cec::OpStandby), quint8(Cec::OpAbort), quint8(Cec::OpUserControlReleased),
                             quint8(0x9E), quint8(0x84) })
        QVERIFY2(reply(state, { 0x04, op, 0x00, 0x00 }).empty(), qPrintable(QString::number(op, 16)));

    // Not for us, from ourselves, no address yet: silent.
    QVERIFY(reply(state, { 0x08, Cec::OpGiveOsdName }).empty());
    QVERIFY(reply(state, { 0x44, Cec::OpGiveOsdName }).empty());
    Cec::FollowerState unclaimed;
    QVERIFY(reply(unclaimed, { 0x04, Cec::OpGiveOsdName }).empty());
}

void TestCec::followerActiveSource()
{
    auto state = us();
    QVERIFY(!state.activeSource);   // never on our own (D17)
    // Nobody routed to us yet: Request Active Source gets no answer.
    QVERIFY(reply(state, { 0x0F, Cec::OpRequestActiveSource }).empty());
    // The TV routes to another input.
    QVERIFY(reply(state, { 0x0F, Cec::OpSetStreamPath, 0x20, 0x00 }).empty());
    QVERIFY(!state.activeSource);
    // The TV routes to us: we are the active source and say so.
    const Bytes active { 0x4F, Cec::OpActiveSource, 0x10, 0x00 };
    QCOMPARE(reply(state, { 0x0F, Cec::OpSetStreamPath, 0x10, 0x00 }), active);
    QVERIFY(state.activeSource);
    QCOMPARE(reply(state, { 0x0F, Cec::OpRequestActiveSource }), active);
    // Another device takes over: not active any more.
    QVERIFY(reply(state, { 0x8F, Cec::OpActiveSource, 0x20, 0x00 }).empty());
    QVERIFY(!state.activeSource);
    QVERIFY(reply(state, { 0x0F, Cec::OpRequestActiveSource }).empty());
    // Routing Change away from us.
    reply(state, { 0x0F, Cec::OpSetStreamPath, 0x10, 0x00 });
    QVERIFY(state.activeSource);
    QVERIFY(reply(state, { 0x0F, Cec::OpRoutingChange, 0x10, 0x00, 0x30, 0x00 }).empty());
    QVERIFY(!state.activeSource);
    // Without a physical address we never claim it.
    Cec::FollowerState noPhysical;
    noPhysical.address = 4;
    QVERIFY(reply(noPhysical, { 0x0F, Cec::OpSetStreamPath, 0xFF, 0xFF }).empty());
    QVERIFY(!noPhysical.activeSource);
}

void TestCec::followerNeverAnswersBroadcasts()
{
    auto state = us();
    // Queries and unknown opcodes sent to everyone: no reply, no Feature Abort.
    for (const quint8 op : { quint8(Cec::OpGiveOsdName), quint8(Cec::OpGiveDevicePowerStatus),
                             quint8(Cec::OpMenuRequest), quint8(Cec::OpGiveDeckStatus), quint8(0x41),
                             quint8(Cec::OpStandby), quint8(0x32) })
        QVERIFY2(reply(state, { 0x0F, op, 0x01, 0x00 }).empty(), qPrintable(QString::number(op, 16)));
}

void TestCec::remoteRepeatAndHold()
{
    Cec::RemoteState remote;
    PadInput::Events out;
    remote.press(Cec::Right, 0, out);
    QCOMPARE(out.size(), size_t(1));
    QCOMPARE(out[0], (PadInput::Event { Kind::Action, Action::Navigate, QStringLiteral("right"), false }));
    out.clear();
    // The TV repeats its press every 300 ms: refreshes only. Our repeat:
    // 350 ms, then a steady 100 ms (no acceleration).
    QList<qint64> times;
    for (qint64 t = 1; t <= 1000; ++t) {
        if (t % 300 == 0)
            remote.press(Cec::Right, t, out);
        remote.tick(t, out);
        for (const auto &e : out) {
            QCOMPARE(e.kind, Kind::Action);
            QVERIFY(e.repeat);
            times.append(t);
        }
        out.clear();
    }
    QCOMPARE(times, (QList<qint64> { 350, 450, 550, 650, 750, 850, 950 }));
    remote.release(1000);
    remote.tick(2000, out);
    QVERIFY(out.empty());
    QVERIFY(!remote.nextDeadline());

    // Non-navigation keys never repeat, and the TV's own repeats don't
    // trigger them twice.
    remote.press(Cec::Select, 3000, out);
    remote.press(Cec::Select, 3300, out);
    remote.tick(3600, out);
    QCOMPARE(out.size(), size_t(1));
    QCOMPARE(out[0].action, Action::Accept);
}

void TestCec::remoteLostRelease()
{
    Cec::RemoteState remote;
    PadInput::Events out;
    remote.press(Cec::Down, 0, out);
    out.clear();
    // No refresh, no release: our repeat stops 1.25 x 400 ms after the
    // press (not at 550 ms), at most two repeats.
    QList<qint64> times;
    for (qint64 t = 1; t <= 2000; ++t) {
        remote.tick(t, out);
        for (const auto &e : out) {
            QVERIFY(e.repeat);
            times.append(t);
        }
        out.clear();
        if (const auto deadline = remote.nextDeadline())
            QVERIFY2(*deadline >= t, "deadline in the past: busy loop");
    }
    QCOMPARE(times, (QList<qint64> { 350, 450 }));
    QVERIFY(!remote.nextDeadline());   // released at 550 ms
    // The same key again is a new press.
    remote.press(Cec::Down, 2100, out);
    QCOMPARE(out.size(), size_t(1));
    QVERIFY(!out[0].repeat);
}

void TestCec::remoteLateRefreshResumes()
{
    Cec::RemoteState remote;
    PadInput::Events out;
    remote.press(Cec::Left, 0, out);
    remote.tick(350, out);
    remote.tick(450, out);
    remote.tick(520, out);   // past 500: paused
    out.clear();
    remote.press(Cec::Left, 530, out);   // a late refresh (still within 550)
    QVERIFY(out.empty());                // not a new step
    remote.tick(630, out);
    QCOMPARE(out.size(), size_t(1));     // scrolling resumes
    QVERIFY(out[0].repeat);
}

void TestCec::remoteRootMenu()
{
    Cec::RemoteState remote;
    PadInput::Events out;
    remote.press(Cec::RootMenu, 0, out);
    QCOMPARE(out.size(), size_t(2));
    QCOMPARE(out[0].kind, Kind::GuidePressed);
    QCOMPARE(out[1].kind, Kind::GuideHeld);
}

void TestCec::remoteLatch()
{
    Cec::RemoteState remote;
    PadInput::Events out;
    remote.press(Cec::Left, 0, out);
    out.clear();
    remote.latchHeld();
    // The TV keeps repeating the held key: ignored until released.
    remote.press(Cec::Left, 300, out);
    remote.tick(400, out);
    remote.press(Cec::Left, 600, out);
    remote.tick(700, out);
    QVERIFY(out.empty());
    remote.release(800);
    remote.press(Cec::Left, 900, out);
    QCOMPARE(out.size(), size_t(1));
}

namespace {

struct Pipe {
    int read = -1, write = -1;
    Pipe() { int fds[2]; if (::pipe2(fds, O_CLOEXEC) == 0) { read = fds[0]; write = fds[1]; } }
    ~Pipe() { if (read >= 0) ::close(read); if (write >= 0) ::close(write); }
    bool send(const char *text) const { return ::write(write, text, strlen(text)) == ssize_t(strlen(text)); }
};

// Key events straight from the test thread (no real-time refresh of its own).
class InjectingBackend : public CecBackend
{
public:
    QString name() const override { return QStringLiteral("Injected"); }
    bool start(Sink sink, Lost) override { m_sink = std::move(sink); return true; }
    void stop() override {}
    void send(const Cec::KeyEvent &event) { m_sink(event); }
private:
    Sink m_sink;
};

// A service on a fake clock: every round runs at `clock` ms.
struct FakeClockService {
    std::shared_ptr<std::atomic<qint64>> clock = std::make_shared<std::atomic<qint64>>(0);
    CecService service;
    FakeClockService(InputRouter *router, std::unique_ptr<CecBackend> backend)
        : service(router, std::move(backend))
    {
        auto c = clock;
        service.setClockForTesting([c] { return c->load(); });
    }
    FakeClockService(InputRouter *router, int fd)
        : FakeClockService(router, std::make_unique<FakeCecBackend>(fd, false, QStringLiteral("Fake")))
    {
    }
    bool waitRound(int before)
    {
        return QTest::qWaitFor([&] { return service.rounds() > before; }, 5000);
    }
    // Moves the clock and waits for the worker to finish a round.
    bool advanceTo(qint64 t)
    {
        clock->store(t);
        const int before = service.rounds();
        service.pokeForTesting();
        return waitRound(before);
    }
};

} // namespace

void TestCec::fakeBackendEndToEnd()
{
    // Key names through the fake backend (taps).
    Pipe pipe;
    QVERIFY(pipe.read >= 0);
    InputRouter router;
    router.setRemoteMode(InputRouter::RemoteMode::RemoteFull);
    QSignalSpy received(&router, &InputRouter::received);
    QSignalSpy devices(&router, &InputRouter::deviceChanged);
    {
        FakeClockService fake(&router, pipe.read);
        QVERIFY(fake.service.start());
        QVERIFY(fake.service.isActive());
        QTRY_COMPARE(devices.count(), 1);
        QCOMPARE(router.controllers(), QStringList { QStringLiteral("Fake") });

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("unknown key not-a-key")));
        QVERIFY(pipe.send("up\nselect\n# comment\nnot-a-key\nback\n"));
        QTRY_COMPARE(received.count(), 3);
        QCOMPARE(received.at(0).at(0).value<Action>(), Action::Navigate);
        QCOMPARE(received.at(0).at(1).toString(), QStringLiteral("up"));
        QCOMPARE(received.at(1).at(0).value<Action>(), Action::Accept);
        QCOMPARE(received.at(2).at(0).value<Action>(), Action::Back);
        QCOMPARE(received.at(0).at(3).value<InputRouter::Source>(), InputRouter::Source::Remote);
        QCOMPARE(router.lastSource(), InputRouter::Source::Remote);
        fake.service.stop();
        QVERIFY(!fake.service.isActive());
    }

    // A held key, deterministically: the "TV" refreshes once, then the
    // release is lost.
    received.clear();
    auto backend = std::make_unique<InjectingBackend>();
    InjectingBackend *tv = backend.get();
    FakeClockService fake(&router, std::move(backend));
    QVERIFY(fake.service.start());
    const auto inject = [&](const Cec::KeyEvent &event) {
        const int before = fake.service.rounds();
        tv->send(event);
        QVERIFY(fake.waitRound(before));
    };
    const auto expect = [&](qint64 t, int total) {
        QVERIFY(fake.advanceTo(t));
        QCoreApplication::processEvents();
        QCOMPARE(received.count(), total);
    };
    QVERIFY(fake.advanceTo(1000));
    inject({ true, Cec::Right });
    QCoreApplication::processEvents();
    QCOMPARE(received.count(), 1);                        // the press
    QVERIFY(!received.at(0).at(2).toBool());
    expect(1349, 1);
    expect(1350, 2);                                       // first repeat
    inject({ true, Cec::Right });                          // TV refresh at 1350: interval 350
    expect(1450, 3);
    expect(1550, 4);
    expect(1650, 5);
    expect(1750, 6);
    // No refresh after 1350 + 1.25 x 350: paused, then released at 1900.
    expect(1850, 6);
    expect(2500, 6);
    QVERIFY(received.last().at(2).toBool());               // repeats are flagged
    // The same key after the timeout is a new press.
    inject({ true, Cec::Right });
    QCoreApplication::processEvents();
    QCOMPARE(received.count(), 7);
    QVERIFY(!received.last().at(2).toBool());
}

void TestCec::fakeBackendWhileRunning()
{
    Pipe pipe;
    InputRouter router;
    router.setBlocked(true);
    QSignalSpy received(&router, &InputRouter::received);
    QSignalSpy held(&router, &InputRouter::guideHeld);
    FakeClockService fake(&router, pipe.read);
    fake.service.setAppRunning(true);
    QVERIFY(fake.service.start());
    QVERIFY(pipe.send("select\nup\nhome\n"));
    QTRY_COMPARE(held.count(), 1);
    QCOMPARE(held.first().at(0).value<InputRouter::Source>(), InputRouter::Source::Remote);
    QVERIFY(fake.advanceTo(10));
    QCoreApplication::processEvents();
    QCOMPARE(received.count(), 0);   // nothing else was even posted
}

void TestCec::pressesAreRateLimited()
{
    Pipe pipe;
    InputRouter router;
    QSignalSpy received(&router, &InputRouter::received);
    FakeClockService fake(&router, pipe.read);
    QVERIFY(fake.service.start());
    // 30 taps at the same instant: the burst of 10 gets through.
    QByteArray burst;
    for (int i = 0; i < 30; ++i)
        burst += i % 2 ? "left\n" : "right\n";
    QVERIFY(pipe.send(burst.constData()));
    QTRY_COMPARE(fake.service.droppedPresses(), 20);
    QTRY_COMPARE(received.count(), 10);
    // A second later the bucket has refilled.
    QVERIFY(fake.advanceTo(1000));
    QVERIFY(pipe.send("down\n"));
    QTRY_COMPARE(received.count(), 11);
}

namespace {

// A backend whose device can be "unplugged".
class LosingBackend : public CecBackend
{
public:
    QString name() const override { return QStringLiteral("Losing <b>adapter</b>") + QChar(0x202E); }
    bool start(Sink, Lost lost) override { m_lost = std::move(lost); return true; }
    void stop() override {}
    void unplug() { m_lost(); }
private:
    Lost m_lost;
};

} // namespace

void TestCec::lostAndStopNotices()
{
    InputRouter router;
    QSignalSpy devices(&router, &InputRouter::deviceChanged);
    auto backend = std::make_unique<LosingBackend>();
    LosingBackend *raw = backend.get();
    {
        CecService service(&router, std::move(backend));
        QVERIFY(service.start());
        QTRY_COMPARE(devices.count(), 1);
        // Sanitised at intake: no bidi override; markup stays text (QML shows
        // it as plain text).
        QCOMPARE(router.controllers(), QStringList { QStringLiteral("Losing <b>adapter</b>") });
        raw->unplug();
        QTRY_COMPARE(devices.count(), 2);
        QVERIFY(!devices.at(1).at(0).toBool());
        QVERIFY(!devices.at(1).at(3).toBool());   // not initial: toast
        QVERIFY(router.controllers().isEmpty());
    }   // stop(): already announced as gone, no second notice
    QCoreApplication::processEvents();
    QCOMPARE(devices.count(), 2);

    // A normal stop posts the removal (quietly, as "initial").
    Pipe pipe;
    {
        CecService service(&router, std::make_unique<FakeCecBackend>(pipe.read, false, QStringLiteral("Fake")));
        QVERIFY(service.start());
        QTRY_COMPARE(devices.count(), 3);
        service.stop();
        QTRY_COMPARE(devices.count(), 4);
        QVERIFY(!devices.at(3).at(0).toBool());
        QVERIFY(devices.at(3).at(3).toBool());
    }
}

void TestCec::fifoChecks()
{
    QTemporaryDir dir;   // 0700, ours
    QVERIFY(dir.isValid());
    QString error;

    // Created as a 0600 FIFO, and usable.
    const QString path = dir.filePath(QStringLiteral("fake-cec"));
    auto backend = FakeCecBackend::fromFifo(path, &error);
    QVERIFY2(backend, qPrintable(error));
    struct stat st {};
    QCOMPARE(::lstat(QFile::encodeName(path).constData(), &st), 0);
    QVERIFY(S_ISFIFO(st.st_mode));
    QCOMPARE(int(st.st_mode & 0777), 0600);
    {
        InputRouter router;
        QSignalSpy received(&router, &InputRouter::received);
        CecService service(&router, std::move(backend));
        QVERIFY(service.start());
        for (const char *key : { "down\n", "left\n" }) {
            QFile fifo(path);
            QVERIFY(fifo.open(QIODevice::WriteOnly));
            fifo.write(key);
            fifo.close();   // the writer goes away: no EOF spin, still running
        }
        QTRY_COMPARE(received.count(), 2);
    }

    // Refused: a regular file, a FIFO others can write to, a directory
    // others can enter, no path.
    const QString regular = dir.filePath(QStringLiteral("regular"));
    QFile file(regular);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QVERIFY(!FakeCecBackend::fromFifo(regular, &error));
    const QString open = dir.filePath(QStringLiteral("open-fifo"));
    QCOMPARE(::mkfifo(QFile::encodeName(open).constData(), 0600), 0);
    QCOMPARE(::chmod(QFile::encodeName(open).constData(), 0622), 0);
    QVERIFY(!FakeCecBackend::fromFifo(open, &error));
    QVERIFY(!FakeCecBackend::fromFifo(QString(), &error));
    const QString shared = dir.filePath(QStringLiteral("shared"));
    QVERIFY(QDir().mkdir(shared));
    QCOMPARE(::chmod(QFile::encodeName(shared).constData(), 0755), 0);
    QVERIFY(!FakeCecBackend::fromFifo(shared + QStringLiteral("/fake-cec"), &error));
    QVERIFY(error.contains(QStringLiteral("0700")));
    QVERIFY(!QFile::exists(shared + QStringLiteral("/fake-cec")));   // nothing created there
}

void TestCec::kernelBackendWithoutDevice()
{
    // No CEC hardware on the dev or test machines: nothing is detected and
    // the service is never created (silent).
    if (KernelCecBackend::devices().isEmpty())
        QVERIFY(!CecService::detectKernelBackend());

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("cannot open /dev/cec-does-not-exist")));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("/dev/null is not a CEC adapter")));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("/dev/null is not a CEC adapter")));
    const auto noSink = [](const Cec::KeyEvent &) {};
    const auto noLost = [] {};
    KernelCecBackend missing(QStringLiteral("/dev/cec-does-not-exist"));
    QVERIFY(!missing.start(noSink, noLost));
    // Not a CEC device: the capability ioctl fails, start() refuses.
    KernelCecBackend notCec(QStringLiteral("/dev/null"));
    QVERIFY(!notCec.start(noSink, noLost));
    notCec.stop();

    InputRouter router;
    CecService service(&router, std::make_unique<KernelCecBackend>(QStringLiteral("/dev/null")));
    QVERIFY(!service.start());
    QVERIFY(!service.isActive());
    QVERIFY(router.controllers().isEmpty());
    CecService empty(&router, nullptr);
    QVERIFY(!empty.start());
}

void TestCec::libcecIsAStub()
{
    LibCecBackend libcec;
    QVERIFY(!libcec.start([](const Cec::KeyEvent &) {}, [] {}));
}

QTEST_GUILESS_MAIN(TestCec)
#include "tst_cec.moc"
