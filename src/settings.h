#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

// Global (not per-profile) settings, PLAN section 2.
//
// File: $XDG_CONFIG_HOME/mightyos/settings.json
//   { "version": 1, "rumble": true, "tvRemote": "navigation" }
//
// Each setting is a typed property with a default; unknown keys in the file
// are kept on write (a newer MightyOS may have added them). Writes are
// atomic; a failed write keeps the old value in memory and on disk and the
// setter emits writeFailed().
//
// Unusable files:
//  - not JSON / not an object / no valid version: moved aside to
//    settings.json.corrupt, defaults apply, writable.
//  - a newer format version: the known keys are read, the file is left
//    untouched and never written (writable == false).
//  - unreadable (EACCES, EIO, not a regular file, too large): defaults,
//    file untouched, not written.
class Settings : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Short controller rumble on edge bumps and error toasts (default on).
    Q_PROPERTY(bool rumble READ rumble WRITE setRumble NOTIFY rumbleChanged FINAL)
    // HDMI-CEC TV remote trust level (PLAN D18): "off", "navigation"
    // (default: move, OK, Back, page, category, Home) or "full".
    Q_PROPERTY(QString tvRemote READ tvRemote WRITE setTvRemote NOTIFY tvRemoteChanged FINAL)
    Q_PROPERTY(bool writable READ isWritable NOTIFY writableChanged FINAL)

public:
    static constexpr int kVersion = 1;
    static constexpr qint64 kMaxFileBytes = 64 * 1024;
    static QStringList tvRemoteModes();

    // $XDG_CONFIG_HOME/mightyos
    explicit Settings(QObject *parent = nullptr);
    Settings(const QString &configDir, QObject *parent = nullptr);

    QString filePath() const;
    bool isWritable() const { return m_writable; }

    bool rumble() const { return m_rumble; }
    void setRumble(bool rumble);
    QString tvRemote() const { return m_tvRemote; }
    void setTvRemote(const QString &mode);

    // Re-reads the file (tests).
    Q_INVOKABLE void reload();

signals:
    void rumbleChanged();
    void tvRemoteChanged();
    void writableChanged();
    void writeFailed();

private:
    bool writeKey(const QString &key, const QJsonValue &value);
    void applyRoot();
    void setWritable(bool writable);

    QString m_configDir;
    QJsonObject m_root;
    bool m_writable = true;
    bool m_rumble = true;
    QString m_tvRemote = QStringLiteral("navigation");
};
