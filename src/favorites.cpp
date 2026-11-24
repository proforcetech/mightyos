#include "favorites.h"

#include "appregistry.h"
#include "ids.h"
#include "itemresolver.h"
#include "jsonfile.h"
#include "library.h"
#include "profileservice.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QQmlEngine>

Q_LOGGING_CATEGORY(lcFavorites, "mightyos.favorites", QtWarningMsg)

namespace {

// Kind (and fallback icon) of a key whose target is gone, from its prefix.
QString kindForKey(const QString &key)
{
    if (key.startsWith(QLatin1String("rom:")) || key.startsWith(QLatin1String("steam:")))
        return QStringLiteral("game");
    if (key.contains(QLatin1Char(':')))
        return QStringLiteral("app");
    return QStringLiteral("system");
}

QString iconForKind(const QString &kind)
{
    if (kind == QLatin1String("game"))
        return QStringLiteral("games");
    if (kind == QLatin1String("app"))
        return QStringLiteral("apps");
    return QStringLiteral("emulators");
}

} // namespace

// ---------------------------------------------------------------------------

CatalogResolver::CatalogResolver(Library *library, AppRegistry *registry, QObject *parent)
    : ItemResolver(parent)
    , m_library(library)
    , m_registry(registry)
{
    if (library)
        connect(library, &Library::changed, this, &ItemResolver::changed);
    if (registry) {
        connect(registry, &AppRegistry::appsChanged, this, &ItemResolver::changed);
        connect(registry, &AppRegistry::readyChanged, this, &ItemResolver::changed);
    }
}

bool CatalogResolver::isPending(const QString &key) const
{
    if (m_library && m_library->isPending(key))
        return true;   // ROMs before the cache loaded, Steam before the first scan
    return m_registry && !m_registry->isReady()
           && (key.startsWith(QLatin1String("app:")) || key.startsWith(QLatin1String("flatpak:"))
               || key.startsWith(QLatin1String("desktop:")));
}

std::optional<TileItem> CatalogResolver::resolve(const QString &key) const
{
    if (m_registry) {
        if (auto app = m_registry->resolve(key))
            return app;
    }
    if (m_library)
        return m_library->resolve(key);
    return std::nullopt;
}

// ---------------------------------------------------------------------------

Favorites::Favorites(ProfileService *profiles, ItemResolver *resolver, QObject *parent)
    : QObject(parent)
    , m_profiles(profiles)
    , m_resolver(resolver)
{
    if (profiles)
        connect(profiles, &ProfileService::activeChanged, this, &Favorites::switchProfile);
    if (resolver)
        connect(resolver, &ItemResolver::changed, this, &Favorites::refresh);
    switchProfile();
}

Favorites *Favorites::create(QQmlEngine *qmlEngine, QJSEngine *)
{
    auto *profiles = qmlEngine->singletonInstance<ProfileService *>("MightyOS.Core", "ProfileService");
    auto *library = qmlEngine->singletonInstance<Library *>("MightyOS.Core", "Library");
    auto *registry = qmlEngine->singletonInstance<AppRegistry *>("MightyOS.Core", "AppRegistry");
    auto *resolver = new CatalogResolver(library, registry);
    auto *favorites = new Favorites(profiles, resolver);
    resolver->setParent(favorites);
    return favorites;
}

QString Favorites::filePath() const
{
    if (!m_profiles)
        return {};
    const QString dir = m_profiles->profileConfigDir(m_profileId);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/favorites.json");
}

