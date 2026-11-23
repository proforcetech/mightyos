#include "padhub.h"

#include <QFile>
#include <QLoggingCategory>

#include <algorithm>

#include <cerrno>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

Q_DECLARE_LOGGING_CATEGORY(lcGamepad)

namespace {

void drainFd(int fd)
{
    char buffer[1024];
    while (::read(fd, buffer, sizeof buffer) > 0) {
    }
}

} // namespace

PadHub::PadHub(InputRouter *router)
    : m_router(router)
    , m_gate(router, InputRouter::Source::Gamepad)
    , m_opener(&PadHub::openWakeFd)
{
}

PadHub::~PadHub()
{
    clear();
}

void PadHub::setControlFds(int eventFd, int inotifyFd)
{
    m_eventFd = eventFd;
    m_inotifyFd = inotifyFd;
}

bool PadHub::looksLikeKeyboard(const unsigned char *keyBits, size_t bytes)
{
    for (int key = KEY_Q; key <= KEY_M; ++key) {   // KEY_Q (16) .. KEY_M (50) spans A..Z
        const bool isLetter = (key >= KEY_Q && key <= KEY_P) || (key >= KEY_A && key <= KEY_L)
                           || (key >= KEY_Z && key <= KEY_M);
        if (!isLetter || size_t(key / 8) >= bytes)
            continue;
        if (keyBits[key / 8] & (1u << (key % 8)))
            return true;
    }
    return false;
}

int PadHub::openWakeFd(const QString &path, QString *why)
{
    const auto fail = [why](const QString &reason) {
        if (why)
            *why = reason;
        return -1;
    };
    const bool evdev = path.startsWith(QLatin1String("/dev/input/event"));
    const bool hidraw = path.startsWith(QLatin1String("/dev/hidraw"));
    if (!evdev && !hidraw)
        return fail(path.isEmpty() ? QStringLiteral("no device path") : QStringLiteral("not an evdev or hidraw node"));
    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return fail(qt_error_string(errno));
    if (evdev) {
        unsigned char keys[KEY_MAX / 8 + 1] = {};
        if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) {
            ::close(fd);
            return fail(QStringLiteral("not an evdev device"));
        }
        if (looksLikeKeyboard(keys, sizeof keys)) {
            ::close(fd);
            qCWarning(lcGamepad).noquote() << "refusing to watch" << path << "- it reports letter keys (keyboard-like)";
            return fail(QStringLiteral("keyboard-like device"));
        }
    }
    return fd;
}

PadHub::Pad &PadHub::add(quint32 id, const QString &name, const QString &path, InputRouter::PadStyle style,
                         bool rumble)
{
    remove(id);
    auto pad = std::make_unique<Pad>();
    pad->id = id;
    pad->name = name;
    pad->path = path;
    pad->style = style;
    pad->rumble = rumble;
    QString why;
    pad->fd = m_opener ? m_opener(path, &why) : -1;
    pad->hidraw = pad->fd >= 0 && path.startsWith(QLatin1String("/dev/hidraw"));
    if (pad->fd < 0)
        qCInfo(lcGamepad).noquote() << name << "- no wake-up fd (" << why
                                    << "): using SDL's 1 ms wait while it is connected";
    if (isAppRunning())
        pad->state.latchHeld();
    if (m_lastActive == 0)
        m_lastActive = id;   // rumble target until a pad is actually used
    m_pads.push_back(std::move(pad));
    return *m_pads.back();
}

void PadHub::closeFd(Pad &pad)
{
    if (pad.fd >= 0)
        ::close(pad.fd);
    pad.fd = -1;
}

bool PadHub::remove(quint32 id)
{
    const auto it = std::find_if(m_pads.begin(), m_pads.end(), [id](const auto &p) { return p->id == id; });
    if (it == m_pads.end())
        return false;
    closeFd(**it);
    m_pads.erase(it);
    if (m_lastActive == id)
        m_lastActive = m_pads.empty() ? 0 : m_pads.front()->id;
    return true;
}

void PadHub::clear()
{
    for (auto &pad : m_pads)
        closeFd(*pad);
    m_pads.clear();
    m_lastActive = 0;
    m_styleSent = false;
}

PadHub::Pad *PadHub::find(quint32 id)
{
    for (auto &pad : m_pads) {
        if (pad->id == id)
            return pad.get();
    }
    return nullptr;
}

