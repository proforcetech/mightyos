#include "sessionstate.h"

#include "jsonfile.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <grp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

Q_LOGGING_CATEGORY(lcSessionState, "mightyos.session")

namespace {

const QString kSubdir = QStringLiteral("mightyos");
const QString kName = QStringLiteral("session.json");

QStringList stringArray(const QJsonValue &value, int max, int maxLength)
{
    QStringList out;
    for (const QJsonValue &v : value.toArray()) {
        const QString s = v.toString();
        if (!s.isEmpty() && s.size() <= maxLength && out.size() < max && !out.contains(s))
            out.append(s);
    }
    return out;
}

} // namespace

SessionState::SessionState(const QString &stateHome)
    : m_stateHome(stateHome)
{
}

QString SessionState::filePath() const
{
    return m_stateHome.isEmpty() ? QString() : m_stateHome + QLatin1Char('/') + kSubdir + QLatin1Char('/') + kName;
}

bool SessionState::isCrash(int previousStatus, bool wasUp)
{
    if (previousStatus >= 128)
        return true;                  // a signal
    return previousStatus > 0 && !wasUp;   // failed during startup
}

SessionState::Start SessionState::evaluate(QJsonObject &state, qint64 bootSecs, const QString &bootId,
                                           int previousStatus)
{
    Start start;
    const bool sameBoot = !bootId.isEmpty() && state.value(QStringLiteral("boot")).toString() == bootId;
    start.previousCrashed = isCrash(previousStatus, state.value(QStringLiteral("up")).toBool(false));
    QList<qint64> crashes;
    if (sameBoot) {
        for (const QJsonValue &value : state.value(QStringLiteral("crashes")).toArray()) {
            const double t = value.toDouble(-1);
            if (t >= 0 && t <= double(bootSecs) && bootSecs - qint64(t) < kWindowSecs)
                crashes.append(qint64(t));
        }
    }
    if (start.previousCrashed)
        crashes.append(bootSecs);
    std::sort(crashes.begin(), crashes.end());
    while (crashes.size() > kMaxCrashEntries)
        crashes.removeFirst();
    start.recentCrashes = int(crashes.size());
    start.safeMode = start.recentCrashes >= kSafeModeCrashes;

    QJsonArray array;
    for (const qint64 t : std::as_const(crashes))
        array.append(double(t));
    state.insert(QStringLiteral("version"), kVersion);
    state.insert(QStringLiteral("boot"), bootId);
    state.insert(QStringLiteral("up"), false);
    state.insert(QStringLiteral("crashes"), array);
    return start;
}

