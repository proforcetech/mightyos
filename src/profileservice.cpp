#include "profileservice.h"

#include "ids.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

Q_LOGGING_CATEGORY(lcProfiles, "mightyos.profiles", QtWarningMsg)

namespace {

const QString kStandard = QStringLiteral("standard");
const QString kKids = QStringLiteral("kids");

QString isoUtc(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

QString defaultBase(QStandardPaths::StandardLocation location)
{
    const QString base = QStandardPaths::writableLocation(location);
    return base.isEmpty() ? QString() : base + QStringLiteral("/mightyos");
}

} // namespace

ProfileService::ProfileService(QObject *parent)
    : ProfileService(defaultBase(QStandardPaths::GenericConfigLocation),
                     defaultBase(QStandardPaths::GenericStateLocation), parent)
{
}

ProfileService::ProfileService(const QString &configDir, const QString &stateDir, QObject *parent)
    : QObject(parent)
    , m_configDir(configDir)
    , m_stateDir(stateDir)
{
    if (!load())
        createDefault();
    pickMostRecent();
}

QStringList ProfileService::avatars()
{
    // Each has a vector "avatar-<name>" icon in qml/Icons.js.
    return { QStringLiteral("robot"), QStringLiteral("cat"), QStringLiteral("ghost"),
             QStringLiteral("rocket"), QStringLiteral("alien"), QStringLiteral("bolt"),
             QStringLiteral("heart"), QStringLiteral("crown") };
}

QStringList ProfileService::colors()
{
    return { QStringLiteral("#4da3ff"), QStringLiteral("#ff6b8b"), QStringLiteral("#7bd88f"),
             QStringLiteral("#ffc94d"), QStringLiteral("#b48cff"), QStringLiteral("#ff9f43"),
             QStringLiteral("#2ed3c6"), QStringLiteral("#a0aec0") };
}

QString ProfileService::normalizedName(const QString &name)
{
    return name.simplified();
}

bool ProfileService::isValidName(const QString &name)
{
    const QString n = normalizedName(name);
    if (n.isEmpty() || n.size() > kMaxNameLength)
        return false;
    for (const QChar c : n) {
        if (!c.isPrint())
            return false;
    }
    return true;
}

bool ProfileService::isValidKind(const QString &kind)
{
    return kind == kStandard || kind == kKids;
}

QString ProfileService::filePath() const
{
    return m_configDir.isEmpty() ? QString() : m_configDir + QStringLiteral("/profiles.json");
}

QString ProfileService::profileConfigDir(const QString &id) const
{
    if (m_configDir.isEmpty() || !Ids::isValidProfileId(id))
        return {};
    return m_configDir + QStringLiteral("/profiles/") + id;
}

QString ProfileService::profileStateDir(const QString &id) const
{
    if (m_stateDir.isEmpty() || !Ids::isValidProfileId(id))
        return {};
    return m_stateDir + QStringLiteral("/profiles/") + id;
}

int ProfileService::indexOf(const QString &id) const
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles.at(i).id == id)
            return i;
    }
    return -1;
}

