// Pure gamepad logic (PadInput) and InputGate: mapping, stick and trigger
// hysteresis, repeat timing with a fake clock, Guide press/hold, coalescing
// of queued repeats, "only Guide while an app runs".
#include "padinput.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <limits>

using namespace PadInput;
using Kind = Event::Kind;

class TestPadInput : public QObject
{
    Q_OBJECT

private slots:
    void positionalMapping_data();
    void positionalMapping();
    void buttonsFireOnPressOnly();
    void repeatTimingAccelerates();
    void repeatAfterStallDoesNotBurst();
    void releaseStopsRepeat();
    void dpadAndStickDontDoubleStep();
    void otherHeldDirectionResumesAfterDelay();
    void stickDeadzoneHysteresis();
    void stickDominantAxis();
    void stickFlickToOpposite();
    void stickDiagonalJitterDoesNotFlip();
    void tokenBucket();
    void triggerHysteresis();
    void guidePressAndHold();
    void latchIgnoresWhatIsHeld();
    void nextDeadline();
    void gateRunningOnlyGuideHeld();
    void gateGuidePressMeansHome();
    void gateCoalescesQueuedRepeats();
    void stalledGuiDoesNotOvershoot();
};

namespace {

Events press(PadState &pad, Button b, qint64 t)
{
    Events out;
    pad.button(b, true, t, out);
    return out;
}

Events release(PadState &pad, Button b, qint64 t)
{
    Events out;
    pad.button(b, false, t, out);
    return out;
}

Events axis(PadState &pad, Axis a, float v, qint64 t)
{
    Events out;
    pad.axis(a, v, t, out);
    return out;
}

Events tick(PadState &pad, qint64 t)
{
    Events out;
    pad.tick(t, out);
    return out;
}

Event nav(const char *dir, bool repeat = false)
{
    return { Kind::Action, Action::Navigate, QString::fromLatin1(dir), repeat };
}

Event act(Action a)
{
    return { Kind::Action, a, {}, false };
}

} // namespace

void TestPadInput::positionalMapping_data()
{
    QTest::addColumn<Button>("button");
    QTest::addColumn<Action>("action");
    QTest::addColumn<QString>("arg");
    // PLAN section 3. Positional: the same for Xbox, PlayStation and
    // Nintendo pads (SDL reports positions); only the glyphs differ.
    QTest::newRow("south=accept") << Button::South << Action::Accept << QString();
    QTest::newRow("east=back") << Button::East << Action::Back << QString();
    QTest::newRow("west=favorite") << Button::West << Action::Favorite << QString();
    QTest::newRow("north=search") << Button::North << Action::Search << QString();
    QTest::newRow("start=options") << Button::Start << Action::Options << QString();
    QTest::newRow("back=quickpanel") << Button::Back << Action::QuickPanel << QString();
    QTest::newRow("lb=prev") << Button::LeftShoulder << Action::PrevCategory << QString();
    QTest::newRow("rb=next") << Button::RightShoulder << Action::NextCategory << QString();
    QTest::newRow("dpad-up") << Button::DpadUp << Action::Navigate << QStringLiteral("up");
    QTest::newRow("dpad-down") << Button::DpadDown << Action::Navigate << QStringLiteral("down");
    QTest::newRow("dpad-left") << Button::DpadLeft << Action::Navigate << QStringLiteral("left");
    QTest::newRow("dpad-right") << Button::DpadRight << Action::Navigate << QStringLiteral("right");
    QTest::newRow("guide-not-an-action") << Button::Guide << Action::None << QString();
    QTest::newRow("l3") << Button::LeftStick << Action::None << QString();
    QTest::newRow("r3") << Button::RightStick << Action::None << QString();
    QTest::newRow("misc/paddles") << Button::Other << Action::None << QString();
}

void TestPadInput::positionalMapping()
{
    QFETCH(Button, button);
    QFETCH(Action, action);
    QFETCH(QString, arg);
    QCOMPARE(bindingFor(button), (Binding { action, arg }));

    // Through PadState too: exactly one event on press for mapped buttons.
    PadState pad;
    const Events out = press(pad, button, 0);
    if (button == Button::Guide) {
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0].kind, Kind::GuidePressed);
    } else if (action == Action::None) {
        QVERIFY(out.empty());
    } else {
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0], (Event { Kind::Action, action, arg, false }));
    }
}

