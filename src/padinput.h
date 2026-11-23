#pragma once

#include "inputrouter.h"

#include <QList>
#include <QString>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

// Device-independent input logic shared by GamepadService (SDL3) and
// CecService (HDMI-CEC). No SDL, no threads, no clock of its own: every call
// gets the current time in milliseconds, so tests drive it with a fake
// clock. Times only need to be monotonic.
namespace PadInput {

using Action = InputRouter::Action;

// SDL3's positional buttons (SDL_GamepadButton), mirrored so this code has
// no SDL dependency. South is the bottom face button on every controller:
// A on Xbox, Cross on PlayStation, B on Nintendo.
enum class Button {
    South, East, West, North,
    Back, Guide, Start,
    LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Other,
};

enum class Axis { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger };

enum class Dir { Left, Right, Up, Down };
QString dirName(Dir dir);
// -1 if `arg` is not a direction name.
int dirIndex(const QString &arg);

struct Binding {
    Action action = Action::None;
    QString arg;
    bool operator==(const Binding &) const = default;
};

// PLAN section 3, positional. Guide is not an action here (see PadState).
Binding bindingFor(Button button);

// What the logic produced.
struct Event {
    enum class Kind {
        Action,         // action/arg/repeat
        GuidePressed,   // Guide went down (Home, unless an app runs)
        GuideHeld,      // Guide held >= guideHoldMs (only meaningful while an app runs)
    };
    Kind kind = Kind::Action;
    Action action = Action::None;
    QString arg;
    bool repeat = false;
    bool operator==(const Event &) const = default;
};
using Events = std::vector<Event>;

struct Timing {
    int initialDelayMs = 350;    // first repeat
    int firstIntervalMs = 90;    // then every 90 ms ...
    int minIntervalMs = 40;      // ... accelerating to 40 ms
    int accelStepMs = 5;         // per repeat (90 -> 40 in 10 repeats)
    int guideHoldMs = 1000;
};

// Auto-repeat schedule for one held key. At most one repeat per due() call:
// after a stall the schedule restarts from "now" instead of catching up with
// a burst.
class RepeatTimer
{
public:
    explicit RepeatTimer(const Timing &timing = {}) : m_timing(timing) {}
    void start(qint64 now);
    // Continue after a pause at the current speed (next repeat one
    // interval from now).
    void resume(qint64 now);
    void stop() { m_active = false; }
    bool isActive() const { return m_active; }
    // True if a repeat is due at `now` (and schedules the next one).
    bool due(qint64 now);
    std::optional<qint64> deadline() const;
    int repeats() const { return m_repeats; }

private:
    Timing m_timing;
    qint64 m_next = 0;
    int m_repeats = 0;
    bool m_active = false;
};

// Token bucket for rate limits (CEC presses, CEC replies, log lines):
// `ratePerSecond` tokens per second, at most `burst` saved up.
class TokenBucket
{
public:
    TokenBucket(double ratePerSecond, double burst) : m_rate(ratePerSecond), m_burst(burst), m_tokens(burst) {}
    // Takes a token if one is available at `now`.
    bool take(qint64 now);

private:
    double m_rate;
    double m_burst;
    double m_tokens;
    qint64 m_last = 0;
    bool m_started = false;
};

// One controller: buttons, D-pad + left stick navigation with repeat, the
// triggers, Guide press/hold.
//
// Left stick: a direction engages when the dominant axis passes
// kStickPress (0.5) and holds until it falls below kStickRelease (0.4); it
// switches to the other axis only when that one passes kStickPress *and*
// exceeds the current one by kStickSwitchRatio (1.3x), so a diagonal held
// at (0.7, -0.7) can't flip on sensor noise. Triggers: kTriggerPress (0.5) / kTriggerRelease (0.3), one
// PageUp/PageDown per pull. D-pad and stick share one "held direction":
// the most recent press wins, and pressing a direction that is already
// held from the other input does not step twice.
class PadState
{
public:
    static constexpr float kStickPress = 0.5f;
    static constexpr float kStickRelease = 0.4f;
    static constexpr float kStickSwitchRatio = 1.3f;
    static constexpr float kTriggerPress = 0.5f;
    static constexpr float kTriggerRelease = 0.3f;

    explicit PadState(const Timing &timing = {});

    void button(Button button, bool down, qint64 now, Events &out);
    // Sticks -1..1 (negative = left/up, as SDL), triggers 0..1.
    void axis(Axis axis, float value, qint64 now, Events &out);
    // Repeats and the Guide hold.
    void tick(qint64 now, Events &out);
    // Earliest time tick() has something to do.
    std::optional<qint64> nextDeadline() const;

    // Whatever is held right now (direction, stick, trigger) is ignored
    // until it is released: called when an app starts or exits, so a
    // direction held in the game doesn't start scrolling the launcher.
    void latchHeld();

    std::optional<Dir> heldDirection() const;

private:
    enum class Hold { DpadUp, DpadDown, DpadLeft, DpadRight, Stick };
    void pressDir(Hold source, Dir dir, qint64 now, Events &out);
    void releaseDir(Hold source, qint64 now);
    void updateStick(qint64 now, Events &out);
    void updateTrigger(int index, float value, Events &out);

    Timing m_timing;
    RepeatTimer m_repeat;
    std::vector<std::pair<Hold, Dir>> m_held;   // press order, last = active
    float m_stickX = 0, m_stickY = 0;
    std::optional<Dir> m_stickDir;
    bool m_stickLatched = false;
    std::array<bool, 2> m_triggerDown { false, false };
    bool m_guideDown = false;
    bool m_guideHeldSent = false;
    qint64 m_guideDownAt = 0;
};

} // namespace PadInput

// Turns PadInput events into InputRouter posts for one source. Used from
// one input thread (not thread-safe itself); setAppRunning() may be called
// from any thread.
//
//  - While an app runs, nothing is posted except guideHeld.
//  - Guide pressed while nothing runs posts Action::Guide.
//  - Coalescing: a Navigate repeat is not posted while a Navigate in that
//    direction is still queued (per-direction atomic pending count,
//    decremented by the router on delivery). First presses always go
//    through, so a quick tap is never lost.
class InputGate
{
public:
    InputGate(InputRouter *router, InputRouter::Source source);

    void setAppRunning(bool running) { m_appRunning.store(running, std::memory_order_relaxed); }
    bool isAppRunning() const { return m_appRunning.load(std::memory_order_relaxed); }

    // Returns true if something was posted.
    bool handle(const PadInput::Event &event);
    void handle(const PadInput::Events &events);

    // Statistics for tests and the input test tool.
    int coalesced() const { return m_coalesced; }

private:
    InputRouter *m_router;
    InputRouter::Source m_source;
    std::atomic<bool> m_appRunning { false };
    std::array<std::shared_ptr<std::atomic<int>>, 4> m_pending;
    int m_coalesced = 0;
};
