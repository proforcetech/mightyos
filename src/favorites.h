#pragma once

#include "tilelistmodel.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class ItemResolver;
class ProfileService;
class QJSEngine;
class QQmlEngine;

// The active profile's favorites (PLAN D13: per profile).
//
// File: $XDG_CONFIG_HOME/mightyos/profiles/<id>/favorites.json
//   { "version": 1, "items": ["rom:snes:...", "desktop:vim", "snes"],
//     "labels": { "<key>": "<title when added>" } }
// `items` is the saved order. `labels` is display-only: it names an
// "Unavailable" tile after its target is gone (uninstalled app, deleted
// ROM). Keys are never used as paths; they must pass Ids::isValidItemKey().
//
// `model` shows the favorites in order, resolved through ItemResolver; a
// key that no longer resolves is a greyed tile (available: false) that can
// be removed. The model is updated incrementally (insert/remove/move), so
// focus stays on the moved tile. Follows ProfileService.activeId.
//
// Tiles bind their star badge to isFavorite(key) + `revision`.
class Favorites : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(TileListModel *model READ model CONSTANT FINAL)
    Q_PROPERTY(int count READ count NOTIFY changed FINAL)
    Q_PROPERTY(int revision READ revision NOTIFY changed FINAL)

public:
    static constexpr int kVersion = 1;
    static constexpr int kMaxItems = 500;
    static constexpr qint64 kMaxFileBytes = 256 * 1024;
    static constexpr int kMaxLabelLength = 100;

    Favorites(ProfileService *profiles, ItemResolver *resolver, QObject *parent = nullptr);
    static Favorites *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    TileListModel *model() { return &m_model; }
    int count() const { return static_cast<int>(m_ids.size()); }
    int revision() const { return m_revision; }

    Q_INVOKABLE bool isFavorite(const QString &key) const { return m_set.contains(key); }
    // Returns the new state (true = now a favorite). Invalid keys: false.
    Q_INVOKABLE bool toggle(const QString &key);
    Q_INVOKABLE bool add(const QString &key);
    Q_INVOKABLE bool remove(const QString &key);
    // Moves by `delta` places (-1 left / +1 right). False at the ends.
    Q_INVOKABLE bool move(const QString &key, int delta);
    Q_INVOKABLE int indexOf(const QString &key) const { return static_cast<int>(m_ids.indexOf(key)); }
    Q_INVOKABLE QStringList ids() const { return m_ids; }

    QString profileId() const { return m_profileId; }
    QString filePath() const;

signals:
    void changed();

private:
    void switchProfile();
    void refresh();
    bool save();
    TileItem tileFor(const QString &key) const;
    void bump();

    QPointer<ProfileService> m_profiles;
    QPointer<ItemResolver> m_resolver;
    QString m_profileId;
    QStringList m_ids;
    QSet<QString> m_set;
    QHash<QString, QString> m_labels;
    TileListModel m_model;
    int m_revision = 0;
    bool m_loaded = false;
};
