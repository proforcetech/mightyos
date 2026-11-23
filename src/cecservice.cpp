#include "cecservice.h"

#include "textsanitize.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include <chrono>
#include <cstring>
#include <utility>

#include <cerrno>
#include <fcntl.h>
#include <linux/cec.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcCec, "mightyos.cec", QtInfoMsg)

namespace {

constexpr int kMaxLine = 64;
const QByteArray kOsdName = QByteArrayLiteral("MightyOS");

qint64 steadyMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void wakeFd(int fd)
{
    if (fd < 0)
        return;
    const uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(fd, &one, sizeof one);
}

int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ::ioctl(fd, request, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

} // namespace

// --- KernelCecBackend --------------------------------------------------------------

KernelCecBackend::KernelCecBackend(const QString &devicePath)
    : m_path(devicePath)
    , m_name(QStringLiteral("TV remote (%1)").arg(QFileInfo(devicePath).fileName()))
{
}

KernelCecBackend::~KernelCecBackend()
{
    stop();
}

QStringList KernelCecBackend::devices()
{
    QStringList result;
    const QStringList names = QDir(QStringLiteral("/dev")).entryList({ QStringLiteral("cec*") },
                                                                     QDir::System | QDir::Files);
    for (const QString &name : names) {
        bool ok = false;
        name.mid(3).toInt(&ok);
        if (ok)
            result.append(QStringLiteral("/dev/") + name);
    }
    result.sort();
    return result;
}

bool KernelCecBackend::start(Sink sink, Lost lost)
{
    if (m_thread.joinable())
        return false;
    const QByteArray path = QFile::encodeName(m_path);
    m_fd = ::open(path.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        if (errno == EACCES || errno == EPERM)
            qCWarning(lcCec).noquote() << "no permission to open" << m_path
                                       << "- the TV remote needs the seat-local udev rule (PLAN D16, see README)";
        else
            qCWarning(lcCec).noquote() << "cannot open" << m_path << ":" << qt_error_string(errno);
        return false;
    }
    if (!configure()) {
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    m_sink = std::move(sink);
    m_lost = std::move(lost);
    m_wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeFd < 0) {
        qCWarning(lcCec) << "eventfd failed:" << qt_error_string(errno);
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    m_quit = false;
    m_thread = std::thread([this] { run(); });
    return true;
}

bool KernelCecBackend::claim(Claim type)
{
    cec_log_addrs want {};
    want.cec_version = CEC_OP_CEC_VERSION_1_4;
    want.vendor_id = CEC_VENDOR_ID_NONE;
    want.num_log_addrs = 1;
    if (type == Claim::Playback) {
        want.log_addr_type[0] = CEC_LOG_ADDR_TYPE_PLAYBACK;
        want.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_PLAYBACK;
        want.all_device_types[0] = CEC_OP_ALL_DEVTYPE_PLAYBACK;
    } else {
        want.log_addr_type[0] = CEC_LOG_ADDR_TYPE_RECORD;
        want.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_RECORD;
        want.all_device_types[0] = CEC_OP_ALL_DEVTYPE_RECORD;
    }
    std::memcpy(want.osd_name, kOsdName.constData(), size_t(kOsdName.size()));
    // flags = 0: no CEC_LOG_ADDRS_FL_ALLOW_RC_PASSTHRU (keys reach us, not
    // the kernel's rc input device as well).
    if (xioctl(m_fd, CEC_ADAP_S_LOG_ADDRS, &want) < 0)
        return false;
    // Non-blocking fd: the claim finishes in the background and ends with
    // CEC_EVENT_STATE_CHANGE (see refreshState()).
    m_claim = type;
    return true;
}

bool KernelCecBackend::configure()
{
    cec_caps caps {};
    if (xioctl(m_fd, CEC_ADAP_G_CAPS, &caps) < 0) {
        qCWarning(lcCec).noquote() << m_path << "is not a CEC adapter:" << qt_error_string(errno);
        return false;
    }
    qCInfo(lcCec).noquote() << "CEC adapter" << m_path << "driver"
                            << TextSanitize::label(QString::fromLatin1(caps.driver))
                            << "name" << TextSanitize::label(QString::fromLatin1(caps.name));

    // Initiator (for replies) + non-exclusive follower (to receive).
    __u32 mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
    if (xioctl(m_fd, CEC_S_MODE, &mode) < 0) {
        qCWarning(lcCec).noquote() << "cannot become a CEC follower on" << m_path << ":"
                                   << qt_error_string(errno) << "(another program owns it exclusively?)";
        return false;
    }

    cec_log_addrs addrs {};
    if (xioctl(m_fd, CEC_ADAP_G_LOG_ADDRS, &addrs) < 0) {
        qCWarning(lcCec).noquote() << "cannot read logical addresses:" << qt_error_string(errno);
        return false;
    }
    if (addrs.num_log_addrs > 0) {
        qCInfo(lcCec) << "using the logical address(es) already configured";
        if (addrs.flags & CEC_LOG_ADDRS_FL_ALLOW_RC_PASSTHRU) {
            // The kernel already turns the keys into input events (they
            // reach MightyOS as keyboard keys): don't process them twice.
            qCWarning(lcCec) << "the configured CEC address passes remote keys through as input events;"
                                " ignoring CEC key messages to avoid double input";
            m_keysEnabled = false;
        }
    } else if (!(caps.capabilities & CEC_CAP_LOG_ADDRS)) {
        qCWarning(lcCec) << "adapter has no logical address and doesn't let us set one";
        return false;
    } else if (!claim(Claim::Playback) && !claim(Claim::Record)) {
        qCWarning(lcCec).noquote() << "cannot claim a logical address:" << qt_error_string(errno);
        return false;
    }
    __u16 physical = CEC_PHYS_ADDR_INVALID;
    if (xioctl(m_fd, CEC_ADAP_G_PHYS_ADDR, &physical) == 0)
        m_follower.physicalAddress = physical;
    refreshState();
    return true;
}

void KernelCecBackend::refreshState()
{
    cec_log_addrs addrs {};
    if (xioctl(m_fd, CEC_ADAP_G_LOG_ADDRS, &addrs) < 0)
        return;
    const quint8 address = addrs.num_log_addrs > 0 ? addrs.log_addr[0] : CEC_LOG_ADDR_INVALID;
    if (address == CEC_LOG_ADDR_INVALID) {
        m_follower.address = Cec::kUnregistered;   // still claiming, or unconfigured
        return;
    }
    if (address == CEC_LOG_ADDR_UNREGISTERED && m_claim == Claim::Playback) {
        // All Playback addresses (4, 8, 11) are taken: clear and try Record.
        cec_log_addrs none {};
        xioctl(m_fd, CEC_ADAP_S_LOG_ADDRS, &none);
        qCInfo(lcCec) << "no free Playback address; claiming a Recording address";
        if (!claim(Claim::Record)) {
            m_claim = Claim::None;
            qCWarning(lcCec) << "cannot claim a Recording address:" << qt_error_string(errno);
        }
        m_follower.address = Cec::kUnregistered;
        return;
    }
    if (address == CEC_LOG_ADDR_UNREGISTERED) {
        qCWarning(lcCec) << "registered as Unregistered (15): the TV may not send remote keys";
        m_claim = Claim::None;
    } else if (m_claim != Claim::None) {
        m_claim = Claim::None;
    }
    m_follower.address = address;
}

void KernelCecBackend::stop()
{
    if (m_thread.joinable()) {
        m_quit = true;
        wakeFd(m_wakeFd);
        m_thread.join();
    }
    if (m_wakeFd >= 0)
        ::close(m_wakeFd);
    m_wakeFd = -1;
    if (m_fd >= 0)
        ::close(m_fd);
    m_fd = -1;
}

void KernelCecBackend::run()
{
    while (!m_quit) {
        pollfd fds[2] = { { m_fd, POLLIN | POLLPRI, 0 }, { m_wakeFd, POLLIN, 0 } };
        const int ready = ::poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            qCWarning(lcCec) << "poll failed:" << qt_error_string(errno);
            break;
        }
        if (fds[1].revents)
            return;   // stop()
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            qCWarning(lcCec).noquote() << m_path << "went away";
            break;
        }
        if (fds[0].revents & POLLPRI)
            dequeueEvents();
        if (fds[0].revents & POLLIN)
            receiveAll();
    }
    if (!m_quit && m_lost)
        m_lost();
}

