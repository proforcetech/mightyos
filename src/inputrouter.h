#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

class QKeyEvent;
class QWindow;

// InputRouter is the single entry point for user input (PLAN.md section 3).
//
// Every device is reduced to a small set of named actions. Screens only ever
// see `triggered(action, arg)` and never look at raw keys or buttons, so the
// keyboard, the mouse, the SDL3 gamepad (GamepadService) and the HDMI-CEC
// remote (CecService) all behave the same.
//
// Policy owned here:
//  - the keyboard key -> action table (keyActionFor()). Bare letters and
//    digits (also with Shift) are Action::Letter, type-to-jump in grids
//    (PLAN M4: the old F/M/Q action keys clashed with A-Z). The actions
//    use non-letter keys: Favorite = Insert / Ctrl+D, Options = Menu / Tab
//    / Shift+F10, Quick panel = F12, Search = / or Ctrl+F,
//  - the repeat policy: only Navigate may auto-repeat; every other action
//    drops repeats so a held Enter cannot launch twice,
//  - `blocked` (bound to Launcher.running in QML) and the short mute window
//    after a game exits (muteFor()). Mapped input is still *consumed* while
//    blocked or muted, so it never leaks to other items,
//  - `lastSource` and `gamepadStyle`, so prompts show device glyphs,
//  - the rumble policy (rumble(): enabled, nothing running, pad in use),
//  - mouse cursor auto-hide (3 s idle, or immediately on key/pad input).
//
// Guide: Action::Guide (Guide button, Ctrl+Home, HomePage key, CEC Root
// Menu) is an ordinary action, so it is dropped while blocked. Held while an
// app runs it becomes `guideHeld` instead (postGuideHeld()), which bypasses
// blocked/mute: it is the way back from a running app (Guide overlay, M6).
//
// Trust levels (PLAN D18): the TV remote (Source::Remote) is lower trust.
//  - remoteMode (Settings.tvRemote): RemoteOff drops all remote input;
//    RemoteNavigation (default) allows only Navigate, Accept, Back,
//    PageUp/Down, Prev/NextCategory and Guide; RemoteFull allows all.
//  - acceptRemote == false (a destructive confirmation, a secure screen)
//    or secureEntry: the remote may only Back / Guide out (both answer
//    with the safe choice), everything else from it is dropped.
//  - secureEntry (PIN pad, admin on-screen keyboard, M8; section 4): on top
//    of that, no action is logged and received() is not emitted, so debug
//    logs and diagnostics can't replay the entry. triggered() still is.
// Device names are sanitised at intake (TextSanitize::label).
//
// Threading: InputRouter lives on the GUI thread. The post*() members are
// the only ones that may be called from other threads (SDL3 gamepad thread,
// CEC thread). They marshal to the GUI thread with queued invocations, in
// call order; blocked, muted and repeat checks happen at *delivery* time on
// the GUI thread. Input threads must be stopped (joined) before the router
// is destroyed, see GamepadService / CecService.
class InputRouter : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool blocked READ isBlocked WRITE setBlocked NOTIFY blockedChanged FINAL)
    Q_PROPERTY(bool muted READ isMuted NOTIFY mutedChanged FINAL)
    Q_PROPERTY(bool accepting READ isAccepting NOTIFY acceptingChanged FINAL)
    Q_PROPERTY(Source lastSource READ lastSource NOTIFY lastSourceChanged FINAL)
    // Button family of the controller used last (glyphs). GUI thread.
    Q_PROPERTY(PadStyle gamepadStyle READ gamepadStyle WRITE setGamepadStyle
               NOTIFY gamepadStyleChanged FINAL)
    // Names of the connected controllers, in connection order.
    Q_PROPERTY(QStringList controllers READ controllers NOTIFY controllersChanged FINAL)
    // Bound to Settings.rumble in QML.
    Q_PROPERTY(bool rumbleEnabled READ isRumbleEnabled WRITE setRumbleEnabled
               NOTIFY rumbleEnabledChanged FINAL)
    // Bound to Settings.tvRemote in QML.
    Q_PROPERTY(RemoteMode remoteMode READ remoteMode WRITE setRemoteMode NOTIFY remoteModeChanged FINAL)
    // False while a remote-restricted dialog or screen is open (Main binds it).
    Q_PROPERTY(bool acceptRemote READ acceptRemote WRITE setAcceptRemote NOTIFY acceptRemoteChanged FINAL)
    // True while a secure entry (PIN pad, admin OSK) is open.
    Q_PROPERTY(bool secureEntry READ isSecureEntry WRITE setSecureEntry NOTIFY secureEntryChanged FINAL)
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
        Guide,          // Home (root screen, Favorites category)
        Letter,         // arg: "A".."Z" or "0".."9" (keyboard type-to-jump)
    };
    Q_ENUM(Action)

    enum class Source { Keyboard, Mouse, Gamepad, Remote };
    Q_ENUM(Source)

    // Face-button labels. Mapping is always positional (South = Accept);
    // only the glyphs differ. Unknown/generic pads use Xbox labels.
    enum class PadStyle { Xbox, PlayStation, Nintendo };
    Q_ENUM(PadStyle)

    // Haptic feedback kinds (rumble()).
    enum class Feedback { Bump, Error };
    Q_ENUM(Feedback)

    // TV remote trust level (Settings.tvRemote).
    enum class RemoteMode { RemoteOff, RemoteNavigation, RemoteFull };
    Q_ENUM(RemoteMode)

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
    PadStyle gamepadStyle() const { return m_gamepadStyle; }
    void setGamepadStyle(PadStyle style);
    QStringList controllers() const;
    bool isRumbleEnabled() const { return m_rumbleEnabled; }
    void setRumbleEnabled(bool enabled);
    RemoteMode remoteMode() const { return m_remoteMode; }
    void setRemoteMode(RemoteMode mode);
    bool acceptRemote() const { return m_acceptRemote; }
    void setAcceptRemote(bool accept);
    bool isSecureEntry() const { return m_secureEntry; }
    void setSecureEntry(bool secure);
    // The trust rule for one remote action (see class comment).
    static bool remoteAllows(Action action, RemoteMode mode, bool restricted);
    bool isCursorHidden() const { return m_cursorHidden; }
    int cursorHideDelay() const { return m_cursorHideTimer.interval(); }
    void setCursorHideDelay(int ms);

    // Install on the application window. Keys and mouse buttons are read in
    // eventFilter() before Qt Quick delivers them to items.
    void attachToWindow(QWindow *window);
    // Further windows whose input is ours too (the Guide overlay, a
    // separate layer-shell window). Same filtering as the main window.
    void addWindow(QWindow *window);
    void removeWindow(QWindow *window);

    // The keyboard table. Returns Action::None for unmapped keys.
    static KeyBinding keyActionFor(int key, Qt::KeyboardModifiers modifiers);
    static bool isRepeatable(Action action) { return action == Action::Navigate; }

    // Thread-safe. May be called from any thread; delivery is always queued
    // to the GUI thread (even when called on it), so ordering between posted
    // actions is preserved.
    void postAction(Action action, const QString &arg, bool isRepeat, Source source);
    // Same, and decrements `*pending` on the GUI thread once the action has
    // been delivered (or dropped). Input threads increment it before posting
    // and skip repeats while it is non-zero, so a stalled GUI thread never
    // finds a backlog of queued repeats (no overshoot when it catches up).
    void postAction(Action action, const QString &arg, bool isRepeat, Source source,
                    std::shared_ptr<std::atomic<int>> pending);
    // Thread-safe. Guide held while an app runs: emits guideHeld() even when
    // blocked or muted.
    Q_INVOKABLE void postGuideHeld(InputRouter::Source source);
    // Thread-safe. Sets gamepadStyle (posted before the action it belongs
    // to, so the glyphs are right when that action arrives).
    void postGamepadStyle(PadStyle style);
    // Thread-safe. A controller (or remote) appeared / went away. `id` is
    // unique per connection; `initial` marks devices present at startup.
    Q_INVOKABLE void postDeviceNotice(bool connected, quint32 id, const QString &name,
                                      InputRouter::Source source, bool initial);

    // GUI thread only. For QML (mouse clicks, prompt buttons). Returns true
    // only if the action was delivered (valid, accepting, not filtered).
    Q_INVOKABLE bool dispatch(InputRouter::Action action, const QString &arg = QString(),
                              InputRouter::Source source = InputRouter::Source::Mouse);

    // Ignore all input for `ms` milliseconds (e.g. right after a game exits).
    Q_INVOKABLE void muteFor(int ms);

    // GUI thread. Haptic feedback for a bump or an error. Emits
    // rumbleRequested() only if rumble is enabled, nothing runs (not
    // blocked) and the last input came from a gamepad; a Bump at most once
    // per held direction (repeats bumping at a row end stay quiet). Returns
    // whether it did. GamepadService checks "nothing runs" again on its own
    // thread.
    Q_INVOKABLE bool rumble(InputRouter::Feedback kind);

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
    // Every action that reached delivery, with the verdict (diagnostics,
    // mightyos-inputtest).
    void received(InputRouter::Action action, const QString &arg, bool isRepeat,
                  InputRouter::Source source, bool delivered);
    void guideHeld(InputRouter::Source source);
    // `initial`: present at startup (no toast for those).
    void deviceChanged(bool connected, const QString &name, InputRouter::Source source, bool initial);
    void rumbleRequested(InputRouter::Feedback kind);
    void gamepadStyleChanged();
    void controllersChanged();
    void rumbleEnabledChanged();
    void remoteModeChanged();
    void acceptRemoteChanged();
    void secureEntryChanged();
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
    QList<QPointer<QWindow>> m_extraWindows;
    QTimer m_muteTimer;
    QTimer m_cursorHideTimer;
    QPointF m_lastPointer { -1, -1 };
    Source m_lastSource = Source::Keyboard;
    PadStyle m_gamepadStyle = PadStyle::Xbox;
    struct Device { quint32 id; QString name; Source source; };
    QList<Device> m_devices;
    bool m_rumbleEnabled = true;
    bool m_bumpRumbled = false;
    RemoteMode m_remoteMode = RemoteMode::RemoteNavigation;
    bool m_acceptRemote = true;
    bool m_secureEntry = false;
    bool m_blocked = false;
    bool m_cursorHidden = false;
    bool m_overrideCursorSet = false;
};
