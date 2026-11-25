#include "history.h"

#include "medialibrary.h"

#include "appregistry.h"
#include "ids.h"
#include "itemresolver.h"
#include "jsonfile.h"
#include "library.h"
#include "profileservice.h"

#include <QJsonObject>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <algorithm>

Q_LOGGING_CATEGORY(lcHistory, "mightyos.history", QtWarningMsg)

History::History(ProfileService *profiles, ItemResolver *resolver, QObject *parent)
    : QObject(parent)
    , m_profiles(profiles)
    , m_resolver(resolver)
{
    if (profiles)
        connect(profiles, &ProfileService::activeChanged, this, &History::switchProfile);
    if (resolver)
        connect(resolver, &ItemResolver::changed, this, &History::rebuildRecent);
    switchProfile();
}

History *History::create(QQmlEngine *qmlEngine, QJSEngine *)
{
    auto *profiles = qmlEngine->singletonInstance<ProfileService *>("MightyOS.Core", "ProfileService");
    auto *library = qmlEngine->singletonInstance<Library *>("MightyOS.Core", "Library");
    auto *registry = qmlEngine->singletonInstance<AppRegistry *>("MightyOS.Core", "AppRegistry");
    auto *media = qmlEngine->singletonInstance<MediaLibrary *>("MightyOS.Core", "Media");
    auto *resolver = new CatalogResolver(library, registry, media);
    auto *history = new History(profiles, resolver);
    resolver->setParent(history);
    return history;
}

QString History::filePath() const
{
    if (!m_profiles)
        return {};
    const QString dir = m_profiles->profileStateDir(m_profileId);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/history.json");
}

void History::switchProfile()
{
    const QString id = m_profiles ? m_profiles->activeId() : QString();
    if (m_loaded && id == m_profileId)
        return;
    m_loaded = true;
    m_profileId = id;
    m_entries.clear();

    JsonFile::ReadStatus status {};
    const auto root = JsonFile::read(filePath(), kMaxFileBytes, &status);
    if (root && root->value(QStringLiteral("version")).toInt(-1) == kVersion) {
        const QJsonObject items = root->value(QStringLiteral("items")).toObject();
        for (auto it = items.constBegin(); it != items.constEnd() && m_entries.size() < kMaxEntries; ++it) {
            if (!Ids::isValidItemKey(it.key()))
                continue;
            const QJsonObject o = it.value().toObject();
            Entry e;
            e.lastPlayed = QDateTime::fromString(o.value(QStringLiteral("lastPlayed")).toString(), Qt::ISODateWithMs);
            e.count = qBound(0, o.value(QStringLiteral("count")).toInt(), 1000000);
            if (e.lastPlayed.isValid() && e.count > 0)
                m_entries.insert(it.key(), e);
        }
    } else if (status == JsonFile::ReadStatus::Invalid || root) {
        // Keep the file for the user instead of overwriting it with the
        // next change: unreadable/malformed -> .corrupt, another format
        // version (e.g. from a newer MightyOS) -> .unsupported.
        const QString suffix = root ? QStringLiteral(".unsupported") : QStringLiteral(".corrupt");
        qCWarning(lcHistory) << "ignoring unusable history of profile" << m_profileId << "- kept as" << suffix;
        JsonFile::moveAside(filePath(), suffix);
    }
    rebuildRecent();
}

void History::rebuildRecent()
{
    QList<QPair<QString, QDateTime>> order;
    order.reserve(m_entries.size());
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        order.append({ it.key(), it->lastPlayed });
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    QList<TileItem> items;
    for (const auto &[key, when] : std::as_const(order)) {
        if (items.size() >= kRecentCount)
            break;
        if (!m_resolver)
            break;
        if (auto item = m_resolver->resolve(key))
            items.append(*item);   // gone targets simply drop out of Recent
    }
    m_recent.setItems(items);
}

bool History::save() const
{
    QJsonObject items;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        items.insert(it.key(), QJsonObject {
            { QStringLiteral("lastPlayed"), it->lastPlayed.toUTC().toString(Qt::ISODateWithMs) },
            { QStringLiteral("count"), it->count },
        });
    }
    const QJsonObject root { { QStringLiteral("version"), kVersion }, { QStringLiteral("items"), items } };
    if (!JsonFile::write(filePath(), root)) {
        qCWarning(lcHistory) << "could not save history of profile" << m_profileId;
        return false;
    }
    return true;
}

bool History::record(const QString &key)
{
    if (!Ids::isValidItemKey(key) || filePath().isEmpty())
        return false;
    // Strictly increasing timestamps, so the newest launch is always first.
    QDateTime now = QDateTime::currentDateTimeUtc();
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.lastPlayed >= now)
            now = e.lastPlayed.addMSecs(1);
    }
    Entry &entry = m_entries[key];
    entry.lastPlayed = now;
    entry.count = qMin(entry.count + 1, 1000000);

    while (m_entries.size() > kMaxEntries) {
        auto oldest = m_entries.begin();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->lastPlayed < oldest->lastPlayed)
                oldest = it;
        }
        m_entries.erase(oldest);
    }
    const bool saved = save();
    rebuildRecent();
    emit recorded(key);
    return saved;
}

qint64 History::lastPlayed(const QString &key) const
{
    const auto found = m_entries.constFind(key);
    return found == m_entries.cend() ? 0 : found->lastPlayed.toMSecsSinceEpoch();
}

int History::launchCount(const QString &key) const
{
    const auto found = m_entries.constFind(key);
    return found == m_entries.cend() ? 0 : found->count;
}
