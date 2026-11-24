#include "jsonfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace JsonFile {

std::optional<QByteArray> readRegularFile(const QString &path, qint64 limit, bool followSymlinks,
                                          int *error, uint *owner, uint *mode, uint *group)
{
    const auto fail = [error](int code) -> std::optional<QByteArray> {
        if (error)
            *error = code;
        return std::nullopt;
    };
    const QByteArray native = QFile::encodeName(path);
    int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY;
    if (!followSymlinks)
        flags |= O_NOFOLLOW;
    const int fd = ::open(native.constData(), flags);
    if (fd < 0)
        return fail(errno);
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int code = errno;
        ::close(fd);
        return fail(code);
    }
    if (!S_ISREG(st.st_mode) || st.st_size > limit) {
        ::close(fd);
        return fail(S_ISREG(st.st_mode) ? EFBIG : EINVAL);
    }
    if (owner)
        *owner = st.st_uid;
    if (mode)
        *mode = st.st_mode;
    if (group)
        *group = st.st_gid;
    // One byte more than fstat() promised, to notice a file that grows.
    const qint64 expected = st.st_size;
    QByteArray data;
    data.resize(expected + 1);
    qint64 total = 0;
    while (total <= expected) {
        const ssize_t n = ::read(fd, data.data() + total, static_cast<size_t>(expected + 1 - total));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            const int code = errno;   // EIO: no partial data
            ::close(fd);
            return fail(code);
        }
        if (n == 0)
            break;
        total += n;
    }
    ::close(fd);
    if (total > expected)
        return fail(EAGAIN);   // changed while we read
    data.truncate(total);
    return data;
}

bool writePrivate(const QString &base, const QStringList &subdirs, const QString &name, const QByteArray &data,
                  uint mode, QString *error)
{
    auto fail = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return false;
    };
    const auto validComponent = [](const QString &c) {
        return !c.isEmpty() && c != QLatin1String(".") && c != QLatin1String("..") && !c.contains(QLatin1Char('/'))
               && !c.contains(QChar(0));
    };
    if (!validComponent(name))
        return fail("invalid file name");
    int dirfd = ::open(QFile::encodeName(base).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0)
        return fail("base directory missing");
    for (const QString &component : subdirs) {
        if (!validComponent(component)) {
            ::close(dirfd);
            return fail("invalid directory name");
        }
        const QByteArray c = QFile::encodeName(component);
        if (::mkdirat(dirfd, c.constData(), 0700) != 0 && errno != EEXIST) {
            ::close(dirfd);
            return fail("cannot create directory");
        }
        const int next = ::openat(dirfd, c.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(dirfd);
        if (next < 0)
            return fail("directory is a symlink or not a directory");
        struct stat st {};
        if (::fstat(next, &st) != 0 || st.st_uid != ::getuid()) {
            ::close(next);
            return fail("directory belongs to another user");
        }
        if ((st.st_mode & 077) != 0)
            ::fchmod(next, st.st_mode & 0700);
        dirfd = next;
    }
    const QByteArray target = QFile::encodeName(name);
    QByteArray temp;
    int fd = -1;
    for (int attempt = 0; attempt < 8 && fd < 0; ++attempt) {
        temp = "." + target + ".tmp-" + QByteArray::number(QRandomGenerator::system()->generate64(), 16);
        fd = ::openat(dirfd, temp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    if (fd < 0) {
        ::close(dirfd);
        return fail("cannot create file");
    }
    bool ok = ::fchmod(fd, mode & 0777) == 0;
    qint64 written = 0;
    while (ok && written < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + written, size_t(data.size() - written));
        if (n < 0 && errno == EINTR)
            continue;
        ok = n > 0;
        written += qMax<ssize_t>(0, n);
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::renameat(dirfd, temp.constData(), dirfd, target.constData()) != 0) {
        ::unlinkat(dirfd, temp.constData(), 0);
        ::close(dirfd);
        return fail("cannot write file");
    }
    ::fsync(dirfd);
    ::close(dirfd);
    return true;
}

bool moveAside(const QString &path, const QString &suffix)
{
    const QString aside = path + suffix;
    QFile::remove(aside);
    return QFile::rename(path, aside);
}

std::optional<QJsonObject> read(const QString &path, qint64 maxBytes, ReadStatus *status)
{
    auto result = [status](ReadStatus s) {
        if (status)
            *status = s;
        return std::nullopt;
    };
    if (path.isEmpty())
        return result(ReadStatus::Missing);
    if (!QFileInfo(path).exists() && !QFileInfo(path).isSymLink())
        return result(ReadStatus::Missing);
    const std::optional<QByteArray> data = readRegularFile(path, maxBytes, false);
    if (!data)
        return result(ReadStatus::Invalid);
    QJsonParseError error {};
    const QJsonDocument doc = QJsonDocument::fromJson(*data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return result(ReadStatus::Invalid);
    if (status)
        *status = ReadStatus::Ok;
    return doc.object();
}

bool write(const QString &path, const QJsonObject &object)
{
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return out.commit();
}

} // namespace JsonFile