void TestPadInput::buttonsFireOnPressOnly()
{
    PadState pad;
    QCOMPARE(press(pad, Button::South, 0).size(), size_t(1));
    QVERIFY(release(pad, Button::South, 10).empty());
    // Held face buttons never repeat.
    QCOMPARE(press(pad, Button::East, 20).size(), size_t(1));
    for (qint64 t = 20; t < 3000; t += 10)
        QVERIFY(tick(pad, t).empty());
}

void TestPadInput::repeatTimingAccelerates()
{
    PadState pad;
    QCOMPARE(press(pad, Button::DpadRight, 1000), Events { nav("right") });

    // 350 ms initial delay.
    QVERIFY(tick(pad, 1000 + 349).empty());
    QCOMPARE(tick(pad, 1000 + 350), Events { nav("right", true) });

    // Then 90, 85, 80, ... 45, 40, 40, 40 ms (ticking every millisecond).
    QList<int> intervals;
    qint64 last = 1350;
    for (qint64 t = 1351; intervals.size() < 14; ++t) {
        const Events out = tick(pad, t);
        if (out.empty())
            continue;
        QCOMPARE(out, Events { nav("right", true) });
        intervals.append(int(t - last));
        last = t;
    }
    const QList<int> expected { 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 40, 40, 40 };
    QCOMPARE(intervals, expected);
}

void TestPadInput::repeatAfterStallDoesNotBurst()
{
    PadState pad;
    press(pad, Button::DpadDown, 0);
    // The input thread stalls for 5 s: one repeat, not a catch-up burst.
    QCOMPARE(tick(pad, 5000), Events { nav("down", true) });
    QVERIFY(tick(pad, 5000).empty());
    QVERIFY(tick(pad, 5089).empty());
    QCOMPARE(tick(pad, 5090).size(), size_t(1));
}

void TestPadInput::releaseStopsRepeat()
{
    PadState pad;
    press(pad, Button::DpadLeft, 0);
    QCOMPARE(tick(pad, 400).size(), size_t(1));
    release(pad, Button::DpadLeft, 410);
    for (qint64 t = 410; t < 2000; t += 5)
        QVERIFY(tick(pad, t).empty());
    QVERIFY(!pad.heldDirection());
}

void TestPadInput::dpadAndStickDontDoubleStep()
{
    PadState pad;
    QCOMPARE(press(pad, Button::DpadRight, 0), Events { nav("right") });
    // The stick also pushes right: no second step, repeat schedule unchanged.
    QVERIFY(axis(pad, Axis::LeftX, 0.9f, 100).empty());
    QVERIFY(tick(pad, 349).empty());
    QCOMPARE(tick(pad, 350).size(), size_t(1));
    // D-pad released, stick still right: keeps repeating, no new step.
    QVERIFY(release(pad, Button::DpadRight, 360).empty());
    QCOMPARE(pad.heldDirection(), std::optional<Dir>(Dir::Right));
}

void TestPadInput::otherHeldDirectionResumesAfterDelay()
{
    PadState pad;
    press(pad, Button::DpadRight, 0);
    QCOMPARE(press(pad, Button::DpadDown, 100), Events { nav("down") });   // newest wins
    QCOMPARE(tick(pad, 450), Events { nav("down", true) });
    // Down released while Right is still held: Right repeats after the
    // initial delay, without an extra immediate step.
    QVERIFY(release(pad, Button::DpadDown, 500).empty());
    QVERIFY(tick(pad, 849).empty());
    QCOMPARE(tick(pad, 850), Events { nav("right", true) });
}

