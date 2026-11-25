#include "inputrouter.h"

#include "textsanitize.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QThread>
#include <QWindow>

#include <algorithm>

// Info by default (guideHeld); QT_LOGGING_RULES="mightyos.input.debug=true"
// logs every action.
Q_LOGGING_CATEGORY(lcInput, "mightyos.input", QtInfoMsg)

namespace {

constexpr int kDefaultCursorHideMs = 3000;
// Hover only counts after the pointer moved at least this far (squared px).
constexpr qreal kPointerSlop2 = 4.0;

bool isDirection(const QString &arg)
{
    return arg == QLatin1StringView("left") || arg == QLatin1StringView("right")
        || arg == QLatin1StringView("up") || arg == QLatin1StringView("down");
}

} // namespace

InputRouter::InputRouter(QObject *parent)
    : QObject(parent)
{
    m_muteTimer.setSingleShot(true);
    connect(&m_muteTimer, &QTimer::timeout, this, [this] {
        emit mutedChanged();
        emit acceptingChanged();
    });

    m_cursorHideTimer.setSingleShot(true);
    m_cursorHideTimer.setInterval(kDefaultCursorHideMs);
    connect(&m_cursorHideTimer, &QTimer::timeout, this, [this] { setCursorHidden(true); });
}

InputRouter::~InputRouter()
{
    setCursorHidden(false);   // never leave an override cursor behind
}

void InputRouter::setBlocked(bool blocked)
{
    if (m_blocked == blocked)
        return;
    const bool wasAccepting = isAccepting();
    m_blocked = blocked;
    emit blockedChanged();
    if (wasAccepting != isAccepting())
        emit acceptingChanged();
}

void InputRouter::setCursorHideDelay(int ms)
{
    if (ms == m_cursorHideTimer.interval())
        return;
    m_cursorHideTimer.setInterval(ms);
    emit cursorHideDelayChanged();
}

void InputRouter::attachToWindow(QWindow *window)
{
    if (m_window == window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    m_window = window;
    if (m_window) {
        m_window->installEventFilter(this);
        m_cursorHideTimer.start();
    }
}

void InputRouter::addWindow(QWindow *window)
{
    m_extraWindows.removeAll(nullptr);
    if (!window || window == m_window || m_extraWindows.contains(window))
        return;
    m_extraWindows.append(window);
    window->installEventFilter(this);
}

void InputRouter::removeWindow(QWindow *window)
{
    if (!window || !m_extraWindows.contains(window))
        return;
    m_extraWindows.removeAll(window);
    window->removeEventFilter(this);
}

InputRouter::KeyBinding InputRouter::keyActionFor(int key, Qt::KeyboardModifiers modifiers)
{
    // Keypad arrows/Enter carry KeypadModifier, which changes nothing.
    // Shift is accepted for '/' (it needs Shift on e.g. German layouts),
    // for letters (Caps-style typing) and for Shift+F10 (the standard
    // context-menu key); other Shift/Ctrl/Alt/Meta chords stay unmapped.
    const Qt::KeyboardModifiers mods = modifiers & ~Qt::KeypadModifier;

    if (mods == Qt::ControlModifier && key == Qt::Key_F)
        return { Action::Search, {} };
    if (mods == Qt::ControlModifier && key == Qt::Key_D)
        return { Action::Favorite, {} };
    if (mods == Qt::ControlModifier && key == Qt::Key_Home)
        return { Action::Guide, {} };
    if (mods == Qt::ShiftModifier && key == Qt::Key_Slash)
        return { Action::Search, {} };
    if (mods == Qt::ShiftModifier && key == Qt::Key_F10)
        return { Action::Options, {} };
    if ((mods == Qt::NoModifier || mods == Qt::ShiftModifier)
        && ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)))
        return { Action::Letter, QString(QChar(key)) };
    if (mods != Qt::NoModifier)
        return {};   // leave the other chords alone

    switch (key) {
    case Qt::Key_Left:      return { Action::Navigate, QStringLiteral("left") };
    case Qt::Key_Right:     return { Action::Navigate, QStringLiteral("right") };
    case Qt::Key_Up:        return { Action::Navigate, QStringLiteral("up") };
    case Qt::Key_Down:      return { Action::Navigate, QStringLiteral("down") };

    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
    case Qt::Key_Select:    return { Action::Accept, {} };

    case Qt::Key_Escape:
    case Qt::Key_Backspace:
    case Qt::Key_Back:      return { Action::Back, {} };

    case Qt::Key_Insert:
    case Qt::Key_Favorites: return { Action::Favorite, {} };

    case Qt::Key_Slash:
    case Qt::Key_Search:    return { Action::Search, {} };

    case Qt::Key_Menu:
    case Qt::Key_Tab:       return { Action::Options, {} };

    case Qt::Key_F12:       return { Action::QuickPanel, {} };

    case Qt::Key_PageUp:    return { Action::PrevCategory, {} };
    case Qt::Key_PageDown:  return { Action::NextCategory, {} };

    case Qt::Key_Home:      return { Action::PageUp, {} };
    case Qt::Key_End:       return { Action::PageDown, {} };

    case Qt::Key_HomePage:  return { Action::Guide, {} };
    }
    return {};
}

