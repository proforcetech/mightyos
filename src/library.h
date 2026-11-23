#pragma once

#include "tilelistmodel.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QtQml/qqmlregistration.h>
#include <optional>

// Systems and their games (PLAN D12: Emulators -> systems, Games ->
// [Steam]; Steam is a system like any emulator).
//
// M2: backed by a bundled mock JSON (data/mock/library.json, same ids and
// structure as the M1 JS mock). M4 replaces the source with systems.d
// definitions, the RomScanner/SQLite cache and the Steam .acf parser; the
// QML-facing API stays.
//
// Models returned to QML are owned by the Library (C++ ownership) and live
// as long as it does, so screens can hold on to them.
class Library : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static constexpr qint64 kMaxFileBytes = 4 * 1024 * 1024;
    static inline const QString kBundledPath = QStringLiteral(":/mightyos/mock/library.json");

    explicit Library(QObject *parent = nullptr);

    // Replaces the content. Returns false (and keeps the old content) if
    // the file is missing, oversized or malformed.
    bool loadFile(const QString &path);

    // Systems of a Home category ("emulators", "games"); an empty model for
    // anything else.
    Q_INVOKABLE TileListModel *systems(const QString &category);
    // Games of one system, sorted by title; nullptr for an unknown system.
    Q_INVOKABLE TileListModel *games(const QString &systemId);

    Q_INVOKABLE bool hasSystem(const QString &systemId) const;
    // { key, title, fullName, category, icon, tint } or empty.
    Q_INVOKABLE QVariantMap system(const QString &systemId) const;
    // The system `delta` steps away within the same category, or "" at
    // either end / for unknown ids.
    Q_INVOKABLE QString adjacentSystem(const QString &systemId, int delta) const;

    // A game (rom:/steam: key) or a system (bare id). For favorites the
    // game's subtitle is its system's label.
    std::optional<TileItem> resolve(const QString &key) const;

signals:
    void changed();

private:
    struct System {
        QString id, label, fullName, category, icon, tint;
    };
    struct Game {
        QString id, title, system;
        int year = 0;
    };

    TileItem systemTile(const System &system) const;
    TileItem gameTile(const Game &game, const System &system) const;
    TileListModel *cachedModel(const QString &cacheKey);

    QList<System> m_systems;
    QHash<QString, int> m_systemIndex;           // id -> m_systems row
    QHash<QString, QList<Game>> m_games;         // system -> sorted games
    QHash<QString, QPair<QString, int>> m_gameIndex;   // game id -> (system, row)
    QHash<QString, TileListModel *> m_models;    // "systems:<cat>" / "games:<sys>"
};