void TestPadInput::stickDeadzoneHysteresis()
{
    PadState pad;
    QVERIFY(axis(pad, Axis::LeftX, 0.49f, 0).empty());              // inside the deadzone
    QCOMPARE(axis(pad, Axis::LeftX, 0.5f, 10), Events { nav("right") });
    QVERIFY(axis(pad, Axis::LeftX, 0.42f, 20).empty());             // hysteresis: still held
    QCOMPARE(pad.heldDirection(), std::optional<Dir>(Dir::Right));
    QVERIFY(axis(pad, Axis::LeftX, 0.39f, 30).empty());             // released
    QVERIFY(!pad.heldDirection());
    QVERIFY(axis(pad, Axis::LeftX, 0.45f, 40).empty());             // not past 0.5 again
    QCOMPARE(axis(pad, Axis::LeftX, 0.55f, 50), Events { nav("right") });
    // Negative Y is up (SDL).
    axis(pad, Axis::LeftX, 0.0f, 60);
    QCOMPARE(axis(pad, Axis::LeftY, -0.7f, 70), Events { nav("up") });
    // Noise and NaN are ignored.
    QVERIFY(axis(pad, Axis::LeftY, std::numeric_limits<float>::quiet_NaN(), 80).empty());
    QCOMPARE(pad.heldDirection(), std::optional<Dir>(Dir::Up));
    // The right stick is not used.
    QVERIFY(axis(pad, Axis::RightX, 1.0f, 90).empty());
}

void TestPadInput::stickDominantAxis()
{
    PadState pad;
    // A diagonal picks the dominant axis.
    axis(pad, Axis::LeftX, 0.6f, 0);
    QCOMPARE(pad.heldDirection(), std::optional<Dir>(Dir::Right));
    // Some Y as well, but not dominant: stays right.
    QVERIFY(axis(pad, Axis::LeftY, 0.58f, 10).empty());
    // Y past the threshold but not clearly dominant (< 1.3x): stays right.
    QVERIFY(axis(pad, Axis::LeftY, 0.7f, 15).empty());
    // Clearly dominant: switches to down.
    QCOMPARE(axis(pad, Axis::LeftY, 0.8f, 20), Events { nav("down") });
    // X grows back but below Y: stays down.
    QVERIFY(axis(pad, Axis::LeftX, 0.65f, 30).empty());
    QCOMPARE(pad.heldDirection(), std::optional<Dir>(Dir::Down));

    // Fresh engage on a diagonal below the threshold on both axes: nothing.
    PadState other;
    axis(other, Axis::LeftX, 0.45f, 0);
    QVERIFY(axis(other, Axis::LeftY, 0.45f, 1).empty());
    QVERIFY(!other.heldDirection());
}

void TestPadInput::stickFlickToOpposite()
{
    PadState pad;
    axis(pad, Axis::LeftX, 0.9f, 0);
    // One event straight to the other side (SDL may skip the middle).
    QCOMPARE(axis(pad, Axis::LeftX, -0.9f, 10), Events { nav("left") });
}

void TestPadInput::stickDiagonalJitterDoesNotFlip()
{
    // The review's case: (0.70, -0.70) +- 0.01 used to flip right/up/right/up.
    PadState pad;
    Events all = axis(pad, Axis::LeftX, 0.72f, 0);   // engages right
    quint32 seed = 12345;
    const auto noise = [&seed] {
        seed = seed * 1103515245u + 12345u;
        return (float((seed >> 16) % 2001) / 1000.0f - 1.0f) * 0.01f;   // -0.01 .. 0.01
    };
    for (qint64 t = 2; t < 2000; t += 2) {
        Events out;
        pad.axis(Axis::LeftX, 0.70f + noise(), t, out);
        pad.axis(Axis::LeftY, -0.70f + noise(), t, out);
        for (const Event &e : out) {
            if (!e.repeat)
                all.push_back(e);
        }
    }
    QCOMPARE(all.size(), size_t(1));   // engaged once, never flipped
    // A clear move to the other axis still switches.
    QCOMPARE(axis(pad, Axis::LeftX, 0.3f, 3000), Events { nav("up") });
}

void TestPadInput::tokenBucket()
{
    TokenBucket bucket(10, 10);
    int taken = 0;
    for (int i = 0; i < 50; ++i)
        taken += bucket.take(1000) ? 1 : 0;
    QCOMPARE(taken, 10);                 // the burst
    QVERIFY(!bucket.take(1050));         // half a token later: no
    QVERIFY(bucket.take(1100));          // 100 ms = one token
    QVERIFY(!bucket.take(1100));
    taken = 0;
    for (qint64 t = 2000; t < 3000; t += 10)
        taken += bucket.take(t) ? 1 : 0;
    QVERIFY(taken >= 18 && taken <= 21);   // refilled burst + ~10/s
}

