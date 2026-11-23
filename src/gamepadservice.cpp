#include "gamepadservice.h"

#include "jsonfile.h"
#include "textsanitize.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>

#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcGamepad, "mightyos.gamepad", QtInfoMsg)

namespace {

using PadInput::Axis;
using PadInput::Button;

// Upper bound for one wait: SDL is pumped at least this often (hotplug of
// devices SDL finds without an inotify event, e.g. Bluetooth via uhid).
constexpr int kIdleWaitMs = 2000;
// After /dev/input changed, pump this often for a while: udev finishes
// (permissions, SDL's own udev monitor) shortly after the node appears.
constexpr int kHotplugSettleMs = 1500;
constexpr int kHotplugPollMs = 100;
// While an app runs, wake-ups from the game's controller input are batched.
constexpr int kRunningBatchMs = 16;
// HIDAPI drivers refresh rumble in their update: pump this often meanwhile.
constexpr int kHidrawRumblePumpMs = 10;

struct Pulse { Uint16 low, high; Uint32 ms; };
// Short and light; Error a little longer and stronger.
constexpr Pulse kBumpPulse { 0x1800, 0x3000, 60 };
constexpr Pulse kErrorPulse { 0x3800, 0x5000, 140 };

qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Button buttonFor(Uint8 sdlButton)
{
    switch (sdlButton) {
    case SDL_GAMEPAD_BUTTON_SOUTH:          return Button::South;
    case SDL_GAMEPAD_BUTTON_EAST:           return Button::East;
    case SDL_GAMEPAD_BUTTON_WEST:           return Button::West;
    case SDL_GAMEPAD_BUTTON_NORTH:          return Button::North;
    case SDL_GAMEPAD_BUTTON_BACK:           return Button::Back;
    case SDL_GAMEPAD_BUTTON_GUIDE:          return Button::Guide;
    case SDL_GAMEPAD_BUTTON_START:          return Button::Start;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return Button::LeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return Button::RightStick;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return Button::LeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Button::RightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:        return Button::DpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return Button::DpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return Button::DpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return Button::DpadRight;
    default:                                return Button::Other;
    }
}

std::optional<Axis> axisFor(Uint8 sdlAxis)
{
    switch (sdlAxis) {
    case SDL_GAMEPAD_AXIS_LEFTX:         return Axis::LeftX;
    case SDL_GAMEPAD_AXIS_LEFTY:         return Axis::LeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX:        return Axis::RightX;
    case SDL_GAMEPAD_AXIS_RIGHTY:        return Axis::RightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return Axis::LeftTrigger;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return Axis::RightTrigger;
    default:                             return std::nullopt;
    }
}

InputRouter::PadStyle styleForLabel(SDL_GamepadButtonLabel southLabel)
{
    switch (southLabel) {
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return InputRouter::PadStyle::PlayStation;
    case SDL_GAMEPAD_BUTTON_LABEL_B:     return InputRouter::PadStyle::Nintendo;
    default:                             return InputRouter::PadStyle::Xbox;
    }
}

void drainFd(int fd)
{
    char buffer[64];
    while (fd >= 0 && ::read(fd, buffer, sizeof buffer) > 0) {
    }
}

QString defaultMappingFile()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/mightyos/gamecontrollerdb.txt");
}

} // namespace

GamepadService::GamepadService(InputRouter *router, QObject *parent)
    : QObject(parent)
    , m_router(router)
    , m_hub(router)
    , m_mappingFile(defaultMappingFile())
{
    qRegisterMetaType<GamepadService::DeviceInfo>();
}

GamepadService::~GamepadService()
{
    stop();
}

InputRouter::PadStyle GamepadService::styleForSdlType(int sdlGamepadType)
{
    return styleForLabel(SDL_GetGamepadButtonLabelForType(SDL_GamepadType(sdlGamepadType),
                                                          SDL_GAMEPAD_BUTTON_SOUTH));
}

bool GamepadService::isSteamVirtual(quint16 vendor, quint16 product)
{
    return vendor == 0x28de && product == 0x11ff;
}

QString GamepadService::sdlVersion()
{
    const int v = SDL_GetVersion();
    return QStringLiteral("%1.%2.%3").arg(SDL_VERSIONNUM_MAJOR(v)).arg(SDL_VERSIONNUM_MINOR(v))
                                       .arg(SDL_VERSIONNUM_MICRO(v));
}

