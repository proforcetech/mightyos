#include "padinput.h"

#include <algorithm>
#include <cmath>

namespace PadInput {

QString dirName(Dir dir)
{
    switch (dir) {
    case Dir::Left:  return QStringLiteral("left");
    case Dir::Right: return QStringLiteral("right");
    case Dir::Up:    return QStringLiteral("up");
    case Dir::Down:  return QStringLiteral("down");
    }
    return {};
}

int dirIndex(const QString &arg)
{
    if (arg == QLatin1StringView("left"))  return int(Dir::Left);
    if (arg == QLatin1StringView("right")) return int(Dir::Right);
    if (arg == QLatin1StringView("up"))    return int(Dir::Up);
    if (arg == QLatin1StringView("down"))  return int(Dir::Down);
    return -1;
}

Binding bindingFor(Button button)
{
    switch (button) {
    case Button::South:         return { Action::Accept, {} };
    case Button::East:          return { Action::Back, {} };
    case Button::West:          return { Action::Favorite, {} };
    case Button::North:         return { Action::Search, {} };
    case Button::Start:         return { Action::Options, {} };
    case Button::Back:          return { Action::QuickPanel, {} };
    case Button::LeftShoulder:  return { Action::PrevCategory, {} };
    case Button::RightShoulder: return { Action::NextCategory, {} };
    case Button::DpadUp:        return { Action::Navigate, dirName(Dir::Up) };
    case Button::DpadDown:      return { Action::Navigate, dirName(Dir::Down) };
    case Button::DpadLeft:      return { Action::Navigate, dirName(Dir::Left) };
    case Button::DpadRight:     return { Action::Navigate, dirName(Dir::Right) };
    case Button::Guide:         // PadState: GuidePressed / GuideHeld
    case Button::LeftStick:
    case Button::RightStick:
    case Button::Other:
        break;
    }
    return {};
}

// --- RepeatTimer -------------------------------------------------------------

void RepeatTimer::start(qint64 now)
{
    m_active = true;
    m_repeats = 0;
    m_next = now + m_timing.initialDelayMs;
}

void RepeatTimer::resume(qint64 now)
{
    m_active = true;
    m_next = now + std::max(m_timing.minIntervalMs,
                            m_timing.firstIntervalMs - m_timing.accelStepMs * m_repeats);
}

bool RepeatTimer::due(qint64 now)
{
    if (!m_active || now < m_next)
        return false;
    const int interval = std::max(m_timing.minIntervalMs,
                                  m_timing.firstIntervalMs - m_timing.accelStepMs * m_repeats);
    ++m_repeats;
    m_next = now + interval;   // from now: no catch-up burst after a stall
    return true;
}

std::optional<qint64> RepeatTimer::deadline() const
{
    if (!m_active)
        return std::nullopt;
    return m_next;
}

// --- TokenBucket -------------------------------------------------------------

bool TokenBucket::take(qint64 now)
{
    if (!m_started) {
        m_started = true;
        m_last = now;
    }
    if (now > m_last) {
        m_tokens = std::min(m_burst, m_tokens + double(now - m_last) * m_rate / 1000.0);
        m_last = now;
    }
    if (m_tokens < 1.0)
        return false;
    m_tokens -= 1.0;
    return true;
}

// --- PadState ----------------------------------------------------------------

namespace {

Event actionEvent(Action action, const QString &arg = {}, bool repeat = false)
{
    return { Event::Kind::Action, action, arg, repeat };
}

} // namespace

PadState::PadState(const Timing &timing)
    : m_timing(timing)
    , m_repeat(timing)
{
}

std::optional<Dir> PadState::heldDirection() const
{
    if (m_held.empty())
        return std::nullopt;
    return m_held.back().second;
}

void PadState::pressDir(Hold source, Dir dir, qint64 now, Events &out)
{
    const auto before = heldDirection();
    std::erase_if(m_held, [source](const auto &h) { return h.first == source; });
    m_held.emplace_back(source, dir);
    if (before == dir)
        return;   // already held from the other input: no double step
    out.push_back(actionEvent(Action::Navigate, dirName(dir)));
    m_repeat.start(now);
}

void PadState::releaseDir(Hold source, qint64 now)
{
    const auto before = heldDirection();
    std::erase_if(m_held, [source](const auto &h) { return h.first == source; });
    const auto after = heldDirection();
    if (!after)
        m_repeat.stop();
    else if (after != before)
        m_repeat.start(now);   // the other input still holds a direction
}

void PadState::button(Button button, bool down, qint64 now, Events &out)
{
    switch (button) {
    case Button::DpadUp:
    case Button::DpadDown:
    case Button::DpadLeft:
    case Button::DpadRight: {
        const Hold hold = button == Button::DpadUp ? Hold::DpadUp
                        : button == Button::DpadDown ? Hold::DpadDown
                        : button == Button::DpadLeft ? Hold::DpadLeft : Hold::DpadRight;
        const Dir dir = button == Button::DpadUp ? Dir::Up
                      : button == Button::DpadDown ? Dir::Down
                      : button == Button::DpadLeft ? Dir::Left : Dir::Right;
        if (down)
            pressDir(hold, dir, now, out);
        else
            releaseDir(hold, now);
        return;
    }
    case Button::Guide:
        if (down && !m_guideDown) {
            m_guideDown = true;
            m_guideHeldSent = false;
            m_guideDownAt = now;
            out.push_back({ Event::Kind::GuidePressed, Action::None, {}, false });
        } else if (!down) {
            m_guideDown = false;
        }
        return;
    default:
        break;
    }
    if (!down)
        return;
    const Binding binding = bindingFor(button);
    if (binding.action != Action::None)
        out.push_back(actionEvent(binding.action, binding.arg));
}

void PadState::axis(Axis axis, float value, qint64 now, Events &out)
{
    if (!std::isfinite(value))
        return;
    switch (axis) {
    case Axis::LeftX:
        m_stickX = std::clamp(value, -1.0f, 1.0f);
        updateStick(now, out);
        break;
    case Axis::LeftY:
        m_stickY = std::clamp(value, -1.0f, 1.0f);
        updateStick(now, out);
        break;
    case Axis::LeftTrigger:
        updateTrigger(0, value, out);
        break;
    case Axis::RightTrigger:
        updateTrigger(1, value, out);
        break;
    case Axis::RightX:
    case Axis::RightY:
        break;   // unused in the launcher
    }
}

void PadState::updateStick(qint64 now, Events &out)
{
    const float ax = std::fabs(m_stickX);
    const float ay = std::fabs(m_stickY);

    if (m_stickLatched) {
        if (std::max(ax, ay) < kStickRelease)
            m_stickLatched = false;
        return;
    }

    if (m_stickDir) {
        const Dir d = *m_stickDir;
        const bool horizontal = d == Dir::Left || d == Dir::Right;
        const float along = d == Dir::Left ? -m_stickX : d == Dir::Right ? m_stickX
                          : d == Dir::Up ? -m_stickY : m_stickY;
        const float other = horizontal ? ay : ax;
        // Switching needs a clear margin, so a diagonal held at (0.7, -0.7)
        // doesn't flip back and forth on sensor noise.
        if (other >= kStickPress && other > along * kStickSwitchRatio) {
            const Dir next = horizontal ? (m_stickY < 0 ? Dir::Up : Dir::Down)
                                        : (m_stickX < 0 ? Dir::Left : Dir::Right);
            m_stickDir = next;
            pressDir(Hold::Stick, next, now, out);
            return;
        }
        if (along >= kStickRelease)
            return;   // still held
        m_stickDir.reset();
        releaseDir(Hold::Stick, now);
        // fall through: a fast flick may already be past the opposite threshold
    }

    if (std::max(ax, ay) >= kStickPress) {
        const Dir dir = ax >= ay ? (m_stickX < 0 ? Dir::Left : Dir::Right)
                                 : (m_stickY < 0 ? Dir::Up : Dir::Down);
        m_stickDir = dir;
        pressDir(Hold::Stick, dir, now, out);
    }
}

void PadState::updateTrigger(int index, float value, Events &out)
{
    bool &down = m_triggerDown[size_t(index)];
    if (!down && value >= kTriggerPress) {
        down = true;
        out.push_back(actionEvent(index == 0 ? Action::PageUp : Action::PageDown));
    } else if (down && value <= kTriggerRelease) {
        down = false;
    }
}

void PadState::tick(qint64 now, Events &out)
{
    if (const auto dir = heldDirection(); dir && m_repeat.due(now))
        out.push_back(actionEvent(Action::Navigate, dirName(*dir), true));
    if (m_guideDown && !m_guideHeldSent && now - m_guideDownAt >= m_timing.guideHoldMs) {
        m_guideHeldSent = true;
        out.push_back({ Event::Kind::GuideHeld, Action::None, {}, false });
    }
}

std::optional<qint64> PadState::nextDeadline() const
{
    std::optional<qint64> deadline = m_held.empty() ? std::nullopt : m_repeat.deadline();
    if (m_guideDown && !m_guideHeldSent) {
        const qint64 hold = m_guideDownAt + m_timing.guideHoldMs;
        deadline = deadline ? std::min(*deadline, hold) : hold;
    }
    return deadline;
}

void PadState::latchHeld()
{
    m_held.clear();
    m_repeat.stop();
    if (m_stickDir) {
        m_stickDir.reset();
        m_stickLatched = true;
    }
    // Triggers that are down stay down: they fire again only after a release.
}

} // namespace PadInput