void Favorites::switchProfile()
{
    const QString id = m_profiles ? m_profiles->activeId() : QString();
    if (m_loaded && id == m_profileId)
        return;   // e.g. activeChanged after a rename
    m_loaded = true;
    m_profileId = id;
    m_ids.clear();
    m_set.clear();
    m_labels.clear();

    JsonFile::ReadStatus status {};
    const auto root = JsonFile::read(filePath(), kMaxFileBytes, &status);
    if (root && root->value(QStringLiteral("version")).toInt(-1) == kVersion) {
        const QJsonObject labels = root->value(QStringLiteral("labels")).toObject();
        for (const QJsonValue &value : root->value(QStringLiteral("items")).toArray()) {
            const QString key = value.toString();
            if (m_ids.size() >= kMaxItems || !Ids::isValidItemKey(key) || m_set.contains(key))
                continue;
            m_ids.append(key);
            m_set.insert(key);
            const QString label = labels.value(key).toString().simplified().left(kMaxLabelLength);
            if (!label.isEmpty())
                m_labels.insert(key, label);
        }
    } else if (status == JsonFile::ReadStatus::Invalid || root) {
        // Keep the file for the user instead of overwriting it with the
        // next change: unreadable/malformed -> .corrupt, another format
        // version (e.g. from a newer MightyOS) -> .unsupported.
        const QString suffix = root ? QStringLiteral(".unsupported") : QStringLiteral(".corrupt");
        qCWarning(lcFavorites) << "ignoring unusable favorites of profile" << m_profileId << "- kept as" << suffix;
        JsonFile::moveAside(filePath(), suffix);
    }

    QList<TileItem> items;
    for (const QString &key : std::as_const(m_ids))
        items.append(tileFor(key));
    m_model.setItems(items);
    bump();
}

void Favorites::refresh()
{
    QList<TileItem> items;
    for (const QString &key : std::as_const(m_ids))
        items.append(tileFor(key));
    m_model.setItems(items);   // same keys: dataChanged only, focus is kept
}

TileItem Favorites::tileFor(const QString &key) const
{
    if (m_resolver) {
        if (auto item = m_resolver->resolve(key))
            return *item;
    }
    TileItem item;
    item.key = key;
    item.kind = kindForKey(key);
    item.title = m_labels.value(key, tr("Unknown item"));
    item.icon = iconForKind(item.kind);
    item.tint = QStringLiteral("#5a6274");
    if (m_resolver && m_resolver->isPending(key)) {
        // Apps not scanned yet: not "Unavailable", just not here yet.
        item.subtitle = key.startsWith(QLatin1String("rom:")) || key.startsWith(QLatin1String("steam:"))
            ? tr("Looking for games…") : tr("Looking for apps…");
        item.available = true;
        return item;
    }
    item.available = false;
    return item;
}

bool Favorites::save()
{
    QJsonArray items;
    QJsonObject labels;
    for (const QString &key : std::as_const(m_ids)) {
        items.append(key);
        if (m_labels.contains(key))
            labels.insert(key, m_labels.value(key));
    }
    const QJsonObject root { { QStringLiteral("version"), kVersion },
                             { QStringLiteral("items"), items },
                             { QStringLiteral("labels"), labels } };
    if (!JsonFile::write(filePath(), root)) {
        qCWarning(lcFavorites) << "could not save favorites of profile" << m_profileId;
        return false;
    }
    return true;
}

void Favorites::bump()
{
    ++m_revision;
    emit changed();
}

bool Favorites::add(const QString &key)
{
    if (!Ids::isValidItemKey(key) || m_set.contains(key) || m_ids.size() >= kMaxItems || filePath().isEmpty())
        return false;
    const TileItem tile = tileFor(key);
    m_ids.append(key);
    m_set.insert(key);
    if (tile.available)
        m_labels.insert(key, tile.title.left(kMaxLabelLength));
    if (!save()) {
        m_ids.removeLast();
        m_set.remove(key);
        m_labels.remove(key);
        return false;
    }
    m_model.insertItem(m_model.count(), tile);
    bump();
    return true;
}

bool Favorites::remove(const QString &key)
{
    const qsizetype i = m_ids.indexOf(key);
    if (i < 0)
        return false;
    const QString label = m_labels.value(key);
    m_ids.removeAt(i);
    m_set.remove(key);
    m_labels.remove(key);
    if (!save()) {
        m_ids.insert(i, key);
        m_set.insert(key);
        if (!label.isEmpty())
            m_labels.insert(key, label);
        return false;
    }
    m_model.removeAt(m_model.indexOfKey(key));
    bump();
    return true;
}

bool Favorites::toggle(const QString &key)
{
    if (m_set.contains(key))
        return !remove(key);
    return add(key);
}

bool Favorites::move(const QString &key, int delta)
{
    const qsizetype from = m_ids.indexOf(key);
    const qsizetype to = from + delta;
    if (from < 0 || delta == 0 || to < 0 || to >= m_ids.size())
        return false;
    m_ids.move(from, to);
    if (!save()) {
        m_ids.move(to, from);
        return false;
    }
    m_model.moveItem(static_cast<int>(from), static_cast<int>(to));
    bump();
    return true;
}
