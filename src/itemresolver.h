#pragma once

#include "tilelistmodel.h"

#include <QObject>
#include <QPointer>
#include <optional>

class Library;
class AppRegistry;
class MediaLibrary;

// Maps an item key (game, app or system id) to the tile that shows it.
// Favorites and History store keys only and resolve them through this, so
// an uninstalled app or a removed ROM becomes an "Unavailable" tile.
class ItemResolver : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // nullopt if the key no longer resolves.
    virtual std::optional<TileItem> resolve(const QString &key) const = 0;
    // True if `key` doesn't resolve *yet* (its source hasn't finished
    // loading), so it must not be shown as unavailable.
    virtual bool isPending(const QString &key) const { Q_UNUSED(key); return false; }

signals:
    // The set of resolvable items may have changed (app re-scan, ...).
    void changed();
};

// The real resolver: AppRegistry (apps; for kids profiles only those
// Library::itemAllowed() lists), MediaLibrary (web apps, Moonlight) and
// Library (games, systems; mock or scanned).
class CatalogResolver : public ItemResolver
{
    Q_OBJECT

public:
    CatalogResolver(Library *library, AppRegistry *registry, MediaLibrary *media = nullptr,
                    QObject *parent = nullptr);

    std::optional<TileItem> resolve(const QString &key) const override;
    // App keys before AppRegistry's first real scan.
    bool isPending(const QString &key) const override;

private:
    QPointer<Library> m_library;
    QPointer<AppRegistry> m_registry;
    QPointer<MediaLibrary> m_media;
};