void KernelCecBackend::dequeueEvents()
{
    cec_event event {};
    while (xioctl(m_fd, CEC_DQEVENT, &event) == 0) {
        if (event.event == CEC_EVENT_STATE_CHANGE) {
            m_follower.physicalAddress = event.state_change.phys_addr;
            if (event.state_change.phys_addr == CEC_PHYS_ADDR_INVALID)
                m_follower.activeSource = false;   // unplugged from the TV
            refreshState();
            qCInfo(lcCec).noquote() << "CEC state: physical address"
                                    << QStringLiteral("%1").arg(event.state_change.phys_addr, 4, 16, QLatin1Char('0'))
                                    << "logical address" << m_follower.address;
        } else if (event.event == CEC_EVENT_LOST_MSGS) {
            if (m_warnings.take(steadyMs())) {
                qCWarning(lcCec) << "CEC messages lost:" << event.lost_msgs.lost_msgs
                                 << "(+" << m_suppressedWarnings << "similar warnings suppressed)";
                m_suppressedWarnings = 0;
            } else {
                ++m_suppressedWarnings;
            }
        }
    }
}

void KernelCecBackend::transmit(const std::vector<quint8> &bytes)
{
    if (bytes.empty() || bytes.size() > CEC_MAX_MSG_SIZE)
        return;
    if (!m_replies.take(steadyMs()))
        return;   // a chatty (or hostile) bus: at most 5 replies per second
    cec_msg out {};
    out.len = __u32(bytes.size());
    std::memcpy(out.msg, bytes.data(), bytes.size());
    out.timeout = 0;
    if (xioctl(m_fd, CEC_TRANSMIT, &out) < 0) {
        if (m_warnings.take(steadyMs())) {
            qCWarning(lcCec) << "CEC reply failed:" << qt_error_string(errno)
                             << "(+" << m_suppressedWarnings << "similar warnings suppressed)";
            m_suppressedWarnings = 0;
        } else {
            ++m_suppressedWarnings;
        }
    }
}

