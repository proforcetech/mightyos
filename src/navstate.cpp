#include "navstate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(lcNavState, "mightyos.navstate")

namespace {

constexpr qsizetype kMaxNameLength = 64;
constexpr qsizetype kMaxKeyLength = 512;

bool isValidScreenName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[a-z][a-z0-9_-]*$"));
    return !name.isEmpty() && name.size() <= kMaxNameLength && re.match(name).hasMatch();
}

// Non-negative integral JSON number (0 if absent). False if malformed.
bool readIndex(const QJsonObject &obj, const QString &name, int *out)
{
    const QJsonValue v = obj.value(name);
    if (v.isUndefined()) {
        *out = 0;
        return true;
    }
    if (!v.isDouble())
        return false;
    const double d = v.toDouble();
    if (d < 0 || d > 1e6 || d != static_cast<double>(static_cast<int>(d)))
        return false;
    *out = static_cast<int>(d);
    return true;
}

} // namespace

NavState::NavState(QObject *parent)
    : QObject(parent)
    , m_directory(defaultDirectory())
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &NavState::flush);
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &NavState::flush);
}

NavState::~NavState()
{
    flush();
}

void NavState::setDirectory(const QString &directory)
{
    if (m_directory == directory)
        return;
    flush();   // a pending save belongs to the old directory
    m_directory = directory;
    emit directoryChanged();
}

QString NavState::defaultDirectory()
{
    // $XDG_STATE_HOME, falling back to ~/.local/state (Qt >= 6.7).
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/mightyos");
}

bool NavState::isValidProfileId(const QString &profileId)
{
    // Used as a single path component: no separators, no dots.
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return re.match(profileId).hasMatch();
}

QString NavState::filePath(const QString &profileId) const
{
    if (m_directory.isEmpty())
        return {};
    if (profileId.isEmpty())
        return m_directory + QStringLiteral("/navstate.json");
    if (!isValidProfileId(profileId))
        return {};
    return m_directory + QStringLiteral("/profiles/") + profileId + QStringLiteral("/navstate.json");
}

QVariantList NavState::sanitize(const QJsonArray &path)
{
    QVariantList out;
    for (const QJsonValue &value : path) {
        if (out.size() >= kMaxEntries || !value.isObject())
            break;
        const QJsonObject obj = value.toObject();

        const QString screen = obj.value(QStringLiteral("screen")).toString();
        if (!isValidScreenName(screen))
            break;

        const QJsonValue params = obj.value(QStringLiteral("params"));
        const QJsonValue extra = obj.value(QStringLiteral("extra"));
        if (!(params.isUndefined() || params.isObject()) || !(extra.isUndefined() || extra.isObject()))
            break;

        int row = 0;
        int index = 0;
        if (!readIndex(obj, QStringLiteral("row"), &row) || !readIndex(obj, QStringLiteral("index"), &index))
            break;

        const QJsonValue key = obj.value(QStringLiteral("key"));
        if (!(key.isUndefined() || key.isString()) || key.toString().size() > kMaxKeyLength)
            break;

        QVariantMap entry;
        entry.insert(QStringLiteral("screen"), screen);
        entry.insert(QStringLiteral("params"), params.toObject().toVariantMap());
        entry.insert(QStringLiteral("row"), row);
        entry.insert(QStringLiteral("index"), index);
        entry.insert(QStringLiteral("key"), key.toString());
        entry.insert(QStringLiteral("extra"), extra.toObject().toVariantMap());
        out.append(entry);
    }
    return out;
}

QVariantList NavState::load(const QString &profileId) const
{
    const QString path = filePath(profileId);
    if (path.isEmpty())
        return {};

    QFile file(path);
    if (!file.exists())
        return {};
    if (file.size() > kMaxFileBytes) {
        qCWarning(lcNavState) << "ignoring oversized" << path;
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcNavState) << "cannot read" << path << file.errorString();
        return {};
    }

    QJsonParseError error {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(kMaxFileBytes), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcNavState) << "ignoring malformed" << path << error.errorString();
        return {};
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt(-1) != kVersion) {
        qCInfo(lcNavState) << "ignoring" << path << "with unknown version";
        return {};
    }
    return sanitize(root.value(QStringLiteral("path")).toArray());
}

bool NavState::save(const QVariantList &path, const QString &profileId)
{
    // A direct save supersedes an older debounced one for the same file;
    // otherwise a later flush() would overwrite it with stale state.
    if (m_pending && profileId == m_pendingProfile) {
        m_pending = false;
        m_saveTimer.stop();
    }
    const QString file = filePath(profileId);
    if (file.isEmpty()) {
        qCWarning(lcNavState) << "no valid state path for profile" << profileId;
        return false;
    }
    // Normalise through the same validation as load(), so we never write
    // something we would refuse to read back.
    const QVariantList clean = sanitize(QJsonArray::fromVariantList(path));

    QJsonObject root;
    root.insert(QStringLiteral("version"), kVersion);
    root.insert(QStringLiteral("path"), QJsonArray::fromVariantList(clean));

    if (!QDir().mkpath(QFileInfo(file).absolutePath())) {
        qCWarning(lcNavState) << "cannot create directory for" << file;
        return false;
    }
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        qCWarning(lcNavState) << "cannot write" << file << out.errorString();
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!out.commit()) {
        qCWarning(lcNavState) << "cannot commit" << file << out.errorString();
        return false;
    }
    return true;
}

void NavState::saveLater(const QVariantList &path, const QString &profileId)
{
    if (m_pending && profileId != m_pendingProfile)
        flush();   // never drop another profile's pending state
    m_pendingPath = path;
    m_pendingProfile = profileId;
    m_pending = true;
    m_saveTimer.start();
}

void NavState::flush()
{
    m_saveTimer.stop();
    if (!m_pending)
        return;
    m_pending = false;
    save(m_pendingPath, m_pendingProfile);
}
