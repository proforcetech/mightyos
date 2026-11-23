#include "cecinput.h"

#include <algorithm>

namespace Cec {

using Action = InputRouter::Action;

namespace {

struct NamedCode { const char *name; quint8 code; };
constexpr NamedCode kNames[] = {
    { "select", Select }, { "ok", Select }, { "enter", Enter },
    { "up", Up }, { "down", Down }, { "left", Left }, { "right", Right },
    { "root-menu", RootMenu }, { "home", RootMenu },
    { "setup-menu", SetupMenu }, { "contents-menu", ContentsMenu },
    { "favorite-menu", FavoriteMenu }, { "media-context-menu", MediaContextMenu },
    { "exit", Exit }, { "back", Exit },
    { "channel-up", ChannelUp }, { "channel-down", ChannelDown },
    { "info", DisplayInformation },
    { "rewind", Rewind }, { "fast-forward", FastForward },
    { "forward", Forward }, { "backward", Backward },
    { "blue", F1Blue }, { "f1", F1Blue }, { "red", F2Red }, { "f2", F2Red },
    { "green", F3Green }, { "f3", F3Green }, { "yellow", F4Yellow }, { "f4", F4Yellow },
};

PadInput::Binding nav(const char *dir)
{
    return { Action::Navigate, QString::fromLatin1(dir) };
}

} // namespace

PadInput::Binding bindingFor(quint8 code)
{
    switch (code) {
    case Select:
    case Enter:              return { Action::Accept, {} };
    case Up:                 return nav("up");
    case Down:               return nav("down");
    case Left:               return nav("left");
    case Right:              return nav("right");
    case Exit:               return { Action::Back, {} };
    case RootMenu:           return { Action::Guide, {} };
    case ContentsMenu:
    case MediaContextMenu:
    case F2Red:              return { Action::Options, {} };
    case SetupMenu:
    case F3Green:            return { Action::QuickPanel, {} };
    case F4Yellow:           return { Action::Search, {} };
    case F1Blue:             return { Action::Favorite, {} };
    case ChannelUp:          return { Action::PageUp, {} };
    case ChannelDown:        return { Action::PageDown, {} };
    case Rewind:
    case Backward:           return { Action::PrevCategory, {} };
    case FastForward:
    case Forward:            return { Action::NextCategory, {} };
    default:                 return {};
    }
}

std::optional<quint8> codeForName(const QString &name)
{
    const QString key = name.trimmed().toLower();
    if (key.size() == 1 && key.at(0).isDigit())
        return quint8(Number0 + key.at(0).digitValue());
    for (const NamedCode &entry : kNames) {
        if (key == QLatin1StringView(entry.name))
            return entry.code;
    }
    return std::nullopt;
}

QString nameForCode(quint8 code)
{
    if (code >= Number0 && code <= Number9)
        return QString::number(code - Number0);
    for (const NamedCode &entry : kNames) {
        if (entry.code == code)
            return QString::fromLatin1(entry.name);
    }
    return QStringLiteral("0x%1").arg(code, 2, 16, QLatin1Char('0'));
}

std::optional<KeyEvent> parseKeyMessage(const quint8 *msg, int len)
{
    if (!msg || len < 2 || (msg[0] & 0x0F) == kBroadcast)
        return std::nullopt;
    const quint8 opcode = msg[1];
    if (opcode == OpUserControlPressed && len >= 3)
        return KeyEvent { true, msg[2] };
    if (opcode == OpUserControlReleased)
        return KeyEvent { false, 0 };
    return std::nullopt;
}

namespace {

quint16 address16(const quint8 *p)
{
    return quint16((p[0] << 8) | p[1]);
}

// Directed messages that are replies or reports, or handled elsewhere:
// never Feature Abort them.
bool neverAbort(quint8 opcode)
{
    switch (opcode) {
    case OpFeatureAbort:
    case OpAbort:                 // the kernel answers it
    case OpStandby:               // ignored: MightyOS doesn't power down from the TV
    case OpUserControlPressed:
    case OpUserControlReleased:
    case OpSetOsdName:
    case OpMenuStatus:
    case OpReportPowerStatus:
    case OpDeckStatus:
    case 0x9E:                    // <CEC Version>
    case 0x84:                    // <Report Physical Address>
    case 0x87:                    // <Device Vendor ID>
    case 0x32:                    // <Set Menu Language>
    case 0x7A:                    // <Report Audio Status>
    case 0x72:                    // <Set System Audio Mode>
    case 0x7E:                    // <System Audio Mode Status>
    case 0xA6:                    // <Report Features>
    case 0x9D:                    // <Inactive Source>
        return true;
    default:
        return false;
    }
}

} // namespace

std::vector<quint8> followerReply(const quint8 *msg, int len, FollowerState &state,
                                  const QByteArray &osdName)
{
    if (!msg || len < 2 || state.address >= kUnregistered)
        return {};
    const quint8 initiator = msg[0] >> 4;
    const quint8 destination = msg[0] & 0x0F;
    const quint8 opcode = msg[1];
    if (initiator == state.address)
        return {};   // our own frame

    const bool havePhysical = state.physicalAddress != kInvalidPhysicalAddress;
    const auto activeSourceReply = [&]() -> std::vector<quint8> {
        return { quint8((state.address << 4) | kBroadcast), OpActiveSource,
                 quint8(state.physicalAddress >> 8), quint8(state.physicalAddress & 0xFF) };
    };

    if (destination == kBroadcast) {
        switch (opcode) {
        case OpSetStreamPath:
            if (len < 4)
                return {};
            if (havePhysical && address16(msg + 2) == state.physicalAddress) {
                state.activeSource = true;   // the TV routed to us
                return activeSourceReply();
            }
            state.activeSource = false;
            return {};
        case OpActiveSource:
        case OpRoutingInformation:
            if (len >= 4 && address16(msg + 2) != state.physicalAddress)
                state.activeSource = false;
            return {};
        case OpRoutingChange:
            if (len >= 6 && address16(msg + 4) != state.physicalAddress)
                state.activeSource = false;
            return {};
        case OpRequestActiveSource:
            return state.activeSource && havePhysical ? activeSourceReply() : std::vector<quint8> {};
        default:
            return {};   // nothing else is answered for a broadcast
        }
    }

    if (destination != state.address || initiator == kBroadcast)
        return {};
    const quint8 header = quint8((state.address << 4) | initiator);
    switch (opcode) {
    case OpGiveOsdName: {
        std::vector<quint8> reply { header, OpSetOsdName };
        // 1..14 printable ASCII bytes.
        for (const char c : osdName.left(14)) {
            if (c >= 0x20 && c < 0x7f)
                reply.push_back(quint8(c));
        }
        if (reply.size() == 2)
            reply.push_back('?');
        return reply;
    }
    case OpGiveDevicePowerStatus:
        return { header, OpReportPowerStatus, 0x00 };   // on
    case OpMenuRequest:
        return { header, OpMenuStatus, 0x00 };          // activated: send us the keys
    case OpGiveDeckStatus:
        if (len >= 3 && msg[2] == 0x02)
            return {};                                  // "off": no reply
        return { header, OpDeckStatus, 0x1A };          // Stop
    default:
        if (neverAbort(opcode))
            return {};
        return { header, OpFeatureAbort, opcode, 0x00 };   // unrecognized opcode
    }
}

// --- RemoteState ---------------------------------------------------------------

void RemoteState::press(quint8 code, qint64 now, PadInput::Events &out)
{
    if (m_latched && *m_latched == code) {
        m_holdUntil = now + kHoldTimeoutMs;
        return;
    }
    m_latched.reset();
    if (m_held && *m_held == code) {
        // The remote's own repeat: learn its interval, keep (or resume)
        // our repeat for 1.25x that long.
        const qint64 interval = std::clamp<qint64>(now - m_lastRefresh, kMinRefreshMs, kHoldTimeoutMs);
        m_lastRefresh = now;
        m_holdUntil = now + kHoldTimeoutMs;
        m_repeatUntil = now + qint64(interval * kRefreshSlack);
        if (m_navigating && m_paused) {
            m_paused = false;
            m_repeat.resume(now);
        }
        return;
    }
    m_held.reset();
    m_repeat.stop();
    m_navigating = false;
    m_paused = false;
    const PadInput::Binding binding = bindingFor(code);
    if (binding.action == Action::None)
        return;
    m_held = code;
    m_lastRefresh = now;
    m_holdUntil = now + kHoldTimeoutMs;
    m_repeatUntil = now + qint64(kAssumedRefreshMs * kRefreshSlack);
    if (binding.action == Action::Guide) {
        // No reliable hold on a TV remote: the press is both.
        out.push_back({ PadInput::Event::Kind::GuidePressed, Action::None, {}, false });
        out.push_back({ PadInput::Event::Kind::GuideHeld, Action::None, {}, false });
        return;
    }
    out.push_back({ PadInput::Event::Kind::Action, binding.action, binding.arg, false });
    if (binding.action == Action::Navigate) {
        m_heldArg = binding.arg;
        m_navigating = true;
        m_repeat.start(now);
    }
}

void RemoteState::release(qint64)
{
    m_held.reset();
    m_latched.reset();
    m_repeat.stop();
    m_navigating = false;
    m_paused = false;
}

void RemoteState::tick(qint64 now, PadInput::Events &out)
{
    if ((m_held || m_latched) && now >= m_holdUntil) {
        release(now);   // <Released> got lost
        return;
    }
    if (!m_held || !m_navigating || m_paused)
        return;
    if (now > m_repeatUntil) {
        m_paused = true;   // no refresh: stop scrolling, wait for one
        m_repeat.stop();
        return;
    }
    if (m_repeat.due(now))
        out.push_back({ PadInput::Event::Kind::Action, Action::Navigate, m_heldArg, true });
}

std::optional<qint64> RemoteState::nextDeadline() const
{
    if (!m_held && !m_latched)
        return std::nullopt;
    qint64 deadline = m_holdUntil;
    if (m_held && m_navigating && !m_paused) {
        deadline = std::min(deadline, m_repeatUntil + 1);
        if (const auto repeat = m_repeat.deadline())
            deadline = std::min(deadline, *repeat);
    }
    return deadline;
}

void RemoteState::latchHeld()
{
    if (m_held)
        m_latched = m_held;
    m_held.reset();
    m_repeat.stop();
    m_navigating = false;
    m_paused = false;
}

} // namespace Cec
