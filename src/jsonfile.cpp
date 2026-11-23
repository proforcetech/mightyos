#include "jsonfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace JsonFile {

std::optional<QByteArray> readRegularFile(const QString &path, qint64 limit, bool followSymlinks)
{
    const QByteArray native = QFile::encodeName(path);
    int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY;
    if (!followSymlinks)
        flags |= O_NOFOLLOW;
    const int fd = ::open(native.constData(), flags);
    if (fd < 0)
        return std::nullopt;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > limit) {
        ::close(fd);
        return std::nullopt;
    }
    // One byte more than fstat() promised, to notice a file that grows.
    const qint64 expected = st.st_size;
    QByteArray data;
    data.resize(expected + 1);
    qint64 total = 0;
    while (total <= expected) {
        const ssize_t n = ::read(fd, data.data() + total, static_cast<size_t>(expected + 1 - total));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        total += n;
    }
    ::close(fd);
    if (total > expected)
        return std::nullopt;   // changed while we read
    data.truncate(total);
    return data;
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
