#include "signalwatcher.h"

#include <QLoggingCategory>
#include <QSocketNotifier>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcSignals, "mightyos.session")

namespace {

// Written by the handler, so plain ints (async-signal-safe reads).
volatile sig_atomic_t g_writeFd = -1;
std::atomic<bool> g_inUse { false };
constexpr int kMaxSignal = 64;
struct sigaction g_previous[kMaxSignal + 1];

extern "C" void onSignal(int signo)
{
    const int saved = errno;
    const int fd = g_writeFd;
    if (fd >= 0) {
        const unsigned char byte = static_cast<unsigned char>(signo);
        // Non-blocking: a full pipe (a signal storm nobody reads) drops it.
        [[maybe_unused]] const ssize_t n = ::write(fd, &byte, 1);
    }
    errno = saved;
}

} // namespace

SignalWatcher::SignalWatcher(const QList<int> &signalNumbers, QObject *parent)
    : QObject(parent)
{
    if (g_inUse.exchange(true)) {
        qCWarning(lcSignals) << "only one SignalWatcher per process";
        return;
    }
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        qCWarning(lcSignals) << "signal pipe:" << std::strerror(errno);
        g_inUse = false;
        return;
    }
    g_writeFd = fds[1];
    m_notifier = new QSocketNotifier(fds[0], QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &SignalWatcher::drain);

    m_active = true;
    for (const int signo : signalNumbers) {
        if (signo <= 0 || signo > kMaxSignal || m_signals.contains(signo))
            continue;
        struct sigaction action {};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        if (::sigaction(signo, &action, &g_previous[signo]) != 0) {
            qCWarning(lcSignals) << "sigaction" << signo << std::strerror(errno);
            m_active = false;
            continue;
        }
        m_signals.append(signo);
    }
}

SignalWatcher::~SignalWatcher()
{
    for (const int signo : std::as_const(m_signals))
        ::sigaction(signo, &g_previous[signo], nullptr);
    if (m_notifier) {
        const int readFd = int(m_notifier->socket());
        delete m_notifier;
        m_notifier = nullptr;
        const int writeFd = g_writeFd;
        g_writeFd = -1;
        ::close(readFd);
        if (writeFd >= 0)
            ::close(writeFd);
        g_inUse = false;
    }
}

void SignalWatcher::drain()
{
    unsigned char buffer[64];
    for (;;) {
        const ssize_t n = ::read(int(m_notifier->socket()), buffer, sizeof buffer);
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; ++i) {
            const int signo = buffer[i];
            if (m_signals.contains(signo))
                emit received(signo);
        }
    }
}
