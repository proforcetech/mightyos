#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

// UI sound effects (soft click / bump / back).
//
// The intended backend is QtMultimedia's QSoundEffect with short, preloaded
// WAV samples. QtMultimedia is not installed on the M1 dev box, so this is a
// no-op stub with the final API: QML already calls play() at the right
// places, and `available` stays false. Adding the backend later only touches
// soundfx.cpp and CMakeLists.txt (find_package(Qt6 OPTIONAL_COMPONENTS
// Multimedia)).
class SoundFx : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool available READ isAvailable CONSTANT FINAL)
    Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged FINAL)

public:
    enum class Sound { Move, Accept, Back, Bump, Error };
    Q_ENUM(Sound)

    explicit SoundFx(QObject *parent = nullptr);

    bool isAvailable() const;
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled);

    // Cheap to call on every focus move; must never block the UI thread.
    Q_INVOKABLE void play(SoundFx::Sound sound);

signals:
    void enabledChanged();
    void played(SoundFx::Sound sound);

private:
    bool m_enabled = true;
};
