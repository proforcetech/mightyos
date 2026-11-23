#include "library.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QRegularExpression>
#include <algorithm>

Q_LOGGING_CATEGORY(lcLibrary, "mightyos.library", QtWarningMsg)

namespace {

bool isValidSystemId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,63}$"));
    return re.match(id).hasMatch();
}

bool isValidColour(const QString &colour)
{
    static const QRegularExpression re(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return re.match(colour).hasMatch();
}

} // namespace

Library::Library(QObject *parent)
    : QObject(parent)
{
    if (!loadFile(kBundledPath))
        qCWarning(lcLibrary) << "bundled mock library missing or invalid";
}

bool Library::loadFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFileBytes)
        return false;
    QJsonParseError error {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(kMaxFileBytes), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()
        || doc.object().value(QStringLiteral("version")).toInt() != 1)
        return false;
    const QJsonObject root = doc.object();

    QList<System> systems;
    QHash<QString, int> systemIndex;
    for (const QJsonValue &value : root.value(QStringLiteral("systems")).toArray()) {
        const QJsonObject o = value.toObject();
        System s {
            o.value(QStringLiteral("id")).toString(),
            o.value(QStringLiteral("label")).toString(),
            o.value(QStringLiteral("fullName")).toString(),
            o.value(QStringLiteral("category")).toString(),
            o.value(QStringLiteral("icon")).toString(),
            o.value(QStringLiteral("tint")).toString(),
        };
        if (!isValidSystemId(s.id) || s.label.isEmpty() || systemIndex.contains(s.id))
            continue;
        if (!isValidColour(s.tint))
            s.tint.clear();
        systemIndex.insert(s.id, static_cast<int>(systems.size()));
        systems.append(s);
    }

    QHash<QString, QList<Game>> games;
    QHash<QString, QPair<QString, int>> gameIndex;
    const QJsonObject gameRoot = root.value(QStringLiteral("games")).toObject();
    for (const System &s : std::as_const(systems)) {
        QList<Game> list;
        for (const QJsonValue &value : gameRoot.value(s.id).toArray()) {
            const QJsonObject o = value.toObject();
            Game g { o.value(QStringLiteral("id")).toString(), o.value(QStringLiteral("title")).toString(),
                     s.id, o.value(QStringLiteral("year")).toInt() };
            if (g.id.isEmpty() || g.title.isEmpty() || gameIndex.contains(g.id))
                continue;
            gameIndex.insert(g.id, {});
            list.append(g);
        }
        std::stable_sort(list.begin(), list.end(), [](const Game &a, const Game &b) {
            return TileListModel::sortLess(a.title, b.title);
        });
        for (int i = 0; i < list.size(); ++i)
            gameIndex.insert(list.at(i).id, { s.id, i });
        games.insert(s.id, list);
    }

    m_systems = systems;
    m_systemIndex = systemIndex;
    m_games = games;
    m_gameIndex = gameIndex;

    // Refresh models handed out earlier (screens keep their pointers).
    for (auto it = m_models.cbegin(); it != m_models.cend(); ++it) {
        const QString &cacheKey = it.key();
        QList<TileItem> items;
        if (cacheKey.startsWith(QLatin1String("systems:"))) {
            const QString category = cacheKey.mid(8);
            for (const System &s : std::as_const(m_systems)) {
                if (s.category == category)
                    items.append(systemTile(s));
            }
        } else {
            const QString systemId = cacheKey.mid(6);
            if (m_systemIndex.contains(systemId)) {
                const System &s = m_systems.at(m_systemIndex.value(systemId));
                for (const Game &g : m_games.value(systemId))
                    items.append(gameTile(g, s));
            }
        }
        it.value()->setItems(items);
    }
    emit changed();
    return true;
}

TileItem Library::systemTile(const System &system) const
{
    TileItem item;
    item.key = system.id;
    item.title = system.label;
    item.subtitle = system.fullName;
    item.icon = system.icon;
    item.tint = system.tint;
    item.kind = QStringLiteral("system");
    return item;
}

TileItem Library::gameTile(const Game &game, const System &system) const
{
    TileItem item;
    item.key = game.id;
    item.title = game.title;
    item.subtitle = game.year > 0 ? QString::number(game.year) : QString();
    item.icon = system.icon;
    item.tint = system.tint;
    item.kind = QStringLiteral("game");
    return item;
}

TileListModel *Library::cachedModel(const QString &cacheKey)
{
    TileListModel *model = m_models.value(cacheKey);
    if (!model) {
        model = new TileListModel(this);
        QQmlEngine::setObjectOwnership(model, QQmlEngine::CppOwnership);
        m_models.insert(cacheKey, model);
    }
    return model;
}

TileListModel *Library::systems(const QString &category)
{
    TileListModel *model = cachedModel(QStringLiteral("systems:") + category);
    if (model->count() == 0) {
        QList<TileItem> items;
        for (const System &s : std::as_const(m_systems)) {
            if (s.category == category)
                items.append(systemTile(s));
        }
        model->setItems(items);
    }
    return model;
}

TileListModel *Library::games(const QString &systemId)
{
    if (!m_systemIndex.contains(systemId))
        return nullptr;
    TileListModel *model = cachedModel(QStringLiteral("games:") + systemId);
    if (model->count() == 0) {
        const System &s = m_systems.at(m_systemIndex.value(systemId));
        QList<TileItem> items;
        for (const Game &g : m_games.value(systemId))
            items.append(gameTile(g, s));
        model->setItems(items);
    }
    return model;
}

bool Library::hasSystem(const QString &systemId) const
{
    return m_systemIndex.contains(systemId);
}

QVariantMap Library::system(const QString &systemId) const
{
    if (!m_systemIndex.contains(systemId))
        return {};
    const System &s = m_systems.at(m_systemIndex.value(systemId));
    return {
        { QStringLiteral("key"), s.id }, { QStringLiteral("title"), s.label },
        { QStringLiteral("fullName"), s.fullName }, { QStringLiteral("category"), s.category },
        { QStringLiteral("icon"), s.icon }, { QStringLiteral("tint"), s.tint },
    };
}

QString Library::adjacentSystem(const QString &systemId, int delta) const
{
    if (!m_systemIndex.contains(systemId))
        return {};
    const QString category = m_systems.at(m_systemIndex.value(systemId)).category;
    QStringList peers;
    for (const System &s : m_systems) {
        if (s.category == category)
            peers.append(s.id);
    }
    const qsizetype next = peers.indexOf(systemId) + delta;
    return (next >= 0 && next < peers.size()) ? peers.at(next) : QString();
}

std::optional<TileItem> Library::resolve(const QString &key) const
{
    if (m_systemIndex.contains(key))
        return systemTile(m_systems.at(m_systemIndex.value(key)));
    const auto found = m_gameIndex.constFind(key);
    if (found == m_gameIndex.cend())
        return std::nullopt;
    const System &s = m_systems.at(m_systemIndex.value(found->first));
    TileItem item = gameTile(m_games.value(found->first).at(found->second), s);
    item.subtitle = s.label;
    return item;
}
