#pragma once

#include "padinput.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

// HDMI-CEC remote logic without any device access (CecService does that):
// UI command codes -> actions, message parsing, the follower replies, and
// press/hold/repeat handling. Pure and tested.
namespace Cec {

// CEC 1.4 "UI Command" operands of <User Control Pressed>.
enum UiCommand : quint8 {
    Select = 0x00, Up = 0x01, Down = 0x02, Left = 0x03, Right = 0x04,
    RootMenu = 0x09, SetupMenu = 0x0A, ContentsMenu = 0x0B, FavoriteMenu = 0x0C,
    Exit = 0x0D,                          // "Back" in CEC 2.0
    MediaContextMenu = 0x11,
    Number0 = 0x20, Number9 = 0x29, Enter = 0x2B,
    ChannelUp = 0x30, ChannelDown = 0x31,
    DisplayInformation = 0x35,
    Rewind = 0x48, FastForward = 0x49, Forward = 0x4B, Backward = 0x4C,
    F1Blue = 0x71, F2Red = 0x72, F3Green = 0x73, F4Yellow = 0x74,
};

// Opcodes used here.
enum Opcode : quint8 {
    OpFeatureAbort = 0x00,
    OpGiveDeckStatus = 0x1A, OpDeckStatus = 0x1B,
    OpStandby = 0x36,
    OpUserControlPressed = 0x44, OpUserControlReleased = 0x45,
    OpGiveOsdName = 0x46, OpSetOsdName = 0x47,
    OpRoutingChange = 0x80, OpRoutingInformation = 0x81,
    OpActiveSource = 0x82, OpRequestActiveSource = 0x85, OpSetStreamPath = 0x86,
    OpMenuRequest = 0x8D, OpMenuStatus = 0x8E,
    OpGiveDevicePowerStatus = 0x8F, OpReportPowerStatus = 0x90,
    OpAbort = 0xFF,
};

constexpr quint8 kBroadcast = 0x0F;
constexpr quint8 kUnregistered = 0x0F;
constexpr quint16 kInvalidPhysicalAddress = 0xFFFF;

// Select/Enter = Accept, arrows = Navigate, Exit = Back, Root Menu = Guide
// (Home), Contents Menu / Media Context Menu = Options, Setup Menu = Quick
// panel, colour keys red = Options, green = Quick panel, yellow = Search,
// blue = Favorite, Channel Up/Down = PageUp/PageDown, Rewind/Backward and
// Fast forward/Forward = previous/next category. Numbers and the rest: None.
PadInput::Binding bindingFor(quint8 code);

// Names for the fake backend and logs ("select", "up", "root-menu", "red",
// "channel-up", "0".."9", ...). Case-insensitive.
std::optional<quint8> codeForName(const QString &name);
QString nameForCode(quint8 code);

struct KeyEvent {
    bool pressed = false;   // false: released
    quint8 code = 0;        // only meaningful when pressed
    bool operator==(const KeyEvent &) const = default;
};

// <User Control Pressed>/<Released> addressed to a logical address.
// Broadcast-addressed key frames are dropped (PLAN D18): no TV sends them,
// any device on the bus could. Anything else: nullopt.
std::optional<KeyEvent> parseKeyMessage(const quint8 *msg, int len);

// What the follower knows about itself. `activeSource` only becomes true
// when the TV routes to our physical address (<Set Stream Path>); MightyOS
// never claims it on its own (PLAN D17).
struct FollowerState {
    quint8 address = kUnregistered;                  // our logical address
    quint16 physicalAddress = kInvalidPhysicalAddress;
    bool activeSource = false;
};

// The follower duties, for one received frame (updates `state`):
//  directed to us:
//    <Give OSD Name> -> <Set OSD Name>; <Give Device Power Status> -> on;
//    <Menu Request> -> menu active; <Give Deck Status> -> Stop (Playback
//    devices must answer it); other opcodes -> <Feature Abort>
//    "unrecognized", except replies/reports, <Standby>, <Abort> and the
//    key messages.
//  broadcast:
//    <Set Stream Path> to our physical address -> we are the active source,
//    reply <Active Source>; to another one, <Active Source> /
//    <Routing Change> / <Routing Information> elsewhere -> not active.
//    <Request Active Source> -> <Active Source> only while active.
//    Nothing else is ever answered for a broadcast frame.
// Returns the reply bytes, or empty.
std::vector<quint8> followerReply(const quint8 *msg, int len, FollowerState &state,
                                  const QByteArray &osdName);

// One remote. CEC remotes repeat <User Control Pressed> themselves while a
// key is held (every 200-450 ms) and should send <Released>.
//  - Repeats of the held key only refresh it.
//  - Navigate gets our own repeat: kTiming (350 ms, then a steady 100 ms,
//    no acceleration: the remote's timing is coarse).
//  - The local repeat pauses when no refresh came within 1.25x the
//    observed refresh interval (400 ms assumed before the first), so a lost
//    <Released> stops scrolling within ~0.5 s; a late refresh resumes it.
//  - The key counts as released kHoldTimeoutMs (CEC 1.4: 550 ms) after
//    the last press/refresh; the same key after that is a new press.
//  - Root Menu yields GuidePressed and GuideHeld: InputGate turns that into
//    Home, or guideHeld while an app runs.
class RemoteState
{
public:
    static constexpr int kHoldTimeoutMs = 550;
    static constexpr int kAssumedRefreshMs = 400;
    static constexpr int kMinRefreshMs = 100;
    static constexpr double kRefreshSlack = 1.25;
    static PadInput::Timing timing() { return { 350, 100, 100, 0, 1000 }; }

    RemoteState() : m_repeat(timing()) {}

    void press(quint8 code, qint64 now, PadInput::Events &out);
    void release(qint64 now);
    void tick(qint64 now, PadInput::Events &out);
    std::optional<qint64> nextDeadline() const;
    // The held key is ignored until it is released (app start/exit).
    void latchHeld();

private:
    PadInput::RepeatTimer m_repeat;
    std::optional<quint8> m_held;
    std::optional<quint8> m_latched;
    QString m_heldArg;
    bool m_navigating = false;
    bool m_paused = false;
    qint64 m_lastRefresh = 0;
    qint64 m_repeatUntil = 0;
    qint64 m_holdUntil = 0;
};

} // namespace Cec