QString InputRouter::nameOf(Action action)
{
    return QString::fromLatin1(QMetaEnum::fromType<Action>().valueToKey(static_cast<int>(action)));
}

void InputRouter::postAction(Action action, const QString &arg, bool isRepeat, Source source)
{
    // Capturing `this` is safe: the invocation is posted to this object and
    // is discarded if the router is destroyed before it runs. QString copies
    // are thread-safe (atomic reference count).
    QMetaObject::invokeMethod(this, [this, action, arg, isRepeat, source] {
        deliver(action, arg, isRepeat, source);
    }, Qt::QueuedConnection);
}

void InputRouter::postAction(Action action, const QString &arg, bool isRepeat, Source source,
                             std::shared_ptr<std::atomic<int>> pending)
{
    QMetaObject::invokeMethod(this, [this, action, arg, isRepeat, source, pending = std::move(pending)] {
        deliver(action, arg, isRepeat, source);
        if (pending)
            pending->fetch_sub(1, std::memory_order_acq_rel);
    }, Qt::QueuedConnection);
}

void InputRouter::postGuideHeld(Source source)
{
    QMetaObject::invokeMethod(this, [this, source] {
        if (source == Source::Remote && m_remoteMode == RemoteMode::RemoteOff)
            return;
        if (!m_secureEntry)
            qCInfo(lcInput) << "Guide held while an app is running, source" << source;
        setLastSource(source);
        emit guideHeld(source);
    }, Qt::QueuedConnection);
}

void InputRouter::postGamepadStyle(PadStyle style)
{
    QMetaObject::invokeMethod(this, [this, style] { setGamepadStyle(style); }, Qt::QueuedConnection);
}

void InputRouter::postDeviceNotice(bool connected, quint32 id, const QString &rawName, Source source,
                                   bool initial)
{
    QString name = TextSanitize::label(rawName);
    if (name.isEmpty())
        name = source == Source::Remote ? QStringLiteral("TV remote") : QStringLiteral("Gamepad");
    QMetaObject::invokeMethod(this, [this, connected, id, name, source, initial] {
        const auto it = std::find_if(m_devices.begin(), m_devices.end(),
                                     [&](const Device &d) { return d.id == id && d.source == source; });
        if (connected && it == m_devices.end())
            m_devices.append({ id, name, source });
        else if (!connected && it != m_devices.end())
            m_devices.erase(it);
        else
            return;   // duplicate notice
        emit controllersChanged();
        emit deviceChanged(connected, name, source, initial);
    }, Qt::QueuedConnection);
}

void InputRouter::setGamepadStyle(PadStyle style)
{
    if (m_gamepadStyle == style)
        return;
    m_gamepadStyle = style;
    emit gamepadStyleChanged();
}

QStringList InputRouter::controllers() const
{
    QStringList names;
    for (const Device &d : m_devices)
        names.append(d.name);
    return names;
}

void InputRouter::setRumbleEnabled(bool enabled)
{
    if (m_rumbleEnabled == enabled)
        return;
    m_rumbleEnabled = enabled;
    emit rumbleEnabledChanged();
}

void InputRouter::setRemoteMode(RemoteMode mode)
{
    if (m_remoteMode == mode)
        return;
    m_remoteMode = mode;
    emit remoteModeChanged();
}

void InputRouter::setAcceptRemote(bool accept)
{
    if (m_acceptRemote == accept)
        return;
    m_acceptRemote = accept;
    emit acceptRemoteChanged();
}

void InputRouter::setSecureEntry(bool secure)
{
    if (m_secureEntry == secure)
        return;
    m_secureEntry = secure;
    emit secureEntryChanged();
}

bool InputRouter::remoteAllows(Action action, RemoteMode mode, bool restricted)
{
    if (mode == RemoteMode::RemoteOff)
        return false;
    if (restricted)
        return action == Action::Back || action == Action::Guide;
    if (mode == RemoteMode::RemoteFull)
        return true;
    switch (action) {
    case Action::Navigate:
    case Action::Accept:
    case Action::Back:
    case Action::PageUp:
    case Action::PageDown:
    case Action::PrevCategory:
    case Action::NextCategory:
    case Action::Guide:
        return true;
    default:
        return false;
    }
}

bool InputRouter::rumble(Feedback kind)
{
    if (!m_rumbleEnabled || m_blocked || m_lastSource != Source::Gamepad)
        return false;
    if (kind == Feedback::Bump) {
        if (m_bumpRumbled)
            return false;   // already buzzed for this held direction
        m_bumpRumbled = true;
    }
    emit rumbleRequested(kind);
    return true;
}

bool InputRouter::dispatch(Action action, const QString &arg, Source source)
{
    Q_ASSERT(QThread::currentThread() == thread());
    return deliver(action, arg, false, source);
}