void KernelCecBackend::receiveAll()
{
    for (;;) {
        cec_msg msg {};
        msg.timeout = 0;
        if (xioctl(m_fd, CEC_RECEIVE, &msg) < 0)
            return;   // EAGAIN: queue empty
        if (msg.tx_status != 0 || !(msg.rx_status & CEC_RX_STATUS_OK) || msg.len < 2
            || msg.len > CEC_MAX_MSG_SIZE)
            continue;   // our own transmit results, errors
        if (const auto key = Cec::parseKeyMessage(msg.msg, int(msg.len))) {
            if (m_keysEnabled && m_sink)
                m_sink(*key);
            continue;
        }
        transmit(Cec::followerReply(msg.msg, int(msg.len), m_follower, kOsdName));
    }
}

// --- FakeCecBackend ----------------------------------------------------------------

FakeCecBackend::FakeCecBackend(int fd, bool ownsFd, const QString &name)
    : m_fd(fd)
    , m_ownsFd(ownsFd)
    , m_name(name)
{
}

FakeCecBackend::~FakeCecBackend()
{
    stop();
    if (m_ownsFd && m_fd >= 0)
        ::close(m_fd);
}

std::unique_ptr<FakeCecBackend> FakeCecBackend::fromStdin()
{
    return std::make_unique<FakeCecBackend>(STDIN_FILENO, false, QStringLiteral("Fake CEC remote (stdin)"));
}

QString FakeCecBackend::defaultFifoPath()
{
    const QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return dir.isEmpty() || !QDir::isAbsolutePath(dir) ? QString() : dir + QStringLiteral("/mightyos-fake-cec");
}