bool GamepadService::start()
{
    if (m_thread.joinable())
        return false;
    m_quit = false;
    m_alive = true;
    {
        std::lock_guard lock(m_mutex);
        m_eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_eventFd < 0)
            qCWarning(lcGamepad) << "eventfd failed:" << qt_error_string(errno)
                                 << "- using SDL's 1 ms wait";
    }
    m_thread = std::thread([this] { run(); });
    return true;
}

void GamepadService::stop()
{
    if (!m_thread.joinable())
        return;
    m_quit = true;
    wake();
    m_thread.join();
    m_alive = false;
    std::lock_guard lock(m_mutex);
    if (m_eventFd >= 0)
        ::close(m_eventFd);
    m_eventFd = -1;
    m_commands.clear();
}

void GamepadService::stopOnAboutToQuit()
{
    if (auto *app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, &GamepadService::stop, Qt::DirectConnection);
}

void GamepadService::setAppRunning(bool running)
{
    if (m_hub.isAppRunning() == running)
        return;
    m_hub.setAppRunning(running);   // the thread latches whatever is held
    wake();
}

bool GamepadService::requestRumble(InputRouter::Feedback kind)
{
    if (m_hub.isAppRunning())
        return false;
    {
        std::lock_guard lock(m_mutex);
        if (!m_sdlReady || m_commands.size() > 8)
            return false;
        m_commands.push_back({ kind });
    }
    wake();
    return true;
}

void GamepadService::wake()
{
    std::lock_guard lock(m_mutex);
    if (m_sdlReady) {
        SDL_Event event {};
        event.type = m_wakeEventType;
        SDL_PushEvent(&event);
    }
    if (m_eventFd >= 0) {
        const uint64_t one = 1;
        [[maybe_unused]] const ssize_t n = ::write(m_eventFd, &one, sizeof one);
    }
}

void GamepadService::run()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Qt (and the M6 session) own SIGINT/SIGTERM; SDL must not turn them
    // into SDL_EVENT_QUIT.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        const QString error = QString::fromUtf8(SDL_GetError());
        qCWarning(lcGamepad) << "SDL gamepad init failed:" << error;
        m_alive = false;
        emit ready(false, error);
        return;
    }
    const Uint32 wakeType = SDL_RegisterEvents(1);
    int eventFd = -1;
    {
        std::lock_guard lock(m_mutex);
        m_wakeEventType = wakeType;
        m_sdlReady = wakeType != 0;
        eventFd = m_eventFd;   // stays valid until stop() has joined us
    }

    loadUserMappings();

    m_inotifyFd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_inotifyFd >= 0 && ::inotify_add_watch(m_inotifyFd, "/dev/input", IN_CREATE | IN_ATTRIB | IN_DELETE) < 0) {
        ::close(m_inotifyFd);
        m_inotifyFd = -1;
    }
    m_hub.setControlFds(eventFd, m_inotifyFd);

    int count = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i)
            addPad(ids[i], true);
        SDL_free(ids);
    }
    emit ready(true, QStringLiteral("SDL %1").arg(sdlVersion()));

    while (!m_quit) {
        const qint64 now = nowMs();
        m_hub.applyPendingLatch();
        runCommands(now);
        m_hub.tick(now);
        m_coalesced = m_hub.coalesced();
        waitForInput(nowMs());
    }

    while (!m_sdlPads.empty())
        removePad(m_sdlPads.back().info.id, true);
    m_hub.clear();
    m_hub.setControlFds(-1, -1);
    if (m_inotifyFd >= 0)
        ::close(m_inotifyFd);
    m_inotifyFd = -1;
    {
        std::lock_guard lock(m_mutex);
        m_sdlReady = false;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_Quit();
}

