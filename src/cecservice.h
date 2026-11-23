#pragma once

#include "cecinput.h"
#include "inputrouter.h"
#include "padinput.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// HDMI-CEC TV remote (PLAN D14, D16-D18). A backend delivers key
// presses/releases on its own thread; CecService turns them into actions on
// a worker thread (Cec::RemoteState) and posts them through InputGate with
// Source::Remote: the same coalescing and "only Guide while an app runs"
// rules as the gamepad. InputRouter applies the remote's trust level
// (Settings.tvRemote, secure screens).
//
// Backends:
//  - KernelCecBackend: the Linux CEC framework (/dev/cecN, <linux/cec.h>).
//    Claims a Playback logical address if none is configured; if that ends
//    as Unregistered (all Playback addresses taken) it clears it and claims
//    a Recording address instead. Follower mode (non-exclusive). Never sets
//    CEC_LOG_ADDRS_FL_ALLOW_RC_PASSTHRU; if a pre-configured address has
//    it, the kernel already turns keys into input events, so the backend
//    ignores keys (no double input) and logs a warning. Follower replies:
//    Cec::followerReply, at most 5 per second.
//    Note: after a claim the kernel itself broadcasts <Report Physical
//    Address> (and answers <Give Physical Address>, <Get CEC Version>,
//    <Give Device Vendor ID>). MightyOS sends nothing else unprompted: no
//    <Active Source> unless the TV routed to us (D17), no TV power-on.
//  - FakeCecBackend: key names from stdin or a FIFO, for tests and
//    development (--fake-cec, only with MIGHTYOS_DEV_TOOLS).
//  - LibCecBackend: placeholder (libcec is not used).
//
// No /dev/cec* -> no backend, CecService is never created: silent, inactive.
// Presses are rate-limited to ~10 per second.
//
// LIFETIME RULE: like GamepadService, stop() must run before the
// InputRouter is destroyed (stopOnAboutToQuit() in main; the destructor
// too). stop() posts a "disconnected" notice.
class CecBackend
{
public:
    using Sink = std::function<void(const Cec::KeyEvent &)>;
    using Lost = std::function<void()>;
    virtual ~CecBackend() = default;
    virtual QString name() const = 0;
    // Starts delivering to `sink` from the backend's own thread; `lost` is
    // called (from that thread) if the device goes away. False if the
    // device can't be used (the reason is logged).
    virtual bool start(Sink sink, Lost lost) = 0;
    // Wakes and joins the backend thread. Idempotent.
    virtual void stop() = 0;
};

class KernelCecBackend : public CecBackend
{
public:
    explicit KernelCecBackend(const QString &devicePath);
    ~KernelCecBackend() override;

    // /dev/cec0, /dev/cec1, ... that exist.
    static QStringList devices();

    QString name() const override { return m_name; }
    bool start(Sink sink, Lost lost) override;
    void stop() override;

private:
    enum class Claim { None, Playback, Record };
    void run();
    bool configure();
    bool claim(Claim type);
    void receiveAll();
    void dequeueEvents();
    void refreshState();
    void transmit(const std::vector<quint8> &bytes);

    QString m_path;
    QString m_name;
    Sink m_sink;
    Lost m_lost;
    int m_fd = -1;
    int m_wakeFd = -1;
    Claim m_claim = Claim::None;
    bool m_keysEnabled = true;
    Cec::FollowerState m_follower;
    PadInput::TokenBucket m_replies { 5, 5 };
    PadInput::TokenBucket m_warnings { 0.1, 2 };
    int m_suppressedWarnings = 0;
    std::thread m_thread;
    std::atomic<bool> m_quit { false };
};

// Reads one command per line:
//   up          tap (press + release)
//   +up / -up   press / release (while pressed it repeats the press every
//               400 ms, like a TV, until released)
// Names: see Cec::codeForName. Lines are capped at 64 bytes; unknown names
// are logged and ignored.
class FakeCecBackend : public CecBackend
{
public:
    static constexpr int kRefreshMs = 400;

    // Reads `fd` (not closed unless `ownsFd`).
    FakeCecBackend(int fd, bool ownsFd, const QString &name);
    ~FakeCecBackend() override;

    // stdin.
    static std::unique_ptr<FakeCecBackend> fromStdin();
    // $XDG_RUNTIME_DIR/mightyos-fake-cec.
    static QString defaultFifoPath();
    // The directory must be ours with mode 0700; the FIFO is created there
    // (0600) if missing and must then be a FIFO owned by us with no
    // group/other access. Opened once, O_RDWR, relative to the checked
    // directory (no reopen by path, no EOF when writers come and go).
    static std::unique_ptr<FakeCecBackend> fromFifo(const QString &path, QString *error);

    QString name() const override { return m_name; }
    bool start(Sink sink, Lost lost) override;
    void stop() override;

private:
    void run();
    void handleLine(const QByteArray &line);

    int m_fd;
    bool m_ownsFd;
    QString m_name;
    Sink m_sink;
    int m_wakeFd = -1;
    std::optional<quint8> m_held;
    std::thread m_thread;
    std::atomic<bool> m_quit { false };
};

// Placeholder for USB adapters driven by libcec (Pulse-Eight without the
// kernel's pulse8-cec + inputattach). libcec is not packaged on the test
// PC and its license (GPLv2+ with a commercial option) needs a check
// against D15 before it can be linked; start() always fails for now.
class LibCecBackend : public CecBackend
{
public:
    QString name() const override { return QStringLiteral("libcec (not built)"); }
    bool start(Sink, Lost) override;
    void stop() override {}
};

class CecService : public QObject
{
    Q_OBJECT

public:
    static constexpr double kPressesPerSecond = 10;

    CecService(InputRouter *router, std::unique_ptr<CecBackend> backend, QObject *parent = nullptr);
    ~CecService() override;

    // A KernelCecBackend for the first /dev/cecN, or nullptr (no device).
    static std::unique_ptr<CecBackend> detectKernelBackend();

    // False if the backend could not start (the service stays inactive).
    bool start();
    void stop();
    void stopOnAboutToQuit();
    bool isActive() const { return m_worker.joinable(); }
    QString backendName() const;

    // Any thread.
    void setAppRunning(bool running);

    // Tests: a fake clock (before start()). The worker then never waits on
    // real time; pokeForTesting() makes it run one round with the current
    // fake time, and rounds() counts finished rounds.
    void setClockForTesting(std::function<qint64()> clock) { m_testClock = std::move(clock); }
    void pokeForTesting();
    int rounds() const { return m_rounds.load(); }
    int droppedPresses() const { return m_dropped.load(); }

private:
    void enqueue(const Cec::KeyEvent &event);
    void lost();
    void run();
    qint64 now() const;

    InputRouter *m_router;
    std::unique_ptr<CecBackend> m_backend;
    InputGate m_gate;
    QString m_name;
    std::function<qint64()> m_testClock;

    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Cec::KeyEvent> m_queue;
    PadInput::TokenBucket m_presses { kPressesPerSecond, kPressesPerSecond };
    bool m_quit = false;
    bool m_latch = false;
    bool m_poke = false;
    std::atomic<bool> m_announced { false };
    std::atomic<int> m_rounds { 0 };
    std::atomic<int> m_dropped { 0 };
};