bool InputRouter::deliver(Action action, const QString &arg, bool isRepeat, Source source)
{
    if (action == Action::None)
        return false;
    if (action == Action::Navigate && !isDirection(arg))
        return false;

    setLastSource(source);
    if (source != Source::Mouse)
        setCursorHidden(true);

    const bool trusted = source != Source::Remote
                      || remoteAllows(action, m_remoteMode, m_secureEntry || !m_acceptRemote);
    const bool deliverIt = trusted && isAccepting() && !(isRepeat && !isRepeatable(action));
    if (!m_secureEntry) {
        // Secure entry: nothing about actions leaves the router except
        // triggered() to the entry itself.
        qCDebug(lcInput) << action << arg << source << (isRepeat ? "repeat" : "") << (deliverIt ? "" : "dropped");
        emit received(action, arg, isRepeat, source, deliverIt);
    }
    if (!deliverIt)
        return false;
    if (!isRepeat)
        m_bumpRumbled = false;   // a fresh press may bump (and buzz) again

    emit triggered(action, arg);
    return true;
}

void InputRouter::muteFor(int ms)
{
    const bool wasMuted = isMuted();
    const bool wasAccepting = isAccepting();
    if (ms <= 0)
        m_muteTimer.stop();
    else
        m_muteTimer.start(ms);
    if (wasMuted != isMuted())
        emit mutedChanged();
    if (wasAccepting != isAccepting())
        emit acceptingChanged();
}

bool InputRouter::pointerMoved(QPointF scenePos)
{
    const QPointF d = scenePos - m_lastPointer;
    const bool first = m_lastPointer.x() < 0;
    if (!first && d.x() * d.x() + d.y() * d.y() < kPointerSlop2)
        return false;
    m_lastPointer = scenePos;
    return !first;
}

bool InputRouter::pointerAction()
{
    setLastSource(Source::Mouse);
    return isAccepting();
}

bool InputRouter::handleKeyEvent(QKeyEvent *event)
{
    const KeyBinding binding = keyActionFor(event->key(), event->modifiers());
    if (binding.action == Action::None)
        return false;
    deliver(binding.action, binding.arg, event->isAutoRepeat(), Source::Keyboard);
    return true;
}

bool InputRouter::focusWantsText() const
{
    QObject *focus = QGuiApplication::focusObject();
    if (!focus)
        return false;
    QInputMethodQueryEvent query(Qt::ImEnabled);
    QCoreApplication::sendEvent(focus, &query);
    return query.value(Qt::ImEnabled).toBool();
}

bool InputRouter::eventFilter(QObject *watched, QEvent *event)
{
    const bool ours = watched && (watched == m_window.data()
        || std::any_of(m_extraWindows.cbegin(), m_extraWindows.cend(),
                       [watched](const QPointer<QWindow> &w) { return w.data() == watched; }));
    if (!ours)
        return QObject::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto *key = static_cast<QKeyEvent *>(event);
        // A focused text field (search, on-screen keyboard fallback) gets
        // its keys; only Esc still means Back.
        if (key->key() != Qt::Key_Escape && focusWantsText())
            return false;
        if (event->type() == QEvent::KeyRelease)
            return keyActionFor(key->key(), key->modifiers()).action != Action::None;
        return handleKeyEvent(key);
    }
    case QEvent::MouseMove:
        pointerActivity(static_cast<QMouseEvent *>(event)->position());
        return false;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        pointerActivity(mouse->position());
        setLastSource(Source::Mouse);
        if (mouse->button() == Qt::BackButton) {
            if (event->type() == QEvent::MouseButtonPress)
                deliver(Action::Back, {}, false, Source::Mouse);
            return true;
        }
        return mouse->button() == Qt::ForwardButton;   // reserved, swallow
    }
    case QEvent::Wheel:
        setCursorHidden(false);
        m_cursorHideTimer.start();
        setLastSource(Source::Mouse);
        return false;
    default:
        return false;
    }
}

void InputRouter::pointerActivity(QPointF windowPos)
{
    if (m_cursorHidden) {
        // The move that reveals the cursor must not also steal focus via
        // hover: pretend the pointer has been here all along.
        m_lastPointer = windowPos;
        setCursorHidden(false);
    }
    setLastSource(Source::Mouse);
    m_cursorHideTimer.start();
}

void InputRouter::setLastSource(Source source)
{
    if (m_lastSource == source)
        return;
    m_lastSource = source;
    emit lastSourceChanged();
}

void InputRouter::setCursorHidden(bool hidden)
{
    if (hidden)
        m_cursorHideTimer.stop();
    if (m_cursorHidden == hidden)
        return;
    m_cursorHidden = hidden;
    // An application override cursor beats Qt Quick's per-item cursor
    // updates, which would otherwise reset a window cursor on hover.
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        if (hidden && !m_overrideCursorSet) {
            QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
            m_overrideCursorSet = true;
        } else if (!hidden && m_overrideCursorSet) {
            QGuiApplication::restoreOverrideCursor();
            m_overrideCursorSet = false;
        }
    }
    emit cursorHiddenChanged();
}