void GamepadService::loadUserMappings()
{
    if (m_mappingFile.isEmpty())
        return;
    // Our own checks (regular file, no FIFO, size cap) on the opened fd, then
    // SDL's mapping parser on the bytes we read.
    const auto data = JsonFile::readRegularFile(m_mappingFile, kMaxMappingFileBytes, true);
    if (!data) {
        if (QFileInfo::exists(m_mappingFile))
            qCWarning(lcGamepad) << "ignoring unusable mapping file" << m_mappingFile
                                 << "(not a regular file, unreadable or over" << kMaxMappingFileBytes << "bytes)";
        return;
    }
    SDL_IOStream *io = SDL_IOFromConstMem(data->constData(), size_t(data->size()));
    const int added = io ? SDL_AddGamepadMappingsFromIO(io, true) : -1;
    if (added < 0)
        qCWarning(lcGamepad) << "could not load" << m_mappingFile << ":" << SDL_GetError();
    else
        qCInfo(lcGamepad) << "loaded" << added << "controller mappings from" << m_mappingFile;
}

GamepadService::SdlPad *GamepadService::sdlPad(quint32 id)
{
    for (auto &pad : m_sdlPads) {
        if (pad.info.id == id)
            return &pad;
    }
    return nullptr;
}

void GamepadService::addPad(quint32 id, bool initial)
{
    if (sdlPad(id))
        return;   // the ADDED event for a pad we found at startup
    if (isSteamVirtual(SDL_GetGamepadVendorForID(id), SDL_GetGamepadProductForID(id))) {
        qCInfo(lcGamepad) << "skipping Steam virtual gamepad" << id;
        return;
    }
    SDL_Gamepad *gamepad = SDL_OpenGamepad(id);
    if (!gamepad) {
        qCWarning(lcGamepad) << "could not open gamepad" << id << ":" << SDL_GetError();
        return;
    }
    SdlPad pad;
    pad.gamepad = gamepad;
    DeviceInfo &info = pad.info;
    info.id = id;
    // Descriptor strings are untrusted: no controls, bidi or zero-width.
    info.name = TextSanitize::label(QString::fromUtf8(SDL_GetGamepadName(gamepad)));
    if (info.name.isEmpty())
        info.name = QStringLiteral("Gamepad");
    const char *type = SDL_GetGamepadStringForType(SDL_GetGamepadType(gamepad));
    info.type = type ? QString::fromLatin1(type) : QStringLiteral("unknown");
    char guid[33] = {};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof guid);
    info.guid = QString::fromLatin1(guid);
    const char *path = SDL_GetGamepadPath(gamepad);
    info.path = path ? QString::fromUtf8(path) : QString();
    info.style = styleForLabel(SDL_GetGamepadButtonLabel(gamepad, SDL_GAMEPAD_BUTTON_SOUTH));
    info.rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(gamepad),
                                         SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    info.initial = initial;
    info.wakeFd = m_hub.add(id, info.name, info.path, info.style, info.rumble).fd >= 0;

    qCInfo(lcGamepad).noquote() << "connected" << info.name << "type" << info.type << "id" << id;
    m_sdlPads.push_back(pad);
    m_router->postDeviceNotice(true, id, info.name, InputRouter::Source::Gamepad, initial);
    emit deviceAdded(info);
}

void GamepadService::removePad(quint32 id, bool stopping)
{
    const auto it = std::find_if(m_sdlPads.begin(), m_sdlPads.end(),
                                 [id](const SdlPad &pad) { return pad.info.id == id; });
    if (it == m_sdlPads.end())
        return;
    const DeviceInfo info = it->info;
    SDL_CloseGamepad(static_cast<SDL_Gamepad *>(it->gamepad));
    m_sdlPads.erase(it);
    m_hub.remove(id);
    if (!stopping)
        qCInfo(lcGamepad).noquote() << "disconnected" << info.name << "id" << id;
    m_router->postDeviceNotice(false, id, info.name, InputRouter::Source::Gamepad, stopping);
    emit deviceRemoved(info);
}

void GamepadService::handleSdlEvent(const void *rawEvent, qint64 now)
{
    const auto &event = *static_cast<const SDL_Event *>(rawEvent);
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        addPad(event.gdevice.which, false);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        removePad(event.gdevice.which, false);
        break;
    case SDL_EVENT_GAMEPAD_REMAPPED:
        if (SdlPad *pad = sdlPad(event.gdevice.which)) {
            pad->info.style = styleForLabel(SDL_GetGamepadButtonLabel(static_cast<SDL_Gamepad *>(pad->gamepad),
                                                                      SDL_GAMEPAD_BUTTON_SOUTH));
            if (PadHub::Pad *hubPad = m_hub.find(pad->info.id))
                hubPad->style = pad->info.style;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        m_hub.button(event.gbutton.which, buttonFor(event.gbutton.button), event.gbutton.down, now);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (const auto axis = axisFor(event.gaxis.axis))
            m_hub.axis(event.gaxis.which, *axis, float(event.gaxis.value) / 32767.0f, now);
        break;
    default:
        break;
    }
}