QJsonObject SessionState::read() const
{
    const QString path = filePath();
    if (path.isEmpty())
        return {};
    int error = 0;
    const auto data = JsonFile::readRegularFile(path, kMaxFileBytes, false, &error);
    if (!data) {
        if (error != ENOENT)
            qCWarning(lcSessionState) << "session state unreadable, errno" << error;
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(*data);
    if (!doc.isObject() || doc.object().value(QStringLiteral("version")).toInt(-1) != kVersion) {
        qCWarning(lcSessionState) << "session state unusable, moved aside";
        JsonFile::moveAside(path, QStringLiteral(".corrupt"));
        return {};
    }
    return doc.object();
}

bool SessionState::write(const QJsonObject &state) const
{
    if (m_stateHome.isEmpty() || !QDir().mkpath(m_stateHome))
        return false;
    QString error;
    const bool ok = JsonFile::writePrivate(m_stateHome, { kSubdir }, kName,
                                           QJsonDocument(state).toJson(QJsonDocument::Compact), 0600, &error);
    if (!ok)
        qCWarning(lcSessionState).noquote() << "session state not written:" << error;
    return ok;
}

bool SessionState::update(const char *key, const QJsonValue &value) const
{
    QJsonObject state = read();
    state.insert(QStringLiteral("version"), kVersion);
    state.insert(QLatin1String(key), value);
    return write(state);
}

SessionState::Start SessionState::recordStart(int previousStatus)
{
    return recordStart(bootSeconds(), bootId(), previousStatus);
}

SessionState::Start SessionState::recordStart(qint64 bootSecs, const QString &bootIdValue, int previousStatus)
{
    QJsonObject state = read();
    const Start start = evaluate(state, bootSecs, bootIdValue, previousStatus);
    write(state);
    return start;
}

bool SessionState::markUp()
{
    return update("up", true);
}

QStringList SessionState::launchUnits() const
{
    return stringArray(read().value(QStringLiteral("units")), kMaxUnits, 255);
}

bool SessionState::setLaunchUnits(const QStringList &units)
{
    return update("units", QJsonArray::fromStringList(units.mid(0, kMaxUnits)));
}

QStringList SessionState::warnedGroups() const
{
    QStringList groups = stringArray(read().value(QStringLiteral("warnedGroups")), 16, 32);
    groups.sort();
    return groups;
}

bool SessionState::setWarnedGroups(const QStringList &groups)
{
    return update("warnedGroups", QJsonArray::fromStringList(groups));
}

namespace {

QStringList pick(const QStringList &names, const QStringList &wanted)
{
    QStringList found;
    for (const QString &name : names) {
        if (wanted.contains(name) && !found.contains(name))
            found.append(name);
    }
    found.sort();
    return found;
}

} // namespace

QStringList SessionState::riskyGroups(const QStringList &names)
{
    static const QStringList risky {
        QStringLiteral("admin"), QStringLiteral("disk"), QStringLiteral("docker"), QStringLiteral("incus-admin"),
        QStringLiteral("input"), QStringLiteral("libvirt"), QStringLiteral("lxd"), QStringLiteral("root"),
        QStringLiteral("sudo"), QStringLiteral("tty"), QStringLiteral("video"), QStringLiteral("wheel"),
    };
    return pick(names, risky);
}

QStringList SessionState::informationalGroups(const QStringList &names)
{
    return pick(names, { QStringLiteral("adm"), QStringLiteral("systemd-journal") });
}

QStringList SessionState::currentGroupNames()
{
    const int count = ::getgroups(0, nullptr);
    std::vector<gid_t> gids(size_t(std::max(count, 0)) + 1);
    const int n = count > 0 ? ::getgroups(count, gids.data()) : 0;
    gids.resize(size_t(std::max(n, 0)));
    gids.push_back(::getgid());
    QStringList names;
    std::vector<char> buffer(4096);
    for (const gid_t gid : gids) {
        group entry {};
        group *result = nullptr;
        int rc = 0;
        // Huge groups (many members) need a bigger buffer.
        while ((rc = ::getgrgid_r(gid, &entry, buffer.data(), buffer.size(), &result)) == ERANGE
               && buffer.size() < (size_t(1) << 22))
            buffer.resize(buffer.size() * 2);
        if (rc == 0 && result && result->gr_name) {
            const QString name = QString::fromLocal8Bit(result->gr_name);
            if (!names.contains(name))
                names.append(name);
        }
    }
    return names;
}

qint64 SessionState::bootSeconds()
{
    timespec ts {};
    if (::clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec);
}

QString SessionState::bootId()
{
    QFile file(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(file.read(64)).trimmed();
}

// ---------------------------------------------------------------------------

SessionLock::~SessionLock()
{
    if (m_fd >= 0)
        ::close(m_fd);   // releases the lock; the file stays (a stale file is unlocked)
}

QString SessionLock::pidFilePath(const QString &runtimeDir)
{
    return runtimeDir.isEmpty() ? QString() : runtimeDir + QStringLiteral("/mightyos/session.pid");
}

namespace {

// <runtimeDir>/mightyos: created 0700 if missing; must be a real dir we own
// without group/other access. Returns an O_DIRECTORY fd or -1.
int openPrivateDir(const QString &runtimeDir, bool create, QString *error)
{
    if (runtimeDir.isEmpty() || !QDir::isAbsolutePath(runtimeDir)) {
        *error = QStringLiteral("XDG_RUNTIME_DIR is not set");
        return -1;
    }
    const int base = ::open(QFile::encodeName(runtimeDir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (base < 0) {
        *error = QStringLiteral("XDG_RUNTIME_DIR: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return -1;
    }
    if (create && ::mkdirat(base, "mightyos", 0700) != 0 && errno != EEXIST) {
        *error = QStringLiteral("mkdir: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        ::close(base);
        return -1;
    }
    const int dir = ::openat(base, "mightyos", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(base);
    struct stat st {};
    if (dir < 0 || ::fstat(dir, &st) != 0 || st.st_uid != ::getuid() || (st.st_mode & 077)) {
        *error = dir < 0 ? QStringLiteral("no session directory") : QStringLiteral("session directory not private");
        if (dir >= 0)
            ::close(dir);
        return -1;
    }
    return dir;
}

} // namespace

bool SessionLock::acquire(const QString &runtimeDir, qint64 pid, QString *error)
{
    QString message;
    QString &err = error ? *error : message;
    if (m_fd >= 0)
        return true;
    const int dir = openPrivateDir(runtimeDir, true, &err);
    if (dir < 0)
        return false;
    const int fd = ::openat(dir, "session.pid", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    ::close(dir);
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid()) {
        err = QStringLiteral("pid file unusable");
        if (fd >= 0)
            ::close(fd);
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        err = QStringLiteral("another MightyOS session instance holds the lock");
        ::close(fd);
        return false;
    }
    ::fchmod(fd, 0600);
    const QByteArray text = QByteArray::number(pid) + '\n';
    if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, text.constData(), size_t(text.size()), 0) != text.size()) {
        err = QStringLiteral("pid file not written");
        ::close(fd);
        return false;
    }
    m_fd = fd;
    return true;
}

QString SessionLock::signalSession(const QString &runtimeDir, int signo)
{
    QString error;
    const int dir = openPrivateDir(runtimeDir, false, &error);
    if (dir < 0)
        return QStringLiteral("no MightyOS session (%1)").arg(error);
    const int fd = ::openat(dir, "session.pid", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ::close(dir);
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid()) {
        if (fd >= 0)
            ::close(fd);
        return QStringLiteral("no MightyOS session (no pid file)");
    }
    // A lock we can take means nobody holds it: a stale file.
    if (::flock(fd, LOCK_SH | LOCK_NB) == 0) {
        ::close(fd);
        return QStringLiteral("no MightyOS session (stale pid file)");
    }
    char buffer[32] = {};
    const ssize_t n = ::pread(fd, buffer, sizeof buffer - 1, 0);
    ::close(fd);
    bool ok = false;
    const qint64 pid = n > 0 ? QByteArray(buffer, int(n)).trimmed().toLongLong(&ok) : 0;
    if (!ok || pid <= 1)
        return QStringLiteral("pid file unreadable");
    QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
    struct stat proc {};
    if (!comm.open(QIODevice::ReadOnly) || comm.read(32).trimmed() != "mightyos"
        || ::stat(QFile::encodeName(QStringLiteral("/proc/%1").arg(pid)).constData(), &proc) != 0)
        return QStringLiteral("the session process is not MightyOS");
    // Non-dumpable processes show root as /proc owner; kill() checks the uid.
    if (::kill(pid_t(pid), signo) != 0)
        return QStringLiteral("signal not sent: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
    return {};
}