void TestPadInput::triggerHysteresis()
{
    PadState pad;
    QVERIFY(axis(pad, Axis::LeftTrigger, 0.49f, 0).empty());
    QCOMPARE(axis(pad, Axis::LeftTrigger, 0.5f, 1), Events { act(Action::PageUp) });
    QVERIFY(axis(pad, Axis::LeftTrigger, 0.35f, 2).empty());        // not released yet
    QVERIFY(axis(pad, Axis::LeftTrigger, 0.9f, 3).empty());         // no second page
    QVERIFY(axis(pad, Axis::LeftTrigger, 0.3f, 4).empty());         // released
    QCOMPARE(axis(pad, Axis::LeftTrigger, 0.6f, 5), Events { act(Action::PageUp) });
    QCOMPARE(axis(pad, Axis::RightTrigger, 1.0f, 6), Events { act(Action::PageDown) });
    // Held triggers never repeat.
    for (qint64 t = 6; t < 2000; t += 10)
        QVERIFY(tick(pad, t).empty());
}

void TestPadInput::guidePressAndHold()
{
    PadState pad;
    const Events down = press(pad, Button::Guide, 100);
    QCOMPARE(down.size(), size_t(1));
    QCOMPARE(down[0].kind, Kind::GuidePressed);
    QCOMPARE(pad.nextDeadline(), std::optional<qint64>(1100));
    QVERIFY(tick(pad, 1099).empty());
    const Events held = tick(pad, 1100);
    QCOMPARE(held.size(), size_t(1));
    QCOMPARE(held[0].kind, Kind::GuideHeld);
    QVERIFY(tick(pad, 3000).empty());                                // once per hold
    QVERIFY(!pad.nextDeadline());
    QVERIFY(release(pad, Button::Guide, 3100).empty());
    // A short press: no hold.
    press(pad, Button::Guide, 4000);
    release(pad, Button::Guide, 4200);
    QVERIFY(tick(pad, 6000).empty());
}

void TestPadInput::latchIgnoresWhatIsHeld()
{
    PadState pad;
    axis(pad, Axis::LeftX, 0.9f, 0);
    press(pad, Button::DpadUp, 10);
    axis(pad, Axis::RightTrigger, 1.0f, 20);
    pad.latchHeld();   // an app starts (or exits)
    QVERIFY(!pad.heldDirection());
    for (qint64 t = 20; t < 2000; t += 10)
        QVERIFY(tick(pad, t).empty());
    // Still deflected: nothing until the stick has come back.
    QVERIFY(axis(pad, Axis::LeftX, 1.0f, 2000).empty());
    QVERIFY(axis(pad, Axis::LeftX, 0.6f, 2010).empty());
    QVERIFY(axis(pad, Axis::LeftX, 0.1f, 2020).empty());
    QCOMPARE(axis(pad, Axis::LeftX, 0.8f, 2030), Events { nav("right") });
    // Trigger: needs a release first.
    QVERIFY(axis(pad, Axis::RightTrigger, 0.9f, 2040).empty());
    axis(pad, Axis::RightTrigger, 0.0f, 2050);
    QCOMPARE(axis(pad, Axis::RightTrigger, 0.9f, 2060), Events { act(Action::PageDown) });
    // The D-pad release of the latched press is harmless.
    QVERIFY(release(pad, Button::DpadUp, 2070).empty());
}

void TestPadInput::nextDeadline()
{
    PadState pad;
    QVERIFY(!pad.nextDeadline());
    press(pad, Button::DpadUp, 0);
    QCOMPARE(pad.nextDeadline(), std::optional<qint64>(350));
    tick(pad, 350);
    QCOMPARE(pad.nextDeadline(), std::optional<qint64>(440));
    press(pad, Button::Guide, 400);
    QCOMPARE(pad.nextDeadline(), std::optional<qint64>(440));
    release(pad, Button::DpadUp, 410);
    QCOMPARE(pad.nextDeadline(), std::optional<qint64>(1400));
}

