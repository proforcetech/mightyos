#include "feedback.h"

#include "inputrouter.h"
#include "soundfx.h"

#include <QQmlEngine>

void Feedback::wire(QQmlEngine *engine)
{
    auto *sounds = engine->singletonInstance<SoundFx *>("MightyOS.Core", "SoundFx");
    auto *router = engine->singletonInstance<InputRouter *>("MightyOS.Core", "InputRouter");
    if (!sounds || !router)
        return;
    QObject::connect(sounds, &SoundFx::played, router, [router](SoundFx::Sound sound) {
        if (sound == SoundFx::Sound::Bump)
            router->rumble(InputRouter::Feedback::Bump);
        else if (sound == SoundFx::Sound::Error)
            router->rumble(InputRouter::Feedback::Error);
    });
}
