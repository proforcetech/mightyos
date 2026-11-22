#include "inputrouter.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTemporaryDir>
#include <QWindow>

#include <atomic>

using Action = InputRouter::Action;
using Source = InputRouter::Source;

class TestInputRouter : public QObject
{
    Q_OBJECT

private slots:
    void keyTable_data();
    void keyTable();
    void repeatPolicy();
    void muteWindow();
    void blockedConsumesButDrops();
    void navigateNeedsDirection();
    void postActionIsQueuedToGuiThread();
    void postActionAppliesPolicyAtDelivery();
    void lastSourceFollowsInput();
    void windowFilterMapsKeysAndBackButton();
    void mappedKeysNeverReachQuickItems();
    void textInputKeepsItsKeys();
    void cursorAutoHide();
    void pointerMovedNeedsRealMovement();
};

namespace {

bool sendKey(InputRouter &router, int key, Qt::KeyboardModifiers mods = Qt::NoModifier,
             bool autoRepeat = false)
{
    QKeyEvent event(QEvent::KeyPress, key, mods, QString(), autoRepeat);
    return router.handleKeyEvent(&event);
}

} // namespace

void TestInputRouter::keyTable_data()
{
    QTest::addColumn<int>("key");
    QTest::addColumn<int>("mods");
    QTest::addColumn<Action>("action");
    QTest::addColumn<QString>("arg");

    const int none = Qt::NoModifier;
    QTest::newRow("left") << int(Qt::Key_Left) << none << Action::Navigate << QStringLiteral("left");
    QTest::newRow("down") << int(Qt::Key_Down) << none << Action::Navigate << QStringLiteral("down");
    QTest::newRow("keypad-up") << int(Qt::Key_Up) << int(Qt::KeypadModifier) << Action::Navigate << QStringLiteral("up");
    QTest::newRow("enter") << int(Qt::Key_Return) << none << Action::Accept << QString();
    QTest::newRow("keypad-enter") << int(Qt::Key_Enter) << int(Qt::KeypadModifier) << Action::Accept << QString();
    QTest::newRow("space") << int(Qt::Key_Space) << none << Action::Accept << QString();
    QTest::newRow("esc") << int(Qt::Key_Escape) << none << Action::Back << QString();
    QTest::newRow("backspace") << int(Qt::Key_Backspace) << none << Action::Back << QString();
    QTest::newRow("remote-back") << int(Qt::Key_Back) << none << Action::Back << QString();
    QTest::newRow("f") << int(Qt::Key_F) << none << Action::Favorite << QString();
    QTest::newRow("slash") << int(Qt::Key_Slash) << none << Action::Search << QString();
    QTest::newRow("shift-slash") << int(Qt::Key_Slash) << int(Qt::ShiftModifier) << Action::Search << QString();
    QTest::newRow("ctrl-f") << int(Qt::Key_F) << int(Qt::ControlModifier) << Action::Search << QString();
    QTest::newRow("m") << int(Qt::Key_M) << none << Action::Options << QString();
    QTest::newRow("menu") << int(Qt::Key_Menu) << none << Action::Options << QString();
    QTest::newRow("q") << int(Qt::Key_Q) << none << Action::QuickPanel << QString();
    QTest::newRow("pgup") << int(Qt::Key_PageUp) << none << Action::PrevCategory << QString();
    QTest::newRow("pgdn") << int(Qt::Key_PageDown) << none << Action::NextCategory << QString();
    QTest::newRow("home") << int(Qt::Key_Home) << none << Action::PageUp << QString();
    QTest::newRow("end") << int(Qt::Key_End) << none << Action::PageDown << QString();
    QTest::newRow("ctrl-left") << int(Qt::Key_Left) << int(Qt::ControlModifier) << Action::None << QString();
    QTest::newRow("alt-f") << int(Qt::Key_F) << int(Qt::AltModifier) << Action::None << QString();
    QTest::newRow("ctrl-q") << int(Qt::Key_Q) << int(Qt::ControlModifier) << Action::None << QString();
    QTest::newRow("shift-f") << int(Qt::Key_F) << int(Qt::ShiftModifier) << Action::None << QString();
    QTest::newRow("shift-m") << int(Qt::Key_M) << int(Qt::ShiftModifier) << Action::None << QString();
    QTest::newRow("shift-q") << int(Qt::Key_Q) << int(Qt::ShiftModifier) << Action::None << QString();
    QTest::newRow("shift-space") << int(Qt::Key_Space) << int(Qt::ShiftModifier) << Action::None << QString();
    QTest::newRow("shift-left") << int(Qt::Key_Left) << int(Qt::ShiftModifier) << Action::None << QString();
    QTest::newRow("ctrl-slash") << int(Qt::Key_Slash) << int(Qt::ControlModifier) << Action::None << QString();
    QTest::newRow("a") << int(Qt::Key_A) << none << Action::None << QString();
    QTest::newRow("tab") << int(Qt::Key_Tab) << none << Action::None << QString();
}

