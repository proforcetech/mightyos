#pragma once

#include "inputrouter.h"
#include "padinput.h"

#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <poll.h>

// The SDL-free core of GamepadService, used on its thread: the connected
// pads with their PadState, InputGate posting, the wake-up fds and the
// poll() that waits on them, app-running latching and the rumble target.
// Tests drive it directly, with pipes standing in for device nodes.
//
// Wake-up fds: for every pad MightyOS opens its own read-only, never
// grabbed fd on the pad's node (evdev /dev/input/eventN, or /dev/hidrawN
// for SDL's HIDAPI drivers), only to learn *when* to pump SDL. Data read
// from it is discarded. evdev nodes that look like keyboards (KEY_A..KEY_Z)
// are refused. A pad without such an fd (no path, no permission, unknown
// node type) makes canBlock() false: the service then uses SDL's own wait
// (1 ms polling) while that pad is connected. That fallback is logged.
class PadHub
{
public:
    struct Pad {
        quint32 id = 0;
        QString name;
        QString path;
        InputRouter::PadStyle style = InputRouter::PadStyle::Xbox;
        bool rumble = false;
        int fd = -1;            // wake-up fd or -1
        bool hidraw = false;    // fd is a hidraw node
        bool lostFd = false;    // fd hung up; reopened on the next hotplug settle
        PadInput::PadState state;
    };
    // Opens a wake-up fd for a device path; -1 and a reason if it can't.
    using Opener = std::function<int(const QString &path, QString *why)>;

    struct WaitResult {
        bool control = false;              // the eventfd (quit, rumble, latch)
        bool hotplug = false;              // inotify on /dev/input
        std::vector<quint32> input;        // pads whose fd was readable
        std::vector<quint32> hungUp;       // pads whose fd hung up (now closed)
    };

    explicit PadHub(InputRouter *router);
    ~PadHub();
    PadHub(const PadHub &) = delete;
    PadHub &operator=(const PadHub &) = delete;

    void setOpener(Opener opener) { m_opener = std::move(opener); }
    // Not owned. -1 = none (then canBlock() is false: no way to wake poll()).
    void setControlFds(int eventFd, int inotifyFd);

    Pad &add(quint32 id, const QString &name, const QString &path, InputRouter::PadStyle style, bool rumble);
    bool remove(quint32 id);
    void clear();
    Pad *find(quint32 id);
    const std::vector<std::unique_ptr<Pad>> &pads() const { return m_pads; }
    quint32 lastActive() const { return m_lastActive; }

    // Input from SDL (hub thread). The first pad producing an action becomes
    // the last active one (glyph style posted before its first action).
    void button(quint32 id, PadInput::Button button, bool down, qint64 now);
    void axis(quint32 id, PadInput::Axis axis, float value, qint64 now);
    void tick(qint64 now);

    // Any thread. Changing it requests a latch of whatever is held.
    void setAppRunning(bool running);
    bool isAppRunning() const { return m_gate.isAppRunning(); }
    // Hub thread: applies a requested latch. Returns true if it did.
    bool applyPendingLatch();

    std::optional<qint64> nextDeadline() const;
    bool canBlock() const;
    bool hasHidraw() const;

    // poll() on the control fds and every pad fd (one reused pollfd array).
    // Drains what was readable; closes and marks fds that hung up.
    WaitResult wait(int timeoutMs);
    // Tries to reopen fds that hung up (devices re-created under the same
    // path). Returns how many came back.
    int reopenLost();

    // The pad to rumble: the last active one, if it can rumble and no app
    // runs (checked again on the hub thread: the request may predate the
    // launch).
    std::optional<quint32> rumbleTarget() const;

    int coalesced() const { return m_gate.coalesced(); }

    // Default opener (see class comment).
    static int openWakeFd(const QString &path, QString *why);
    // True if an EV_KEY bitmap has any of KEY_A..KEY_Z.
    static bool looksLikeKeyboard(const unsigned char *keyBits, size_t bytes);

private:
    void deliver(Pad &pad, const PadInput::Events &events);
    void closeFd(Pad &pad);

    InputRouter *m_router;
    InputGate m_gate;
    Opener m_opener;
    std::vector<std::unique_ptr<Pad>> m_pads;
    std::vector<pollfd> m_pollFds;
    int m_eventFd = -1;
    int m_inotifyFd = -1;
    quint32 m_lastActive = 0;
    bool m_styleSent = false;
    InputRouter::PadStyle m_sentStyle = InputRouter::PadStyle::Xbox;
    std::atomic<bool> m_latchPending { false };
};