void GamepadService::runCommands(qint64 now)
{
    m_commandBatch.clear();
    {
        std::lock_guard lock(m_mutex);
        m_commandBatch.swap(m_commands);
    }
    for (const Command &command : m_commandBatch) {
        const auto target = m_hub.rumbleTarget();   // nullopt while an app runs
        SdlPad *pad = target ? sdlPad(*target) : nullptr;
        if (!pad)
            continue;
        const Pulse &pulse = command.rumble == InputRouter::Feedback::Error ? kErrorPulse : kBumpPulse;
        if (SDL_RumbleGamepad(static_cast<SDL_Gamepad *>(pad->gamepad), pulse.low, pulse.high, pulse.ms))
            m_rumbleUntil = std::max(m_rumbleUntil, now + pulse.ms + 10);
    }
}

void GamepadService::pumpSdl()
{
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event))
        handleSdlEvent(&event, nowMs());
}

void GamepadService::waitForInput(qint64 now)
{
    int timeout = kIdleWaitMs;
    if (const auto deadline = m_hub.nextDeadline())
        timeout = int(std::clamp<qint64>(*deadline - now, 0, kIdleWaitMs));
    if (now < m_hotplugSettleUntil)
        timeout = std::min(timeout, kHotplugPollMs);
    if (m_rumbleUntil > 0) {
        if (now >= m_rumbleUntil)
            m_rumbleUntil = 0;   // this pump stops it
        else
            timeout = std::min(timeout, int(m_rumbleUntil - now));
        if (m_rumbleUntil > 0 && m_hub.hasHidraw())
            timeout = std::min(timeout, kHidrawRumblePumpMs);
    }

    const bool sdlWait = !m_hub.canBlock();
    if (sdlWait != m_usingSdlWait) {
        m_usingSdlWait = sdlWait;
        qCInfo(lcGamepad) << (sdlWait ? "waiting with SDL's 1 ms polling (a pad has no wake-up fd)"
                                      : "waiting in poll() on the pads' wake-up fds");
    }

    if (sdlWait) {
        int eventFd = -1;
        {
            std::lock_guard lock(m_mutex);
            eventFd = m_eventFd;
        }
        // A wake-up that came before SDL could take user events is on the eventfd.
        pollfd wakeFd { eventFd, POLLIN, 0 };
        if (eventFd >= 0 && ::poll(&wakeFd, 1, 0) > 0) {
            drainFd(eventFd);
            pumpSdl();
            return;
        }
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, timeout)) {
            handleSdlEvent(&event, nowMs());
            while (SDL_PollEvent(&event))
                handleSdlEvent(&event, nowMs());
        }
        drainFd(eventFd);
        char buffer[512];
        if (m_inotifyFd >= 0 && ::read(m_inotifyFd, buffer, sizeof buffer) > 0) {
            drainFd(m_inotifyFd);
            m_hotplugSettleUntil = nowMs() + kHotplugSettleMs;
        }
        if (nowMs() < m_hotplugSettleUntil)
            m_hub.reopenLost();   // back to poll() once every pad has its fd
        return;
    }

    const PadHub::WaitResult result = m_hub.wait(timeout);
    if (result.hotplug)
        m_hotplugSettleUntil = nowMs() + kHotplugSettleMs;
    if (nowMs() < m_hotplugSettleUntil)
        m_hub.reopenLost();
    if (!result.input.empty() && m_hub.isAppRunning() && !m_quit) {
        // Only Guide matters while a game runs: batch its input.
        int eventFd = -1;
        {
            std::lock_guard lock(m_mutex);
            eventFd = m_eventFd;
        }
        pollfd quitFd { eventFd, POLLIN, 0 };
        ::poll(&quitFd, 1, kRunningBatchMs);
    }
    pumpSdl();
}