void PadHub::deliver(Pad &pad, const PadInput::Events &events)
{
    if (events.empty())
        return;
    m_lastActive = pad.id;
    if (!m_styleSent || pad.style != m_sentStyle) {
        m_styleSent = true;
        m_sentStyle = pad.style;
        m_router->postGamepadStyle(pad.style);
    }
    m_gate.handle(events);
}

void PadHub::button(quint32 id, PadInput::Button button, bool down, qint64 now)
{
    if (Pad *pad = find(id)) {
        PadInput::Events out;
        pad->state.button(button, down, now, out);
        deliver(*pad, out);
    }
}

void PadHub::axis(quint32 id, PadInput::Axis axis, float value, qint64 now)
{
    if (Pad *pad = find(id)) {
        PadInput::Events out;
        pad->state.axis(axis, value, now, out);
        deliver(*pad, out);
    }
}

void PadHub::tick(qint64 now)
{
    for (auto &pad : m_pads) {
        PadInput::Events out;
        pad->state.tick(now, out);
        if (!out.empty())
            m_gate.handle(out);
    }
}

void PadHub::setAppRunning(bool running)
{
    if (m_gate.isAppRunning() == running)
        return;
    m_gate.setAppRunning(running);
    m_latchPending = true;
}

bool PadHub::applyPendingLatch()
{
    if (!m_latchPending.exchange(false))
        return false;
    for (auto &pad : m_pads)
        pad->state.latchHeld();
    return true;
}

std::optional<qint64> PadHub::nextDeadline() const
{
    std::optional<qint64> deadline;
    for (const auto &pad : m_pads) {
        if (const auto t = pad->state.nextDeadline())
            deadline = deadline ? std::min(*deadline, *t) : *t;
    }
    return deadline;
}

bool PadHub::canBlock() const
{
    return m_eventFd >= 0
        && std::all_of(m_pads.begin(), m_pads.end(), [](const auto &pad) { return pad->fd >= 0; });
}

bool PadHub::hasHidraw() const
{
    return std::any_of(m_pads.begin(), m_pads.end(), [](const auto &pad) { return pad->hidraw; });
}

PadHub::WaitResult PadHub::wait(int timeoutMs)
{
    WaitResult result;
    m_pollFds.clear();   // keeps its capacity: no allocation per wake
    m_pollFds.push_back({ m_eventFd, POLLIN, 0 });
    m_pollFds.push_back({ m_inotifyFd, POLLIN, 0 });   // poll() ignores negative fds
    constexpr size_t firstPad = 2;
    for (const auto &pad : m_pads)
        m_pollFds.push_back({ pad->fd, POLLIN, 0 });

    const int ready = ::poll(m_pollFds.data(), nfds_t(m_pollFds.size()), timeoutMs);
    if (ready <= 0)
        return result;
    if (m_pollFds[0].revents) {
        drainFd(m_eventFd);
        result.control = true;
    }
    if (m_pollFds[1].revents) {
        drainFd(m_inotifyFd);
        result.hotplug = true;
    }
    // m_pads is unchanged since the array was built: index i <-> pad i.
    for (size_t i = firstPad; i < m_pollFds.size(); ++i) {
        const short revents = m_pollFds[i].revents;
        if (!revents)
            continue;
        Pad &pad = *m_pads[i - firstPad];
        if (revents & POLLIN)
            drainFd(pad.fd);
        if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
            closeFd(pad);
            pad.lostFd = true;
            qCInfo(lcGamepad).noquote() << pad.name
                << "- wake-up fd hung up: using SDL's 1 ms wait until it is reopened or removed";
            result.hungUp.push_back(pad.id);
        } else {
            result.input.push_back(pad.id);
        }
    }
    return result;
}

int PadHub::reopenLost()
{
    int reopened = 0;
    for (auto &pad : m_pads) {
        if (!pad->lostFd || !m_opener)
            continue;
        QString why;
        pad->fd = m_opener(pad->path, &why);
        if (pad->fd >= 0) {
            pad->lostFd = false;
            ++reopened;
            qCInfo(lcGamepad).noquote() << pad->name << "- wake-up fd reopened";
        }
    }
    return reopened;
}

std::optional<quint32> PadHub::rumbleTarget() const
{
    if (isAppRunning())
        return std::nullopt;
    for (const auto &pad : m_pads) {
        if (pad->id == m_lastActive && pad->rumble)
            return pad->id;
    }
    return std::nullopt;
}
