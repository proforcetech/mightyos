#pragma once

#include "inputrouter.h"
#include "padhub.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// Gamepads via SDL3 on a dedicated thread (PLAN D6, section 3).
//
// SDL is initialised with SDL_INIT_GAMEPAD only (no video, no window) on the
// service thread and used only there. Any number of controllers can be
// connected (hotplug) and any of them navigates. Everything that isn't SDL
// lives in PadHub (pads, PadState, InputGate posting, the wake-up poll),
// which is tested on its own; actions reach the GUI thread through
// InputRouter::postAction(..., Source::Gamepad).
//
// LIFETIME RULE: the thread posts to the InputRouter passed in. stop() joins
// it and must run before that router (i.e. the QML engine owning the
// singleton) is destroyed. main() calls stopOnAboutToQuit(), so the thread
// is joined on QCoreApplication::aboutToQuit while everything is still
// alive; the destructor stops it as a safety net, so also declare the
// service *after* the engine. stop() is idempotent and posts a
// "disconnected" notice for every pad.
//
// Waiting: SDL_WaitEvent without a video subsystem polls every 1 ms (about
// 2.5% of a core with a controller open on the test PC). So the thread
// blocks in PadHub::wait(): poll() on MightyOS's own read-only wake-up fds
// of the pads (evdev or hidraw), inotify on /dev/input and an eventfd, and
// pumps SDL only when something happens or a deadline (repeat, Guide hold,
// rumble end) is due. While rumbling with a HIDAPI (hidraw) pad, SDL is
// pumped every 10 ms, since those drivers refresh rumble in their update.
// A pad without a wake-up fd (or no eventfd) falls back to
// SDL_WaitEventTimeout while it is connected (logged). Quit, rumble and
// latch requests push an SDL user event *and* signal the eventfd, so both
// waits return at once.
//
// Never grabs devices: launched games read the same nodes. While an app
// runs (setAppRunning) nothing is posted except guideHeld, and there is no
// rumble.
//
// Mappings: SDL's built-in database, SDL_GAMECONTROLLERCONFIG(_FILE) from
// the environment (read by SDL itself), then the optional user file
// $XDG_CONFIG_HOME/mightyos/gamecontrollerdb.txt (regular file, at most
// kMaxMappingFileBytes). Nothing is downloaded.
//
// Steam: SDL already hides Steam's virtual gamepads (28de:11ff) unless
// SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD is set; they are skipped
// here explicitly as well, so the physical pad is the only one seen.
class GamepadService : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 kMaxMappingFileBytes = 2 * 1024 * 1024;

    struct DeviceInfo {
        quint32 id = 0;          // SDL_JoystickID, unique per connection
        QString name;            // sanitised (TextSanitize::label)
        QString type;            // SDL_GetGamepadStringForType(), e.g. "xboxone"
        QString guid;
        QString path;            // e.g. /dev/input/event4
        InputRouter::PadStyle style = InputRouter::PadStyle::Xbox;
        bool rumble = false;
        bool initial = false;    // present when the service started
        bool wakeFd = false;     // MightyOS could open its wake-up fd
    };

    explicit GamepadService(InputRouter *router, QObject *parent = nullptr);
    ~GamepadService() override;

    // Default: $XDG_CONFIG_HOME/mightyos/gamecontrollerdb.txt. Before start().
    void setUserMappingFile(const QString &path) { m_mappingFile = path; }
    QString userMappingFile() const { return m_mappingFile; }

    // Starts the thread (SDL is initialised there). Returns false if it is
    // already running. SDL init failures are logged and reported through
    // ready(false); isStarted() is then false.
    bool start();
    // Wakes and joins the thread (SDL_Quit runs on it). Idempotent.
    void stop();
    bool isStarted() const { return m_thread.joinable() && m_alive.load(); }
    // Connects QCoreApplication::aboutToQuit to stop().
    void stopOnAboutToQuit();

    // Any thread.
    void setAppRunning(bool running);
    bool isAppRunning() const { return m_hub.isAppRunning(); }

    // Any thread. Queues a short pulse on the controller used last. Returns
    // false (nothing queued) while an app runs or the thread isn't running.
    // The service thread checks again (PadHub::rumbleTarget()).
    bool requestRumble(InputRouter::Feedback kind);

    // How many Navigate repeats were coalesced (diagnostics).
    int coalescedRepeats() const { return m_coalesced.load(); }

    // Style for an SDL_GamepadType, from SDL's own label of the south
    // button (Cross -> PlayStation, B -> Nintendo, else Xbox).
    static InputRouter::PadStyle styleForSdlType(int sdlGamepadType);
    // Steam Input's virtual gamepad.
    static bool isSteamVirtual(quint16 vendor, quint16 product);
    // Version of the SDL library in use, e.g. "3.4.2".
    static QString sdlVersion();

signals:
    // Emitted on the service thread (connect queued / auto).
    void ready(bool ok, const QString &detail);
    void deviceAdded(const GamepadService::DeviceInfo &info);
    void deviceRemoved(const GamepadService::DeviceInfo &info);

private:
    struct Command { InputRouter::Feedback rumble; };
    struct SdlPad { DeviceInfo info; void *gamepad = nullptr; };

    void run();
    void wake();
    void loadUserMappings();
    void handleSdlEvent(const void *event, qint64 now);
    void addPad(quint32 id, bool initial);
    void removePad(quint32 id, bool stopping);
    SdlPad *sdlPad(quint32 id);
    void runCommands(qint64 now);
    void waitForInput(qint64 now);
    void pumpSdl();

    InputRouter *m_router;
    PadHub m_hub;
    QString m_mappingFile;

    std::thread m_thread;
    std::atomic<bool> m_quit { false };
    std::atomic<bool> m_alive { false };
    std::atomic<int> m_coalesced { 0 };

    // Guards SDL_PushEvent from other threads against SDL_Quit, the eventfd
    // against stop() closing it, and the command queue.
    std::mutex m_mutex;
    bool m_sdlReady = false;
    quint32 m_wakeEventType = 0;
    std::vector<Command> m_commands;
    int m_eventFd = -1;

    // Service thread only.
    std::vector<SdlPad> m_sdlPads;
    std::vector<Command> m_commandBatch;
    int m_inotifyFd = -1;
    qint64 m_rumbleUntil = 0;
    qint64 m_hotplugSettleUntil = 0;
    bool m_usingSdlWait = false;
};

Q_DECLARE_METATYPE(GamepadService::DeviceInfo)
