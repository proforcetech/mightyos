#include "iconlocator.h"

#include "ids.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QStandardPaths>
#include <climits>
#include <functional>
#include <memory>

namespace {

constexpr qint64 kMaxIndexBytes = 256 * 1024;
constexpr int kMaxThemeDepth = 8;
const char *const kExtensions[] = { ".svg", ".png", ".xpm" };

// Minimal INI reader for index.theme: group -> key -> value.
QHash<QString, QHash<QString, QString>> readIndex(const QString &path)
{
    QHash<QString, QHash<QString, QString>> groups;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxIndexBytes)
        return groups;
    QString group;
    const QList<QByteArray> lines = file.read(kMaxIndexBytes).split('\n');
    for (const QByteArray &rawLine : lines) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            group = line.mid(1, line.size() - 2);
            continue;
        }
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq > 0 && !group.isEmpty())
            groups[group].insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return groups;
}

QStringList dataDirsFromEnvironment()
{
    QStringList dirs;
    auto add = [&dirs](const QString &dir) {
        if (QDir::isAbsolutePath(dir) && !dirs.contains(QDir::cleanPath(dir)))
            dirs.append(QDir::cleanPath(dir));
    };
    add(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    add(QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share"));
    add(QStringLiteral("/var/lib/flatpak/exports/share"));
    QString env = qEnvironmentVariable("XDG_DATA_DIRS");
    if (env.isEmpty())
        env = QStringLiteral("/usr/local/share:/usr/share");
    for (const QString &dir : env.split(QLatin1Char(':'), Qt::SkipEmptyParts))
        add(dir);
    return dirs;
}

std::unique_ptr<IconLocator> &sharedSlot()
{
    static std::unique_ptr<IconLocator> instance;
    return instance;
}

QString &sharedThemeName()
{
    static QString name;
    return name;
}

QMutex &sharedMutex()
{
    static QMutex mutex;
    return mutex;
}

} // namespace

IconLocator::IconLocator(Config config)
    : m_config(std::move(config))
    , m_themes(std::make_shared<const ThemeMap>())
{
    if (!m_config.themes.contains(QStringLiteral("hicolor")))
        m_config.themes.append(QStringLiteral("hicolor"));
    m_baseMtimes = baseMtimes();
}

IconLocator::Config IconLocator::defaultConfig(const QString &themeName)
{
    Config config;
    config.baseDirs.append(QDir::homePath() + QStringLiteral("/.icons"));
    for (const QString &dir : dataDirsFromEnvironment()) {
        config.baseDirs.append(dir + QStringLiteral("/icons"));
        config.pixmapDirs.append(dir + QStringLiteral("/pixmaps"));
    }
    // Without a desktop environment (cage session) there is often no
    // configured theme; prefer Ubuntu's and GNOME's, then hicolor.
    if (!themeName.isEmpty() && themeName != QLatin1String("hicolor"))
        config.themes.append(themeName);
    for (const char *fallback : { "Yaru", "Adwaita" }) {
        if (!config.themes.contains(QLatin1String(fallback)))
            config.themes.append(QString::fromLatin1(fallback));
    }
    return config;
}

IconLocator &IconLocator::shared()
{
    QMutexLocker lock(&sharedMutex());
    auto &slot = sharedSlot();
    if (!slot)
        slot = std::make_unique<IconLocator>(defaultConfig(sharedThemeName()));
    return *slot;
}

void IconLocator::setSharedThemeName(const QString &themeName)
{
    QMutexLocker lock(&sharedMutex());
    sharedThemeName() = themeName;
}

void IconLocator::invalidate() const
{
    QMutexLocker lock(&m_mutex);
    m_themes = std::make_shared<const ThemeMap>();
    m_cache.clear();
    ++m_generation;
}

namespace {
qint64 mtimeOf(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() ? info.lastModified().toMSecsSinceEpoch() : -1;
}
} // namespace

QHash<QString, qint64> IconLocator::baseMtimes() const
{
    QHash<QString, qint64> stamps;
    for (const QString &dir : m_config.baseDirs + m_config.pixmapDirs)
        stamps.insert(dir, mtimeOf(dir));
    return stamps;
}

bool IconLocator::refreshIfChanged() const
{
    std::shared_ptr<const ThemeMap> snapshot;
    QHash<QString, qint64> lastBase;
    {
        QMutexLocker lock(&m_mutex);
        snapshot = m_themes;
        lastBase = m_baseMtimes;
    }
    // All stats happen without the lock.
    const QHash<QString, qint64> currentBase = baseMtimes();
    bool changed = currentBase != lastBase;
    for (auto t = snapshot->cbegin(); !changed && t != snapshot->cend(); ++t) {
        for (auto r = t->rootMtimes.cbegin(); !changed && r != t->rootMtimes.cend(); ++r)
            changed = mtimeOf(r.key()) != r.value();
        for (const ThemeDir &dir : t->dirs) {
            if (changed)
                break;
            changed = mtimeOf(dir.path) != dir.mtime;
        }
    }
    QMutexLocker lock(&m_mutex);
    m_baseMtimes = currentBase;
    if (!changed)
        return false;
    m_themes = std::make_shared<const ThemeMap>();
    m_cache.clear();
    ++m_generation;
    return true;
}

IconLocator::Theme IconLocator::loadTheme(const Config &config, const QString &name)
{
    Theme theme;
    QHash<QString, QHash<QString, QString>> index;
    for (const QString &base : config.baseDirs) {
        const QString file = base + QLatin1Char('/') + name + QStringLiteral("/index.theme");
        if (QFileInfo::exists(file)) {
            index = readIndex(file);
            break;
        }
    }
    const QHash<QString, QString> header = index.value(QStringLiteral("Icon Theme"));
    theme.inherits = header.value(QStringLiteral("Inherits")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    QStringList dirNames = header.value(QStringLiteral("Directories")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    dirNames += header.value(QStringLiteral("ScaledDirectories")).split(QLatin1Char(','), Qt::SkipEmptyParts);

    for (const QString &base : config.baseDirs) {
        const QString themeRoot = base + QLatin1Char('/') + name;
        theme.rootMtimes.insert(themeRoot, mtimeOf(themeRoot));
        if (!QFileInfo(themeRoot).isDir())
            continue;
        for (const QString &rawName : std::as_const(dirNames)) {
            const QString dirName = rawName.trimmed();
            if (dirName.contains(QStringLiteral("..")))
                continue;
            const QString path = themeRoot + QLatin1Char('/') + dirName;
            const QDir dir(path);
            if (!dir.exists())
                continue;
            const QHash<QString, QString> keys = index.value(dirName);
            ThemeDir td;
            td.path = path;
            td.mtime = mtimeOf(path);
            td.size = keys.value(QStringLiteral("Size")).toInt();
            td.scale = qMax(1, keys.value(QStringLiteral("Scale"), QStringLiteral("1")).toInt());
            td.minSize = keys.value(QStringLiteral("MinSize"), QString::number(td.size)).toInt();
            td.maxSize = keys.value(QStringLiteral("MaxSize"), QString::number(td.size)).toInt();
            td.threshold = keys.value(QStringLiteral("Threshold"), QStringLiteral("2")).toInt();
            const QString type = keys.value(QStringLiteral("Type"), QStringLiteral("Threshold"));
            td.type = type == QLatin1String("Fixed") ? DirType::Fixed
                    : type == QLatin1String("Scalable") ? DirType::Scalable : DirType::Threshold;
            if (td.size <= 0)
                continue;
            const QStringList files = dir.entryList(QDir::Files | QDir::Readable);
            td.files = QSet<QString>(files.cbegin(), files.cend());
            if (!td.files.isEmpty())
                theme.dirs.append(td);
        }
    }
    return theme;
}

int IconLocator::distance(const ThemeDir &dir, int size)
{
    // Icon Theme Specification, DirectorySizeDistance (scale folded in).
    const int s = size;
    switch (dir.type) {
    case DirType::Fixed:
        return qAbs(dir.size * dir.scale - s);
    case DirType::Scalable:
        if (s < dir.minSize * dir.scale)
            return dir.minSize * dir.scale - s;
        if (s > dir.maxSize * dir.scale)
            return s - dir.maxSize * dir.scale;
        return 0;
    case DirType::Threshold:
        if (s < (dir.size - dir.threshold) * dir.scale)
            return dir.minSize * dir.scale - s;
        if (s > (dir.size + dir.threshold) * dir.scale)
            return s - dir.maxSize * dir.scale;
        return 0;
    }
    return 0;
}

QString IconLocator::find(const QString &name, int size) const
{
    if (!Ids::isValidIconName(name))
        return {};
    size = qBound(16, size, 1024);
    const QString cacheKey = name + QLatin1Char('@') + QString::number(size);

    std::shared_ptr<const ThemeMap> snapshot;
    quint64 generation = 0;
    {
        QMutexLocker lock(&m_mutex);
        const auto cached = m_cache.constFind(cacheKey);
        if (cached != m_cache.cend())
            return *cached;
        snapshot = m_themes;
        generation = m_generation;
    }

    // Everything below runs without the lock. Themes missing from the
    // snapshot are listed here and published afterwards.
    ThemeMap built;
    auto themeFor = [&](const QString &t) -> const Theme & {
        const auto found = snapshot->constFind(t);
        if (found != snapshot->cend())
            return *found;
        auto it = built.find(t);
        if (it == built.end())
            it = built.insert(t, loadTheme(m_config, t));
        return *it;
    };

    QStringList chain;
    std::function<void(const QString &, int)> addTheme = [&](const QString &t, int depth) {
        if (depth > kMaxThemeDepth || t.isEmpty() || chain.contains(t) || !Ids::isValidIconName(t))
            return;
        chain.append(t);
        const QStringList parents = themeFor(t).inherits;   // copy: `built` may rehash
        for (const QString &parent : parents)
            addTheme(parent.trimmed(), depth + 1);
    };
    for (const QString &t : m_config.themes) {
        if (t != QLatin1String("hicolor"))
            addTheme(t, 0);
    }
    addTheme(QStringLiteral("hicolor"), 0);

    QString result;
    for (const QString &t : std::as_const(chain)) {
        int best = INT_MAX;
        int bestExt = INT_MAX;
        const QList<ThemeDir> dirs = themeFor(t).dirs;
        for (const ThemeDir &dir : dirs) {
            for (int e = 0; e < int(std::size(kExtensions)); ++e) {
                const QString file = name + QLatin1String(kExtensions[e]);
                if (!dir.files.contains(file))
                    continue;
                const int d = distance(dir, size);
                // Closest size wins; at equal distance svg > png > xpm.
                if (d < best || (d == best && e < bestExt)) {
                    best = d;
                    bestExt = e;
                    result = dir.path + QLatin1Char('/') + file;
                }
                break;
            }
        }
        if (!result.isEmpty())
            break;
    }
    if (result.isEmpty()) {
        for (const QString &dir : m_config.pixmapDirs) {
            for (const char *ext : kExtensions) {
                const QString file = dir + QLatin1Char('/') + name + QLatin1String(ext);
                if (QFileInfo(file).isFile()) {
                    result = file;
                    break;
                }
            }
            if (!result.isEmpty())
                break;
        }
    }

    QMutexLocker lock(&m_mutex);
    if (generation != m_generation)
        return result;   // invalidated meanwhile: don't publish stale data
    if (!built.isEmpty()) {
        auto next = std::make_shared<ThemeMap>(*m_themes);
        for (auto it = built.cbegin(); it != built.cend(); ++it) {
            if (!next->contains(it.key()))
                next->insert(it.key(), it.value());
        }
        m_themes = std::move(next);
    }
    m_cache.insert(cacheKey, result);
    return result;
}
