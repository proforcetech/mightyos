#include "definitionlayers.h"

#include "ids.h"
#include "jsonfile.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <algorithm>

#include <sys/stat.h>
#include <unistd.h>

namespace DefinitionLayers {

bool trustedOwnerAndMode(uint owner, uint group, uint mode)
{
    if (owner != ::getuid() && owner != 0)
        return false;
    if (mode & 002)
        return false;
    return !(mode & 020) || group == ::getgid();
}

QStringList defaultDirs(const QString &builtinDir, const QString &name)
{
    QStringList dirs { builtinDir };
    // XDG_DATA_DIRS is most important first; layers go least important first.
    QStringList data = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    data.removeAll(dataHome);
    std::reverse(data.begin(), data.end());
    for (const QString &d : std::as_const(data))
        dirs.append(d + QStringLiteral("/mightyos/") + name);
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!config.isEmpty())
        dirs.append(config + QStringLiteral("/mightyos/") + name);
    return dirs;
}

Layer read(const QString &dir, int maxFiles, qint64 maxBytes)
{
    Layer layer;
    layer.dir = dir;
    const bool builtin = dir.startsWith(QLatin1String(":/"));
    if (!builtin) {
        if (!Ids::isCleanAbsolutePath(dir))
            return layer;
        struct stat dst {};
        if (::stat(QFile::encodeName(dir).constData(), &dst) != 0)
            return layer;   // no such layer: normal
        layer.exists = true;
        if (!S_ISDIR(dst.st_mode) || !trustedOwnerAndMode(dst.st_uid, dst.st_gid, dst.st_mode)) {
            layer.dirError = QStringLiteral("directory is writable by other users or not ours; ignored");
            return layer;
        }
    } else {
        layer.exists = QDir(dir).exists();
    }
    const QStringList names = QDir(dir).entryList({ QStringLiteral("*.json") }, QDir::Files | QDir::Readable, QDir::Name);
    for (const QString &name : names) {
        if (layer.files.size() >= maxFiles)
            break;
        File file;
        file.name = name;
        file.path = dir + QLatin1Char('/') + name;
        if (builtin) {
            QFile f(file.path);
            if (f.open(QIODevice::ReadOnly) && f.size() <= maxBytes)
                file.data = f.readAll();
            else
                file.error = QStringLiteral("unreadable or too large");
        } else {
            uint owner = 0, mode = 0, group = 0;
            file.data = JsonFile::readRegularFile(file.path, maxBytes, false, nullptr, &owner, &mode, &group);
            if (file.data && !trustedOwnerAndMode(owner, group, mode)) {
                file.data.reset();
                file.error = QStringLiteral("not ours (or root's), or writable by other users");
            } else if (!file.data) {
                file.error = QStringLiteral("unreadable, too large or not a regular file");
            }
        }
        layer.files.append(file);
    }
    return layer;
}

} // namespace DefinitionLayers