void TestInputRouter::keyTable()
{
    QFETCH(int, key);
    QFETCH(int, mods);
    QFETCH(Action, action);
    QFETCH(QString, arg);
    const auto binding = InputRouter::keyActionFor(key, Qt::KeyboardModifiers(mods));
    QCOMPARE(binding.action, action);
    QCOMPARE(binding.arg, arg);
}

void TestInputRouter::repeatPolicy()
{
    InputRouter router;
    QSignalSpy spy(&router, &InputRouter::triggered);

    // Auto-repeated Enter is consumed but never delivered (no double launch).
    QVERIFY(sendKey(router, Qt::Key_Return, Qt::NoModifier, true));
    QVERIFY(sendKey(router, Qt::Key_Escape, Qt::NoModifier, true));
    QVERIFY(sendKey(router, Qt::Key_PageDown, Qt::NoModifier, true));
    QCOMPARE(spy.count(), 0);

    // Navigation repeats.
    QVERIFY(sendKey(router, Qt::Key_Right, Qt::NoModifier, true));
    QVERIFY(sendKey(router, Qt::Key_Right, Qt::NoModifier, true));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).at(0).value<Action>(), Action::Navigate);
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("right"));

    // First press of a non-repeatable key goes through.
    QVERIFY(sendKey(router, Qt::Key_Return));
    QCOMPARE(spy.count(), 3);
    QCOMPARE(spy.at(2).at(0).value<Action>(), Action::Accept);

    // Unmapped keys are not consumed.
    QVERIFY(!sendKey(router, Qt::Key_A));
}

void TestInputRouter::muteWindow()
{
    InputRouter router;
    QSignalSpy spy(&router, &InputRouter::triggered);
    QSignalSpy muted(&router, &InputRouter::mutedChanged);

    router.muteFor(150);
    QVERIFY(router.isMuted());
    QVERIFY(!router.isAccepting());
    QCOMPARE(muted.count(), 1);

    QVERIFY(sendKey(router, Qt::Key_Return));     // consumed ...
    QVERIFY(!router.dispatch(Action::Accept));
    QCOMPARE(spy.count(), 0);                      // ... but dropped

    QVERIFY(muted.wait(1000));                     // mute ends by itself
    QVERIFY(!router.isMuted());
    QVERIFY(router.isAccepting());
    QVERIFY(sendKey(router, Qt::Key_Return));
    QCOMPARE(spy.count(), 1);

    // muteFor(0) cancels a running mute.
    router.muteFor(10000);
    router.muteFor(0);
    QVERIFY(router.isAccepting());
}

void TestInputRouter::blockedConsumesButDrops()
{
    InputRouter router;
    QSignalSpy spy(&router, &InputRouter::triggered);
    QSignalSpy accepting(&router, &InputRouter::acceptingChanged);

    router.setBlocked(true);
    QVERIFY(!router.isAccepting());
    QCOMPARE(accepting.count(), 1);
    QVERIFY(sendKey(router, Qt::Key_Return));
    QVERIFY(sendKey(router, Qt::Key_Left));
    QVERIFY(!router.dispatch(Action::Back));
    QVERIFY(!router.pointerAction());
    QCOMPARE(spy.count(), 0);

    router.setBlocked(false);
    QCOMPARE(accepting.count(), 2);
    QVERIFY(router.pointerAction());
    QVERIFY(router.dispatch(Action::Back));
    QCOMPARE(spy.count(), 1);
}

void TestInputRouter::navigateNeedsDirection()
{
    InputRouter router;
    QSignalSpy spy(&router, &InputRouter::triggered);
    QVERIFY(!router.dispatch(Action::Navigate, QStringLiteral("sideways")));
    QVERIFY(!router.dispatch(Action::Navigate, QString()));
    QVERIFY(!router.dispatch(Action::None));
    QCOMPARE(spy.count(), 0);
    QVERIFY(router.dispatch(Action::Navigate, QStringLiteral("up")));
    QCOMPARE(spy.count(), 1);
}