std::unique_ptr<FakeCecBackend> FakeCecBackend::fromFifo(const QString &path, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return nullptr;
    };
    if (path.isEmpty())
        return fail(QStringLiteral("XDG_RUNTIME_DIR is not set"));
    const QFileInfo info(path);
    const QByteArray dirName = QFile::encodeName(info.absolutePath());
    const QByteArray fileName = QFile::encodeName(info.fileName());
    if (fileName.isEmpty())
        return fail(QStringLiteral("no FIFO name in %1").arg(path));

    const int dir = ::open(dirName.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0)
        return fail(QStringLiteral("cannot open %1: %2").arg(info.absolutePath(), qt_error_string(errno)));
    struct stat dst {};
    if (::fstat(dir, &dst) < 0 || dst.st_uid != ::getuid() || (dst.st_mode & 077)) {
        ::close(dir);
        return fail(QStringLiteral("%1 must be your own directory with mode 0700").arg(info.absolutePath()));
    }
    if (::mkfifoat(dir, fileName.constData(), 0600) < 0 && errno != EEXIST) {
        const int code = errno;
        ::close(dir);
        return fail(QStringLiteral("cannot create %1: %2").arg(path, qt_error_string(code)));
    }
    // O_RDWR: our own writer end too, so the FIFO never reports EOF when an
    // `echo up > fifo` closes its end.
    const int fd = ::openat(dir, fileName.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    const int openError = errno;
    ::close(dir);
    if (fd < 0)
        return fail(QStringLiteral("cannot open %1: %2").arg(path, qt_error_string(openError)));
    struct stat st {};
    if (::fstat(fd, &st) < 0 || !S_ISFIFO(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 077)) {
        ::close(fd);
        return fail(QStringLiteral("%1 must be a FIFO owned by you with mode 0600").arg(path));
    }
    return std::make_unique<FakeCecBackend>(fd, true, QStringLiteral("Fake CEC remote (FIFO)"));
}

bool FakeCecBackend::start(Sink sink, Lost)
{
    if (m_thread.joinable() || m_fd < 0)
        return false;
    m_sink = std::move(sink);
    m_wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeFd < 0)
        return false;
    m_quit = false;
    m_thread = std::thread([this] { run(); });
    return true;
}

void FakeCecBackend::stop()
{
    if (m_thread.joinable()) {
        m_quit = true;
        wakeFd(m_wakeFd);
        m_thread.join();
    }
    if (m_wakeFd >= 0)
        ::close(m_wakeFd);
    m_wakeFd = -1;
}

