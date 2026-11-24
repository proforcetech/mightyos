#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// User profiles (PLAN D13). Decides which profile is active; per-profile
// data lives in .../mightyos/profiles/<id>/ under the config and state dirs
// (favorites.json, emulators.json, allowed.json in config; history.json,
// navstate.json and generated emulator configs in state) and game saves
// under the data dir (profiles/<id>/saves/<system>; the Flatpak RetroArch
// keeps its per-profile saves inside its own sandbox dir,
// ~/.var/app/org.libretro.RetroArch/data/mightyos/profiles/<id>).
//
// File: $XDG_CONFIG_HOME/mightyos/profiles.json
//   { "version": 1, "profiles": [ { "id", "name", "avatar", "color",
//     "kind": "standard" | "kids", "createdAt", "lastUsed" } ] }
// Timestamps are ISO 8601 UTC with milliseconds.
//
// Rules:
//  - ids match ^[A-Za-z0-9_-]{1,64}$ (they are path components); new ones
//    are random ("p" + 10 hex digits); the first-run profile is "default".
//  - names are 1..kMaxNameLength printable characters, unique
//    (case-insensitive); avatars and colours come from the built-in sets.
//  - the last profile can't be deleted; at most kMaxProfiles.
//  - every write is atomic (QSaveFile); a failed write leaves memory and
//    disk unchanged and the call returns false.
//  - a corrupt file is moved aside to profiles.json.corrupt and a fresh
//    default profile is created, so MightyOS always starts.
//  - startup: the most recently used profile is active; the picker is
//    shown at startup only with 2+ profiles (pickerAtStartup).
//  - kind "kids" is stored and shown, not enforced (enforcement is M8).
class ProfileService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString activeId READ activeId NOTIFY activeChanged FINAL)
    Q_PROPERTY(QVariantMap active READ active NOTIFY activeChanged FINAL)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY profilesChanged FINAL)
    Q_PROPERTY(bool pickerAtStartup READ pickerAtStartup NOTIFY profilesChanged FINAL)
    Q_PROPERTY(QStringList avatars READ avatars CONSTANT FINAL)
    Q_PROPERTY(QStringList colors READ colors CONSTANT FINAL)
    Q_PROPERTY(int maxProfiles READ maxProfiles CONSTANT FINAL)

public:
    static constexpr int kVersion = 1;
    static constexpr int kMaxProfiles = 16;
    static constexpr int kMaxNameLength = 24;
    static constexpr qint64 kMaxFileBytes = 64 * 1024;
    static inline const QString kDefaultId = QStringLiteral("default");

    struct Profile {
        QString id, name, avatar, color, kind;
        QDateTime createdAt, lastUsed;
        bool operator==(const Profile &) const = default;
    };

    // $XDG_CONFIG_HOME/mightyos and $XDG_STATE_HOME/mightyos.
    explicit ProfileService(QObject *parent = nullptr);
    ProfileService(const QString &configDir, const QString &stateDir, QObject *parent = nullptr);
    // `dataDir` ($XDG_DATA_HOME/mightyos) and `home` locate the saves; empty
    // means "no saves to manage".
    ProfileService(const QString &configDir, const QString &stateDir, const QString &dataDir,
                   const QString &home, QObject *parent = nullptr);

    QString activeId() const { return m_activeId; }
    QVariantMap active() const { return profile(m_activeId); }
    QVariantList profiles() const;
    int count() const { return static_cast<int>(m_profiles.size()); }
    bool pickerAtStartup() const { return m_profiles.size() >= 2; }
    static QStringList avatars();
    static QStringList colors();
    int maxProfiles() const { return kMaxProfiles; }

    // Returns the new id, or "" (invalid input, limit reached, write failed).
    Q_INVOKABLE QString create(const QString &name, const QString &avatar, const QString &color,
                               const QString &kind = QStringLiteral("standard"));
    Q_INVOKABLE bool rename(const QString &id, const QString &name);
    Q_INVOKABLE bool setAppearance(const QString &id, const QString &avatar, const QString &color);
    Q_INVOKABLE bool setKind(const QString &id, const QString &kind);
    // All editable fields in one atomic write (the profile editor's Save):
    // either everything is applied or nothing is.
    Q_INVOKABLE bool update(const QString &id, const QString &name, const QString &avatar,
                            const QString &color, const QString &kind);
    // Deletes the profile and its per-profile config and state dirs. Game
    // saves are kept unless `keepSaves` is false (the UI asks; keeping is
    // the default). Never the last profile, and nothing at all if mightyos/
    // or profiles/ (in any of our dirs) is a symlink: the deletion would
    // follow it out of our tree. If it was active, the most recently used
    // remaining profile becomes active.
    Q_INVOKABLE bool remove(const QString &id, bool keepSaves = true);
    // True if the profile has any saved games (a non-empty saves dir).
    Q_INVOKABLE bool hasSaves(const QString &id) const;
    // Where its saves live (display: "~/..."), for the delete dialog.
    Q_INVOKABLE QString savesLocation(const QString &id) const;
    // Makes `id` active and stamps lastUsed.
    Q_INVOKABLE bool select(const QString &id);
    Q_INVOKABLE QVariantMap profile(const QString &id) const;
    Q_INVOKABLE bool contains(const QString &id) const { return indexOf(id) >= 0; }
    // Up to `count` friendly names not used by any profile.
    Q_INVOKABLE QStringList suggestedNames(int count) const;
    // Re-reads profiles.json (tests; external edits).
    Q_INVOKABLE void reload();

    QString configDirectory() const { return m_configDir; }
    QString stateDirectory() const { return m_stateDir; }
    QString filePath() const;
    // Empty for an invalid id.
    QString profileConfigDir(const QString &id) const;
    QString profileStateDir(const QString &id) const;
    QString profileDataDir(const QString &id) const;
    // Every directory that holds saves of `id` (existing or not).
    QStringList savesDirs(const QString &id) const;

    static QString normalizedName(const QString &name);
    static bool isValidName(const QString &name);
    static bool isValidKind(const QString &kind);

signals:
    void activeChanged();
    void profilesChanged();
    void profileRemoved(const QString &id);

private:
    int indexOf(const QString &id) const;
    bool nameTaken(const QString &name, const QString &exceptId) const;
    bool load();
    bool write(const QList<Profile> &profiles) const;
    // Swaps in `next` if it could be written. Emits profilesChanged.
    bool commit(const QList<Profile> &next);
    void createDefault();
    void pickMostRecent();
    QDateTime nextTimestamp() const;
    QString newId() const;
    bool dataDirsAreSafe() const;

    bool removeTree(const QString &dir) const;

    QString m_configDir;
    QString m_stateDir;
    QString m_dataDir;
    QString m_home;
    QList<Profile> m_profiles;
    QString m_activeId;
};
