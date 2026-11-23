#include "settings.h"

#include "jsonfile.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <cerrno>

Q_LOGGING_CATEGORY(lcSettings, "mightyos.settings", QtWarningMsg)

namespace {

QString defaultConfigDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/mightyos");
}

const QString kRumble = QStringLiteral("rumble");
const QString kTvRemote = QStringLiteral("tvRemote");
const QString kVersionKey = QStringLiteral("version");

} // namespace

QStringList Settings::tvRemoteModes()
{
    return { QStringLiteral("off"), QStringLiteral("navigation"), QStringLiteral("full") };
}

Settings::Settings(QObject *parent)
    : Settings(defaultConfigDir(), parent)
{
}

Settings::Settings(const QString &configDir, QObject *parent)
    : QObject(parent)
    , m_configDir(configDir)
{
    reload();
}

QString Settings::filePath() const
{
    return m_configDir.isEmpty() ? QString() : m_configDir + QStringLiteral("/settings.json");
}

void Settings::setWritable(bool writable)
{
    if (m_writable == writable)
        return;
    m_writable = writable;
    emit writableChanged();
}

void Settings::reload()
{
    m_root = {};
    bool writable = !filePath().isEmpty();
    int error = 0;
    const auto data = filePath().isEmpty()
        ? std::nullopt : JsonFile::readRegularFile(filePath(), kMaxFileBytes, false, &error);
    if (!data) {
        if (!filePath().isEmpty() && error != ENOENT) {
            // Can't read it: don't call it corrupt and don't overwrite it.
            qCWarning(lcSettings) << "cannot read settings.json:" << qt_error_string(error)
                                  << "- using defaults, not saving";
            writable = false;
        }
    } else {
        QJsonParseError parse {};
        const QJsonDocument doc = QJsonDocument::fromJson(*data, &parse);
        const int version = doc.isObject() ? doc.object().value(kVersionKey).toInt(0) : 0;
        if (parse.error != QJsonParseError::NoError || !doc.isObject() || version < 1) {
            qCWarning(lcSettings) << "ignoring unusable settings.json - kept as \".corrupt\"";
            JsonFile::moveAside(filePath(), QStringLiteral(".corrupt"));
        } else {
            m_root = doc.object();
            if (version > kVersion) {
                qCWarning(lcSettings) << "settings.json is from a newer MightyOS (version" << version
                                      << ") - reading known keys, not saving";
                writable = false;
            }
        }
    }
    setWritable(writable);
    applyRoot();
}

void Settings::applyRoot()
{
    const QJsonValue rumble = m_root.value(kRumble);
    const bool rumbleValue = rumble.isBool() ? rumble.toBool() : true;
    if (rumbleValue != m_rumble) {
        m_rumble = rumbleValue;
        emit rumbleChanged();
    }
    const QString remote = m_root.value(kTvRemote).toString();
    const QString remoteValue = tvRemoteModes().contains(remote) ? remote : QStringLiteral("navigation");
    if (remoteValue != m_tvRemote) {
        m_tvRemote = remoteValue;
        emit tvRemoteChanged();
    }
}

bool Settings::writeKey(const QString &key, const QJsonValue &value)
{
    if (!m_writable) {
        qCWarning(lcSettings) << "settings.json is read-only for this session; not saved";
        emit writeFailed();
        return false;
    }
    QJsonObject root = m_root;
    root.insert(kVersionKey, kVersion);
    root.insert(key, value);
    if (!JsonFile::write(filePath(), root)) {
        qCWarning(lcSettings) << "could not write settings.json";
        emit writeFailed();
        return false;
    }
    m_root = root;
    return true;
}

void Settings::setRumble(bool rumble)
{
    if (rumble == m_rumble || !writeKey(kRumble, rumble))
        return;
    m_rumble = rumble;
    emit rumbleChanged();
}

void Settings::setTvRemote(const QString &mode)
{
    if (mode == m_tvRemote || !tvRemoteModes().contains(mode) || !writeKey(kTvRemote, mode))
        return;
    m_tvRemote = mode;
    emit tvRemoteChanged();
}