void FakeCecBackend::run()
{
    QByteArray buffer;
    bool open = true;
    while (!m_quit) {
        pollfd fds[2] = { { m_wakeFd, POLLIN, 0 }, { open ? m_fd : -1, POLLIN, 0 } };
        const int ready = ::poll(fds, 2, m_held ? kRefreshMs : -1);
        if (ready < 0 && errno != EINTR)
            return;
        if (fds[0].revents)
            break;
        if (ready == 0 && m_held) {
            m_sink({ true, *m_held });   // a held TV key repeats its press
            continue;
        }
        if (!(fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        char chunk[256];
        const ssize_t n = ::read(m_fd, chunk, sizeof chunk);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            open = false;   // stdin closed; keep running until stopped
            continue;
        }
        if (n < 0)
            continue;
        buffer.append(chunk, n);
        qsizetype newline;
        while ((newline = buffer.indexOf('\n')) >= 0) {
            handleLine(buffer.left(newline));
            buffer.remove(0, newline + 1);
        }
        if (buffer.size() > kMaxLine)
            buffer.clear();   // overlong line: drop it
    }
}

void FakeCecBackend::handleLine(const QByteArray &raw)
{
    QByteArray line = raw.trimmed();
    if (line.isEmpty() || line.startsWith('#') || line.size() > kMaxLine)
        return;
    char op = 0;
    if (line.startsWith('+') || line.startsWith('-')) {
        op = line.at(0);
        line.remove(0, 1);
    }
    const auto code = Cec::codeForName(QString::fromLatin1(line));
    if (!code) {
        qCWarning(lcCec).noquote() << "fake CEC: unknown key"
                                   << TextSanitize::escapeForTerminal(QString::fromLatin1(line));
        return;
    }
    if (op == '+') {
        m_held = *code;
        m_sink({ true, *code });
    } else if (op == '-') {
        m_held.reset();
        m_sink({ false, 0 });
    } else {
        m_held.reset();
        m_sink({ true, *code });
        m_sink({ false, 0 });
    }
}

bool LibCecBackend::start(Sink, Lost)
{
    qCInfo(lcCec) << "libcec support is not built (M8, license and packaging check pending)";
    return false;
}

// --- CecService --------------------------------------------------------------------

CecService::CecService(InputRouter *router, std::unique_ptr<CecBackend> backend, QObject *parent)
    : QObject(parent)
    , m_router(router)
    , m_backend(std::move(backend))
    , m_gate(router, InputRouter::Source::Remote)
    , m_name(m_backend ? m_backend->name() : QString())
{
}

CecService::~CecService()
{
    stop();
}

std::unique_ptr<CecBackend> CecService::detectKernelBackend()
{
    const QStringList devices = KernelCecBackend::devices();
    if (devices.isEmpty())
        return nullptr;
    return std::make_unique<KernelCecBackend>(devices.constFirst());
}

QString CecService::backendName() const
{
    return m_name;
}

qint64 CecService::now() const
{
    return m_testClock ? m_testClock() : steadyMs();
}

bool CecService::start()
{
    if (!m_backend || m_worker.joinable())
        return false;
    {
        std::lock_guard lock(m_mutex);
        m_quit = false;
        m_queue.clear();
    }
    m_worker = std::thread([this] { run(); });
    if (!m_backend->start([this](const Cec::KeyEvent &event) { enqueue(event); },
                          [this] { lost(); })) {
        stop();
        return false;
    }
    m_router->postDeviceNotice(true, 0, m_name, InputRouter::Source::Remote, true);
    m_announced = true;
    return true;
}

void CecService::stop()
{
    if (m_backend)
        m_backend->stop();   // no more enqueue() / lost() after this
    if (!m_worker.joinable())
        return;
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    m_worker.join();
    if (m_announced.exchange(false))
        m_router->postDeviceNotice(false, 0, m_name, InputRouter::Source::Remote, true);
}

void CecService::lost()
{
    // Backend thread: the adapter went away (unplugged USB-CEC adapter).
    if (m_announced.exchange(false))
        m_router->postDeviceNotice(false, 0, m_name, InputRouter::Source::Remote, false);
}

void CecService::stopOnAboutToQuit()
{
    if (auto *app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, &CecService::stop, Qt::DirectConnection);
}

void CecService::setAppRunning(bool running)
{
    if (m_gate.isAppRunning() == running)
        return;
    m_gate.setAppRunning(running);
    {
        std::lock_guard lock(m_mutex);
        m_latch = true;
    }
    m_cv.notify_all();
}

void CecService::pokeForTesting()
{
    {
        std::lock_guard lock(m_mutex);
        m_poke = true;
    }
    m_cv.notify_all();
}

void CecService::enqueue(const Cec::KeyEvent &event)
{
    {
        std::lock_guard lock(m_mutex);
        // ~10 presses per second (TV refreshes included); releases always.
        if (event.pressed && !m_presses.take(now())) {
            ++m_dropped;
            return;
        }
        if (m_queue.size() >= 64)
            return;
        m_queue.push_back(event);
    }
    m_cv.notify_all();
}

void CecService::run()
{
    Cec::RemoteState state;
    std::vector<Cec::KeyEvent> batch;   // reused: no allocation per wake
    PadInput::Events out;
    std::unique_lock lock(m_mutex);
    while (!m_quit) {
        const auto wakeUp = [this] { return m_quit || m_latch || m_poke || !m_queue.empty(); };
        const auto deadline = state.nextDeadline();
        if (m_testClock || !deadline) {
            m_cv.wait(lock, wakeUp);
        } else {
            const auto wait = std::chrono::milliseconds(std::max<qint64>(0, *deadline - now()));
            m_cv.wait_for(lock, wait, wakeUp);
        }
        if (m_quit)
            break;
        batch.swap(m_queue);
        const bool latch = std::exchange(m_latch, false);
        m_poke = false;
        lock.unlock();

        out.clear();
        if (latch)
            state.latchHeld();
        for (const Cec::KeyEvent &event : batch) {
            if (event.pressed)
                state.press(event.code, now(), out);
            else
                state.release(now());
        }
        batch.clear();
        state.tick(now(), out);
        m_gate.handle(out);
        ++m_rounds;

        lock.lock();
    }
}
