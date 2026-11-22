#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QKeyEvent;
class QWindow;

// InputRouter is the single entry point for user input (PLAN.md section 3).
//
// Every device is reduced to a small set of named actions. Screens only ever
// see `triggered(action, arg)` and never look at raw keys or buttons, so the
// keyboard, the mouse, the SDL3 gamepad (M3) and the HDMI-CEC remote (M3)
// all behave the same.
//
// Policy owned here:
//  - the keyboard key -> action table (keyActionFor()),
//  - the repeat policy: only Navigate may auto-repeat; every other action
//    drops repeats so a held Enter cannot launch twice,
//  - `blocked` (bound to Launcher.running in QML) and the short mute window
//    after a game exits (muteFor()). Mapped input is still *consumed* while
//    blocked or muted, so it never leaks to other items,
//  - `lastSource`, so prompts can later show device-specific glyphs,
//  - mouse cursor auto-hide (3 s idle, or immediately on key/pad input).
//
// Threading: InputRouter lives on the GUI thread. postAction() is the only
// member that may be called from other threads (SDL3 gamepad thread, CEC
// thread). It marshals to the GUI thread with a queued invocation; blocked,
// muted and repeat checks happen at *delivery* time on the GUI thread.
class InputRouter : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool blocked READ isBlocked WRITE setBlocked NOTIFY blockedChanged FINAL)
    Q_PROPERTY(bool muted READ isMuted NOTIFY mutedChanged FINAL)
    Q_PROPERTY(bool accepting READ isAccepting NOTIFY acceptingChanged FINAL)
    Q_PROPERTY(Source lastSource READ lastSource NOTIFY lastSourceChanged FINAL)
    Q_PROPERTY(bool cursorHidden READ isCursorHidden NOTIFY cursorHiddenChanged FINAL)
    Q_PROPERTY(int cursorHideDelay READ cursorHideDelay WRITE setCursorHideDelay
               NOTIFY cursorHideDelayChanged FINAL)

public:
    enum class Action {
        None,
        Navigate,       // arg: "left" | "right" | "up" | "down"
        Accept,
        Back,
        Favorite,
        Search,
        Options,
        QuickPanel,
        PrevCategory,
        NextCategory,
        PageUp,
        PageDown,
    };
    Q_ENUM(Action)

    enum class Source { Keyboard, Mouse, Gamepad, Remote };
    Q_ENUM(Source)

    struct KeyBinding {
        Action action = Action::None;
        QString arg;
    };

    explicit InputRouter(QObject *parent = nullptr);
    ~InputRouter() override;

    bool isBlocked() const { return m_blocked; }
    void setBlocked(bool blocked);
    bool isMuted() const { return m_muteTimer.isActive(); }
    bool isAccepting() const { return !m_blocked && !isMuted(); }
    Source lastSource() const { return m_lastSource; }
    bool isCursorHidden() const { return m_cursorHidden; }
    int cursorHideDelay() const { return m_cursorHideTimer.interval(); }
    void setCursorHideDelay(int ms);

    // Install on the application window. Keys and mouse buttons are read in
    // eventFilter() before Qt Quick delivers them to items.
    void attachToWindow(QWindow *window);

    // The keyboard table. Returns Action::None for unmapped keys.
    static KeyBinding keyActionFor(int key, Qt::KeyboardModifiers modifiers);
    static bool isRepeatable(Action action) { return action == Action::Navigate; }

    // Thread-safe. May be called from any thread; delivery is always queued
    // to the GUI thread (even when called on it), so ordering between posted
    // actions is preserved.
    void postAction(Action action, const QString &arg, bool isRepeat, Source source);

    // GUI thread only. For QML (mouse clicks, prompt buttons). Returns true
    // only if the action was delivered (valid, accepting, not filtered).
    Q_INVOKABLE bool dispatch(InputRouter::Action action, const QString &arg = QString(),
                              InputRouter::Source source = InputRouter::Source::Mouse);

    // Ignore all input for `ms` milliseconds (e.g. right after a game exits).
    Q_INVOKABLE void muteFor(int ms);

    // Mouse hover may only move focus after the pointer really moved: items
    // scrolling under a resting cursor also generate hover events. Returns
    // true if hover at `scenePos` should take focus.
    Q_INVOKABLE bool pointerMoved(QPointF scenePos);

    // A pointer click or wheel step that acts on the item under the pointer
    // directly (a tile activating itself). Records the mouse as source and
    // returns whether input is currently accepted (not blocked or muted).
    Q_INVOKABLE bool pointerAction();

    Q_INVOKABLE QString actionName(InputRouter::Action action) const { return nameOf(action); }
    static QString nameOf(Action action);

    // Returns true if the key event was mapped (and therefore consumed).
    bool handleKeyEvent(QKeyEvent *event);

    // Window event filter (installed by attachToWindow()); public for tests.
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void triggered(InputRouter::Action action, const QString &arg);
    void blockedChanged();
    void mutedChanged();
    void acceptingChanged();
    void lastSourceChanged();
    void cursorHiddenChanged();
    void cursorHideDelayChanged();

private:
    // Returns true if `triggered` was emitted.
    bool deliver(Action action, const QString &arg, bool isRepeat, Source source);
    void setLastSource(Source source);
    void setCursorHidden(bool hidden);
    void pointerActivity(QPointF windowPos);
    bool focusWantsText() const;

    QPointer<QWindow> m_window;
    QTimer m_muteTimer;
    QTimer m_cursorHideTimer;
    QPointF m_lastPointer { -1, -1 };
    Source m_lastSource = Source::Keyboard;
    bool m_blocked = false;
    bool m_cursorHidden = false;
    bool m_overrideCursorSet = false;
};
