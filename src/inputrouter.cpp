#include "inputrouter.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QThread>
#include <QWindow>

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

InputRouter::KeyBinding InputRouter::keyActionFor(int key, Qt::KeyboardModifiers modifiers)
{
    // Keypad arrows/Enter carry KeypadModifier, which changes nothing.
    // Shift is only accepted for '/' (it needs Shift on e.g. German
    // layouts); Shift+F/M/Q/Space etc. stay unmapped.
    const Qt::KeyboardModifiers mods = modifiers & ~Qt::KeypadModifier;

    if (mods == Qt::ControlModifier && key == Qt::Key_F)
        return { Action::Search, {} };
    if (mods == Qt::ShiftModifier && key == Qt::Key_Slash)
        return { Action::Search, {} };
    if (mods != Qt::NoModifier)
        return {};   // leave Shift/Ctrl/Alt/Meta chords alone

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

    case Qt::Key_F:
    case Qt::Key_Favorites: return { Action::Favorite, {} };

    case Qt::Key_Slash:
    case Qt::Key_Search:    return { Action::Search, {} };

    case Qt::Key_M:
    case Qt::Key_Menu:      return { Action::Options, {} };

    case Qt::Key_Q:         return { Action::QuickPanel, {} };

    case Qt::Key_PageUp:    return { Action::PrevCategory, {} };
    case Qt::Key_PageDown:  return { Action::NextCategory, {} };

    case Qt::Key_Home:      return { Action::PageUp, {} };
    case Qt::Key_End:       return { Action::PageDown, {} };
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

    if (!isAccepting())
        return false;
    if (isRepeat && !isRepeatable(action))
        return false;

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
    if (watched != m_window)
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