bool ProfileService::nameTaken(const QString &name, const QString &exceptId) const
{
    const QString n = normalizedName(name);
    for (const Profile &p : m_profiles) {
        if (p.id != exceptId && p.name.compare(n, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QVariantMap ProfileService::profile(const QString &id) const
{
    const int i = indexOf(id);
    if (i < 0)
        return {};
    const Profile &p = m_profiles.at(i);
    return {
        { QStringLiteral("id"), p.id }, { QStringLiteral("name"), p.name },
        { QStringLiteral("avatar"), p.avatar }, { QStringLiteral("color"), p.color },
        { QStringLiteral("kind"), p.kind }, { QStringLiteral("createdAt"), isoUtc(p.createdAt) },
        { QStringLiteral("lastUsed"), isoUtc(p.lastUsed) },
    };
}

QVariantList ProfileService::profiles() const
{
    QVariantList list;
    for (const Profile &p : m_profiles)
        list.append(profile(p.id));
    return list;
}

bool ProfileService::load()
{
    const QString path = filePath();
    QFile file(path);
    if (path.isEmpty() || !file.exists())
        return false;

    auto moveAside = [&path](const char *why) {
        qCWarning(lcProfiles) << "profiles.json unusable (" << why << "), moving it aside";
        const QString aside = path + QStringLiteral(".corrupt");
        QFile::remove(aside);
        QFile::rename(path, aside);
        return false;
    };

    if (file.size() > kMaxFileBytes)
        return moveAside("too large");
    if (!file.open(QIODevice::ReadOnly))
        return moveAside("unreadable");
    QJsonParseError error {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(kMaxFileBytes), &error);
    file.close();
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return moveAside("malformed");
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt(-1) != kVersion)
        return moveAside("unknown version");

    QList<Profile> profiles;
    for (const QJsonValue &value : root.value(QStringLiteral("profiles")).toArray()) {
        if (profiles.size() >= kMaxProfiles)
            break;
        const QJsonObject o = value.toObject();
        Profile p;
        p.id = o.value(QStringLiteral("id")).toString();
        p.name = normalizedName(o.value(QStringLiteral("name")).toString());
        p.avatar = o.value(QStringLiteral("avatar")).toString();
        p.color = o.value(QStringLiteral("color")).toString();
        p.kind = o.value(QStringLiteral("kind")).toString();
        p.createdAt = QDateTime::fromString(o.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
        p.lastUsed = QDateTime::fromString(o.value(QStringLiteral("lastUsed")).toString(), Qt::ISODateWithMs);

        // Invalid ids would become paths: drop the entry. Everything else
        // is repaired to a sane default instead of losing the profile.
        bool duplicate = false;
        for (const Profile &other : std::as_const(profiles))
            duplicate = duplicate || other.id == p.id;
        if (!Ids::isValidProfileId(p.id) || duplicate) {
            qCWarning(lcProfiles) << "dropping profile entry with invalid or duplicate id";
            continue;
        }
        if (!isValidName(p.name))
            p.name = QStringLiteral("Player %1").arg(profiles.size() + 1);
        if (!avatars().contains(p.avatar))
            p.avatar = avatars().constFirst();
        if (!colors().contains(p.color))
            p.color = colors().constFirst();
        if (!isValidKind(p.kind))
            p.kind = kStandard;
        if (!p.createdAt.isValid())
            p.createdAt = QDateTime::fromMSecsSinceEpoch(0, QTimeZone::UTC);
        if (!p.lastUsed.isValid())
            p.lastUsed = p.createdAt;
        profiles.append(p);
    }
    if (profiles.isEmpty())
        return moveAside("no valid profiles");
    m_profiles = profiles;
    return true;
}

bool ProfileService::write(const QList<Profile> &profiles) const
{
    const QString path = filePath();
    if (path.isEmpty() || !QDir().mkpath(m_configDir))
        return false;
    QJsonArray array;
    for (const Profile &p : profiles) {
        array.append(QJsonObject {
            { QStringLiteral("id"), p.id }, { QStringLiteral("name"), p.name },
            { QStringLiteral("avatar"), p.avatar }, { QStringLiteral("color"), p.color },
            { QStringLiteral("kind"), p.kind }, { QStringLiteral("createdAt"), isoUtc(p.createdAt) },
            { QStringLiteral("lastUsed"), isoUtc(p.lastUsed) },
        });
    }
    const QJsonObject root { { QStringLiteral("version"), kVersion }, { QStringLiteral("profiles"), array } };
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        qCWarning(lcProfiles) << "cannot write profiles.json:" << out.errorString();
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        qCWarning(lcProfiles) << "cannot commit profiles.json:" << out.errorString();
        return false;
    }
    return true;
}

bool ProfileService::commit(const QList<Profile> &next)
{
    if (!write(next))
        return false;
    m_profiles = next;
    emit profilesChanged();
    return true;
}

void ProfileService::createDefault()
{
    Profile p;
    p.id = kDefaultId;
    p.name = QStringLiteral("Player 1");
    p.avatar = avatars().constFirst();
    p.color = colors().constFirst();
    p.kind = kStandard;
    p.createdAt = QDateTime::currentDateTimeUtc();
    p.lastUsed = p.createdAt;
    m_profiles = { p };
    if (!write(m_profiles))
        qCWarning(lcProfiles) << "running with an unsaved default profile";

    // M1 kept one navstate.json for everybody; it becomes the default
    // profile's, so the first M2 start returns to the same tile.
    if (!m_stateDir.isEmpty()) {
        const QString legacy = m_stateDir + QStringLiteral("/navstate.json");
        const QString target = profileStateDir(kDefaultId) + QStringLiteral("/navstate.json");
        if (QFileInfo::exists(legacy) && !QFileInfo::exists(target) && QDir().mkpath(profileStateDir(kDefaultId)))
            QFile::rename(legacy, target);
    }
}

void ProfileService::pickMostRecent()
{
    QString best;
    QDateTime bestTime;
    for (const Profile &p : std::as_const(m_profiles)) {
        if (best.isEmpty() || p.lastUsed > bestTime) {
            best = p.id;
            bestTime = p.lastUsed;
        }
    }
    if (best != m_activeId) {
        m_activeId = best;
        emit activeChanged();
    }
}

QDateTime ProfileService::nextTimestamp() const
{
    // Strictly increasing, so "most recently used" never ties, even for two
    // selections within the same millisecond or with a clock step back.
    QDateTime now = QDateTime::currentDateTimeUtc();
    for (const Profile &p : m_profiles) {
        if (p.lastUsed >= now)
            now = p.lastUsed.addMSecs(1);
    }
    return now;
}

QString ProfileService::newId() const
{
    for (;;) {
        const QString id = QStringLiteral("p") + QString::number(QRandomGenerator::system()->generate64()
                                                                  & 0xffffffffffULL, 16).rightJustified(10, QLatin1Char('0'));
        if (indexOf(id) < 0)
            return id;
    }
}

QString ProfileService::create(const QString &name, const QString &avatar, const QString &color,
                               const QString &kind)
{
    if (m_profiles.size() >= kMaxProfiles || !isValidName(name) || nameTaken(name, QString())
        || !avatars().contains(avatar) || !colors().contains(color) || !isValidKind(kind))
        return {};
    Profile p;
    p.id = newId();
    p.name = normalizedName(name);
    p.avatar = avatar;
    p.color = color;
    p.kind = kind;
    p.createdAt = QDateTime::currentDateTimeUtc();
    // Never used yet: older than every existing profile, so creating one
    // doesn't change who is preselected.
    p.lastUsed = QDateTime::fromMSecsSinceEpoch(0, QTimeZone::UTC);
    QList<Profile> next = m_profiles;
    next.append(p);
    if (!commit(next))
        return {};
    qCInfo(lcProfiles) << "created profile" << p.id;
    return p.id;
}

bool ProfileService::rename(const QString &id, const QString &name)
{
    const int i = indexOf(id);
    if (i < 0 || !isValidName(name) || nameTaken(name, id))
        return false;
    QList<Profile> next = m_profiles;
    next[i].name = normalizedName(name);
    if (next == m_profiles)
        return true;
    if (!commit(next))
        return false;
    if (id == m_activeId)
        emit activeChanged();
    return true;
}

bool ProfileService::setAppearance(const QString &id, const QString &avatar, const QString &color)
{
    const int i = indexOf(id);
    if (i < 0 || !avatars().contains(avatar) || !colors().contains(color))
        return false;
    QList<Profile> next = m_profiles;
    next[i].avatar = avatar;
    next[i].color = color;
    if (next == m_profiles)
        return true;
    if (!commit(next))
        return false;
    if (id == m_activeId)
        emit activeChanged();
    return true;
}

bool ProfileService::setKind(const QString &id, const QString &kind)
{
    const int i = indexOf(id);
    if (i < 0 || !isValidKind(kind))
        return false;
    QList<Profile> next = m_profiles;
    next[i].kind = kind;
    if (next == m_profiles)
        return true;
    if (!commit(next))
        return false;
    if (id == m_activeId)
        emit activeChanged();
    return true;
}

bool ProfileService::update(const QString &id, const QString &name, const QString &avatar,
                            const QString &color, const QString &kind)
{
    const int i = indexOf(id);
    if (i < 0 || !isValidName(name) || nameTaken(name, id) || !avatars().contains(avatar)
        || !colors().contains(color) || !isValidKind(kind))
        return false;
    QList<Profile> next = m_profiles;
    next[i].name = normalizedName(name);
    next[i].avatar = avatar;
    next[i].color = color;
    next[i].kind = kind;
    if (next == m_profiles)
        return true;
    if (!commit(next))
        return false;
    if (id == m_activeId)
        emit activeChanged();
    return true;
}

bool ProfileService::dataDirsAreSafe() const
{
    for (const QString &base : { m_configDir, m_stateDir }) {
        if (base.isEmpty())
            continue;
        if (QFileInfo(base).isSymLink() || QFileInfo(base + QStringLiteral("/profiles")).isSymLink())
            return false;
    }
    return true;
}

bool ProfileService::remove(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0 || m_profiles.size() <= 1)
        return false;
    if (!dataDirsAreSafe()) {
        qCWarning(lcProfiles) << "not removing profile" << id << "- a mightyos/ or profiles/ dir is a symlink";
        return false;
    }
    QList<Profile> next = m_profiles;
    next.removeAt(i);
    if (!commit(next))
        return false;

    // Per-profile data goes with it. The id was validated, so these are
    // direct children of our own profiles/ dirs.
    for (const QString &dir : { profileConfigDir(id), profileStateDir(id) }) {
        if (!dir.isEmpty() && QFileInfo(dir).isDir() && !QFileInfo(dir).isSymLink())
            QDir(dir).removeRecursively();
        else if (!dir.isEmpty() && QFileInfo(dir).isSymLink())
            QFile::remove(dir);   // never follow a symlink out of our dirs
    }
    qCInfo(lcProfiles) << "removed profile" << id;
    emit profileRemoved(id);
    if (id == m_activeId) {
        m_activeId.clear();
        pickMostRecent();
    }
    return true;
}

bool ProfileService::select(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0)
        return false;
    QList<Profile> next = m_profiles;
    next[i].lastUsed = nextTimestamp();
    if (!commit(next))
        qCWarning(lcProfiles) << "could not record last use of" << id;   // still switch
    if (m_activeId != id) {
        m_activeId = id;
        emit activeChanged();
    }
    return true;
}

QStringList ProfileService::suggestedNames(int count) const
{
    static const char *const pool[] = {
        "Comet", "Pixel", "Nova", "Blaze", "Echo", "Orbit", "Ziggy", "Luna",
        "Jet", "Ace", "Indy", "Juno", "Rex", "Sunny", "Storm", "Kiwi",
    };
    QStringList names;
    for (const char *candidate : pool) {
        if (names.size() >= count)
            break;
        const QString name = QString::fromLatin1(candidate);
        if (!nameTaken(name, QString()))
            names.append(name);
    }
    for (int n = 2; names.size() < count && n < 100; ++n) {
        const QString name = QStringLiteral("Player %1").arg(n);
        if (!nameTaken(name, QString()))
            names.append(name);
    }
    return names;
}

void ProfileService::reload()
{
    const QString previous = m_activeId;
    if (!load())
        createDefault();
    emit profilesChanged();
    if (indexOf(previous) < 0) {
        m_activeId.clear();
        pickMostRecent();
    }
}