void TestPadInput::gateRunningOnlyGuideHeld()
{
    InputRouter router;
    router.setBlocked(true);   // Main.qml: blocked = Launcher.running
    QSignalSpy received(&router, &InputRouter::received);
    QSignalSpy held(&router, &InputRouter::guideHeld);
    InputGate gate(&router, InputRouter::Source::Gamepad);
    gate.setAppRunning(true);

    PadState pad;
    Events out;
    pad.button(Button::South, true, 0, out);
    pad.button(Button::DpadRight, true, 0, out);
    pad.button(Button::Start, true, 0, out);
    pad.button(Button::Guide, true, 0, out);
    pad.tick(400, out);
    for (const Event &e : out)
        QVERIFY(!gate.handle(e));            // nothing posted, not even Guide's press
    out.clear();
    pad.tick(1000, out);
    QCOMPARE(out.size(), size_t(2));         // a Navigate repeat and the hold
    QVERIFY(!gate.handle(out[0]));
    QCOMPARE(out[1].kind, Kind::GuideHeld);
    QVERIFY(gate.handle(out[1]));            // the hold gets through ...
    QTRY_COMPARE(held.count(), 1);           // ... even though the router is blocked
    QCOMPARE(held.first().at(0).value<InputRouter::Source>(), InputRouter::Source::Gamepad);
    QCOMPARE(received.count(), 0);

    // Not running: a hold is just a held Guide (it already went Home).
    gate.setAppRunning(false);
    QVERIFY(!gate.handle({ Kind::GuideHeld, Action::None, {}, false }));
}

void TestPadInput::gateGuidePressMeansHome()
{
    InputRouter router;
    QSignalSpy triggered(&router, &InputRouter::triggered);
    InputGate gate(&router, InputRouter::Source::Gamepad);
    QVERIFY(gate.handle({ Kind::GuidePressed, Action::None, {}, false }));
    QTRY_COMPARE(triggered.count(), 1);
    QCOMPARE(triggered.first().at(0).value<Action>(), Action::Guide);
}

void TestPadInput::gateCoalescesQueuedRepeats()
{
    InputRouter router;
    QSignalSpy triggered(&router, &InputRouter::triggered);
    InputGate gate(&router, InputRouter::Source::Gamepad);

    QVERIFY(gate.handle(nav("right")));                 // first press: posted
    for (int i = 0; i < 10; ++i)
        QVERIFY(!gate.handle(nav("right", true)));     // GUI hasn't run: coalesced
    QCOMPARE(gate.coalesced(), 10);
    // Other directions have their own flag; first presses always go through.
    QVERIFY(gate.handle(nav("left", true)));
    QVERIFY(gate.handle(nav("right")));
    QCoreApplication::processEvents();
    QCOMPARE(triggered.count(), 3);
    // Delivered: the next repeat is posted again.
    QVERIFY(gate.handle(nav("right", true)));
    QTRY_COMPARE(triggered.count(), 4);
    // Invalid directions are dropped.
    QVERIFY(!gate.handle(nav("sideways")));
}

void TestPadInput::stalledGuiDoesNotOvershoot()
{
    InputRouter router;
    QStringList delivered;
    connect(&router, &InputRouter::triggered, this, [&](Action, const QString &arg) { delivered.append(arg); });
    InputGate gate(&router, InputRouter::Source::Gamepad);
    PadState pad;

    // Hold right for 3 s while the GUI thread is stuck: ~40 repeats are due,
    // but only the press is queued.
    Events out;
    pad.button(Button::DpadRight, true, 0, out);
    for (qint64 t = 0; t <= 3000; t += 5)
        pad.tick(t, out);
    QVERIFY(out.size() > 30);
    gate.handle(out);
    pad.button(Button::DpadRight, false, 3000, out);
    QCoreApplication::processEvents();
    QCOMPARE(delivered, QStringList { QStringLiteral("right") });
}

QTEST_GUILESS_MAIN(TestPadInput)
#include "tst_padinput.moc"
