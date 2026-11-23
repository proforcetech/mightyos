#pragma once

#include "tilelistmodel.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

class ItemResolver;
class ProfileService;
class QJSEngine;
class QQmlEngine;

// The active profile's launch history.
//
// File: $XDG_STATE_HOME/mightyos/profiles/<id>/history.json
//   { "version": 1, "items": { "<key>": { "lastPlayed": "<ISO UTC>",
//     "count": <launches> } } }
// record() is called on a *successful* launch (Launcher.launched). At most
// kMaxEntries keys are kept (the oldest are dropped).
//
// `recent`: the kRecentCount most recently played items that still resolve,
// newest first (the small "Recent" row on Home > Favorites).
class History : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(TileListModel *recent READ recent CONSTANT FINAL)

public:
    static constexpr int kVersion = 1;
    static constexpr int kMaxEntries = 1000;
    static constexpr int kRecentCount = 8;
    static constexpr qint64 kMaxFileBytes = 512 * 1024;

    History(ProfileService *profiles, ItemResolver *resolver, QObject *parent = nullptr);
    static History *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    TileListModel *recent() { return &m_recent; }

    Q_INVOKABLE bool record(const QString &key);
    // Milliseconds since the epoch, 0 if never played.
    Q_INVOKABLE qint64 lastPlayed(const QString &key) const;
    Q_INVOKABLE int launchCount(const QString &key) const;

    QString profileId() const { return m_profileId; }
    QString filePath() const;

signals:
    void recorded(const QString &key);

private:
    struct Entry {
        QDateTime lastPlayed;
        int count = 0;
    };

    void switchProfile();
    void rebuildRecent();
    bool save() const;

    QPointer<ProfileService> m_profiles;
    QPointer<ItemResolver> m_resolver;
    QString m_profileId;
    bool m_loaded = false;
    QHash<QString, Entry> m_entries;
    TileListModel m_recent;
};
