#include "ids.h"

#include <QDir>
#include <QRegularExpression>

namespace Ids {

bool isValidProfileId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return re.match(id).hasMatch();
}

bool isValidFlatpakId(const QString &id)
{
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_-]*(\\.[A-Za-z_][A-Za-z0-9_-]*)+$"));
    return id.size() <= 255 && re.match(id).hasMatch();
}

bool isValidDesktopFileId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_+@-][A-Za-z0-9._+@-]*\\.desktop$"));
    return id.size() <= 255 && re.match(id).hasMatch();
}

bool isValidManifestId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$"));
    return re.match(id).hasMatch();
}

bool isValidIconName(const QString &name)
{
    if (name.isEmpty() || name.size() > 128 || name.startsWith(QLatin1Char('.')))
        return false;
    for (const QChar c : name) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c.category() == QChar::Other_Control)
            return false;
    }
    return true;
}

bool isCleanAbsolutePath(const QString &path)
{
    if (path.isEmpty() || path.size() > 4096 || !path.startsWith(QLatin1Char('/')))
        return false;
    for (const QChar c : path) {
        if (c.category() == QChar::Other_Control)
            return false;
    }
    // cleanPath() resolves "." / ".." / "//"; a clean path is its own result.
    return QDir::cleanPath(path) == path;
}

bool isValidItemKey(const QString &key)
{
    if (key.isEmpty() || key.size() > 512)
        return false;
    for (const QChar c : key) {
        if (c.isSpace() || c.category() == QChar::Other_Control)
            return false;
    }
    return true;
}

} // namespace Ids
