#include "soundfx.h"

SoundFx::SoundFx(QObject *parent)
    : QObject(parent)
{
}

bool SoundFx::isAvailable() const
{
    return false;   // no QtMultimedia backend yet (see header)
}

void SoundFx::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    emit enabledChanged();
}

void SoundFx::play(Sound sound)
{
    Q_UNUSED(sound)
}