// --- InputGate ---------------------------------------------------------------

InputGate::InputGate(InputRouter *router, InputRouter::Source source)
    : m_router(router)
    , m_source(source)
{
    for (auto &pending : m_pending)
        pending = std::make_shared<std::atomic<int>>(0);
}

bool InputGate::handle(const PadInput::Event &event)
{
    using Kind = PadInput::Event::Kind;
    const bool running = isAppRunning();
    switch (event.kind) {
    case Kind::GuideHeld:
        if (!running)
            return false;
        m_router->postGuideHeld(m_source);
        return true;
    case Kind::GuidePressed:
        if (running)
            return false;
        m_router->postAction(InputRouter::Action::Guide, {}, false, m_source);
        return true;
    case Kind::Action:
        break;
    }
    if (running || event.action == InputRouter::Action::None)
        return false;
    if (event.action != InputRouter::Action::Navigate) {
        m_router->postAction(event.action, event.arg, event.repeat, m_source);
        return true;
    }
    const int index = PadInput::dirIndex(event.arg);
    if (index < 0)
        return false;
    const auto &pending = m_pending[size_t(index)];
    if (event.repeat && pending->load(std::memory_order_acquire) > 0) {
        ++m_coalesced;
        return false;
    }
    pending->fetch_add(1, std::memory_order_acq_rel);
    m_router->postAction(event.action, event.arg, event.repeat, m_source, pending);
    return true;
}

void InputGate::handle(const PadInput::Events &events)
{
    for (const auto &event : events)
        handle(event);
}