// Stand-in for the SDL3 / CEC input threads of M3.
class PosterThread : public QThread
{
public:
    PosterThread(InputRouter *router, int count) : m_router(router), m_count(count) {}
    std::atomic<bool> done { false };

protected:
    void run() override
    {
        for (int i = 0; i < m_count; ++i)
            m_router->postAction(Action::Navigate,
                                 i % 2 == 0 ? QStringLiteral("left") : QStringLiteral("right"),
                                 true, Source::Gamepad);
        m_router->postAction(Action::Accept, {}, false, Source::Gamepad);
        done = true;
    }

private:
    InputRouter *m_router;
    int m_count;
};

void TestInputRouter::postActionIsQueuedToGuiThread()
{
    InputRouter router;
    QList<QThread *> deliveryThreads;
    QStringList args;
    connect(&router, &InputRouter::triggered, this, [&](Action, const QString &arg) {
        deliveryThreads.append(QThread::currentThread());
        args.append(arg);
    });

    constexpr int kCount = 200;
    PosterThread poster(&router, kCount);
    poster.start();
    QVERIFY(poster.wait(5000));
    QVERIFY(poster.done);
    // Nothing is delivered on the posting thread or synchronously.
    QCOMPARE(deliveryThreads.size(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(deliveryThreads.size(), kCount + 1, 5000);
    for (QThread *t : std::as_const(deliveryThreads))
        QCOMPARE(t, QCoreApplication::instance()->thread());
    // Order is preserved.
    for (int i = 0; i < kCount; ++i)
        QCOMPARE(args.at(i), i % 2 == 0 ? QStringLiteral("left") : QStringLiteral("right"));
    QCOMPARE(router.lastSource(), Source::Gamepad);
}

void TestInputRouter::postActionAppliesPolicyAtDelivery()
{
    InputRouter router;
    QSignalSpy spy(&router, &InputRouter::triggered);

    // Posted while accepting, delivered while blocked: dropped.
    router.postAction(Action::Accept, {}, false, Source::Remote);
    router.setBlocked(true);
    QCoreApplication::processEvents();
    QCOMPARE(spy.count(), 0);
    router.setBlocked(false);

    // Repeat policy applies to posted actions too.
    router.postAction(Action::Accept, {}, true, Source::Gamepad);
    router.postAction(Action::Navigate, QStringLiteral("down"), true, Source::Gamepad);
    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<Action>(), Action::Navigate);
}

void TestInputRouter::lastSourceFollowsInput()
{
    InputRouter router;
    QSignalSpy changed(&router, &InputRouter::lastSourceChanged);
    QCOMPARE(router.lastSource(), Source::Keyboard);

    router.postAction(Action::Navigate, QStringLiteral("left"), false, Source::Remote);
    QTRY_COMPARE(router.lastSource(), Source::Remote);
    router.pointerAction();
    QCOMPARE(router.lastSource(), Source::Mouse);
    sendKey(router, Qt::Key_Left);
    QCOMPARE(router.lastSource(), Source::Keyboard);
    QCOMPARE(changed.count(), 3);
}

void TestInputRouter::windowFilterMapsKeysAndBackButton()
{
    QWindow window;
    window.resize(200, 100);
    InputRouter router;
    router.attachToWindow(&window);
    QSignalSpy spy(&router, &InputRouter::triggered);

    QKeyEvent press(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
    QKeyEvent unmapped(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QVERIFY(router.eventFilter(&window, &press));
    QVERIFY(router.eventFilter(&window, &release));     // releases are consumed, not delivered
    QVERIFY(!router.eventFilter(&window, &unmapped));
    QCOMPARE(spy.count(), 1);

    // Mouse back button = Back.
    const QPointF pos(10, 10);
    QMouseEvent backPress(QEvent::MouseButtonPress, pos, pos, Qt::BackButton, Qt::BackButton, Qt::NoModifier);
    QMouseEvent backRelease(QEvent::MouseButtonRelease, pos, pos, Qt::BackButton, Qt::NoButton, Qt::NoModifier);
    QVERIFY(router.eventFilter(&window, &backPress));
    QVERIFY(router.eventFilter(&window, &backRelease));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).value<Action>(), Action::Back);
    QCOMPARE(router.lastSource(), Source::Mouse);

    // Left clicks are left to Qt Quick.
    QMouseEvent leftPress(QEvent::MouseButtonPress, pos, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(!router.eventFilter(&window, &leftPress));

    // Events for other objects are ignored.
    QObject other;
    QVERIFY(!router.eventFilter(&other, &press));
    QCOMPARE(spy.count(), 2);
}

// End to end through a real QQuickView: a focused item with Keys.onPressed
// must never see a mapped key (the router consumes it first), but still
// gets unmapped ones.
void TestInputRouter::mappedKeysNeverReachQuickItems()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString qmlPath = dir.filePath(QStringLiteral("KeySink.qml"));
    {
        QFile file(qmlPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("import QtQuick\n"
                   "Item {\n"
                   "    property var seen: []\n"
                   "    focus: true\n"
                   "    Keys.onPressed: (event) => { seen = seen.concat([event.key]); event.accepted = true }\n"
                   "}\n");
    }
    QQuickView view;
    view.setSource(QUrl::fromLocalFile(qmlPath));
    QVERIFY(view.rootObject());
    view.resize(200, 100);
    view.rootObject()->forceActiveFocus();
    view.show();
    view.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&view));

    InputRouter router;
    router.attachToWindow(&view);
    QSignalSpy spy(&router, &InputRouter::triggered);

    const QList<Qt::Key> mapped { Qt::Key_Down, Qt::Key_Return, Qt::Key_Escape, Qt::Key_Backspace,
                                  Qt::Key_F, Qt::Key_M, Qt::Key_Q, Qt::Key_PageDown, Qt::Key_End };
    for (Qt::Key key : mapped)
        QTest::keyClick(&view, key);
    QCOMPARE(spy.count(), mapped.size());
    QCOMPARE(view.rootObject()->property("seen").toList().size(), 0);

    QTest::keyClick(&view, Qt::Key_A);
    QTest::keyClick(&view, Qt::Key_F, Qt::ShiftModifier);   // unmapped chord (sends Shift, then F)
    const QVariantList seen = view.rootObject()->property("seen").toList();
    QVERIFY(seen.size() >= 2);
    QCOMPARE(seen.first().toInt(), int(Qt::Key_A));
    QCOMPARE(seen.last().toInt(), int(Qt::Key_F));
    QCOMPARE(spy.count(), mapped.size());
}

