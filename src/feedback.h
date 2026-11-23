#pragma once

class QQmlEngine;

// Haptics: SoundFx's Bump and Error sounds also ask InputRouter for a
// rumble (which applies the policy: enabled, nothing running, last input
// from a gamepad). Call once per engine, after it is created; it creates
// the two singletons if needed.
namespace Feedback {
void wire(QQmlEngine *engine);
}
