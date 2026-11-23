#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// NavState persists the navigation focus path, so MightyOS comes back to the
// same tile after a restart (PLAN.md section 3, "survives restarts").
//
// The path is a list of entries, outermost screen first:
//   { screen: "games", params: { system: "snes" },
//     row: 0, index: 23, key: "rom:snes:...", extra: { ... } }
// `screen` names a screen type known to the QML NavStack, `params` are its
// construction parameters, row/index locate the focused tile and `key` is
// the focused item's stable id (preferred over index once real models can
// reorder). `extra` is optional, screen-specific state.
//
// File: $XDG_STATE_HOME/mightyos/navstate.json (default
// ~/.local/state/mightyos/navstate.json). Profiles (M2) get
// .../mightyos/profiles/<profile-id>/navstate.json; pass the id to load/save.
//
// Robustness: an unreadable, oversized, malformed or wrong-version file
// yields an empty path. The path is truncated at the first malformed entry,
// because every later entry depends on the ones before it. Semantic checks
// (unknown screen, missing system, index out of range) happen in QML.
class NavState : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Base directory; defaults to defaultDirectory(). Tests override it.
    Q_PROPERTY(QString directory READ directory WRITE setDirectory NOTIFY directoryChanged FINAL)

public:
    static constexpr int kVersion = 1;
    static constexpr int kMaxEntries = 16;
    static constexpr qint64 kMaxFileBytes = 64 * 1024;
    static constexpr int kSaveDelayMs = 400;

    explicit NavState(QObject *parent = nullptr);
    ~NavState() override;

    QString directory() const { return m_directory; }
    void setDirectory(const QString &directory);

    static QString defaultDirectory();
    static bool isValidProfileId(const QString &profileId);

    // Empty string for an invalid profile id.
    QString filePath(const QString &profileId = QString()) const;

    // Synchronous; the file is tiny. Returns an empty list on any problem.
    Q_INVOKABLE QVariantList load(const QString &profileId = QString()) const;
    // Atomic write (QSaveFile). Returns false on failure. Cancels a pending
    // saveLater() for the same profile (this write is newer).
    Q_INVOKABLE bool save(const QVariantList &path, const QString &profileId = QString());
    // Debounced save: coalesces bursts of focus moves (key repeat) into one
    // write kSaveDelayMs after the last change.
    Q_INVOKABLE void saveLater(const QVariantList &path, const QString &profileId = QString());
    // Writes a pending saveLater() now. Called on quit.
    Q_INVOKABLE void flush();
    // Drops a pending saveLater() for `profileId` without writing it (the
    // profile was deleted: a late write would recreate its directory).
    Q_INVOKABLE void discard(const QString &profileId);

    // Exposed for tests: validates and normalises a JSON path array.
    static QVariantList sanitize(const QJsonArray &path);

signals:
    void directoryChanged();

private:
    QString m_directory;
    QTimer m_saveTimer;
    QVariantList m_pendingPath;
    QString m_pendingProfile;
    bool m_pending = false;
};