void TestInputRouter::textInputKeepsItsKeys()
{
    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.loadFromModule("QtQuick", "TextInput");
    QVERIFY(view.rootObject());
    view.resize(300, 60);
    view.rootObject()->setProperty("text", QStringLiteral("abc"));
    view.rootObject()->forceActiveFocus();
    view.show();
    view.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&view));

    InputRouter router;
    router.attachToWindow(&view);
    QSignalSpy spy(&router, &InputRouter::triggered);

    // Backspace / Left / Space edit the text instead of becoming actions.
    view.rootObject()->setProperty("cursorPosition", 3);
    QTest::keyClick(&view, Qt::Key_Backspace);
    QTest::keyClick(&view, Qt::Key_Left);
    QCOMPARE(spy.count(), 0);
    QCOMPARE(view.rootObject()->property("text").toString(), QStringLiteral("ab"));

    // Esc still means Back.
    QTest::keyClick(&view, Qt::Key_Escape);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<Action>(), Action::Back);
}

void TestInputRouter::cursorAutoHide()
{
    QWindow window;
    window.resize(200, 100);
    InputRouter router;
    router.setCursorHideDelay(60);
    router.attachToWindow(&window);
    QVERIFY(!router.isCursorHidden());

    // Idle -> hidden.
    QTRY_VERIFY_WITH_TIMEOUT(router.isCursorHidden(), 2000);
    QVERIFY(QGuiApplication::overrideCursor());

    // Real mouse movement reveals it, without turning that same move into
    // a hover-focus.
    const QPointF pos(40, 40);
    QMouseEvent move(QEvent::MouseMove, pos, pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    router.eventFilter(&window, &move);
    QVERIFY(!router.isCursorHidden());
    QVERIFY(!QGuiApplication::overrideCursor());
    QVERIFY(!router.pointerMoved(pos));

    // Key input hides it immediately; mouse-sourced actions don't.
    router.dispatch(Action::Accept, {}, Source::Mouse);
    QVERIFY(!router.isCursorHidden());
    sendKey(router, Qt::Key_Left);
    QVERIFY(router.isCursorHidden());

    router.eventFilter(&window, &move);
    QVERIFY(!router.isCursorHidden());
    router.postAction(Action::Navigate, QStringLiteral("left"), false, Source::Gamepad);
    QTRY_VERIFY(router.isCursorHidden());
}

void TestInputRouter::pointerMovedNeedsRealMovement()
{
    InputRouter router;
    QVERIFY(!router.pointerMoved({ 100, 100 }));    // first sample only records
    QVERIFY(!router.pointerMoved({ 101, 100 }));    // below the slop
    QVERIFY(router.pointerMoved({ 110, 100 }));
    QVERIFY(!router.pointerMoved({ 110, 100 }));    // synthetic hover, same spot
}

QTEST_MAIN(TestInputRouter)
#include "tst_inputrouter.moc"
