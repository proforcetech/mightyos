#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>

// Finds freedesktop icon theme icons (Icon Theme Specification) without
// QIcon, so it can run on worker threads (QIcon's theme loader is
// GUI-thread only).
//
// Lookup: for each theme in the chain (configured themes, their Inherits=,
// then hicolor), the best-matching size across all base dirs; then the
// flat pixmap dirs. SVG is preferred over PNG over XPM at equal distance.
// Each existing theme directory is listed once and kept as a set of file
// names, so a lookup is a few hash probes instead of hundreds of stats
// (hicolor's index alone lists ~650 directories).
//
// Locking: the theme index is an immutable snapshot. find() builds missing
// themes (directory listings) *outside* the mutex and swaps a new snapshot
// in, so a lookup on one thread never waits for another thread's I/O.
// refreshIfChanged() (called by the app scan on its worker thread) drops
// the index only if an indexed directory's mtime changed (icons installed
// or removed); invalidate() drops it unconditionally.
//
// Thread-safe. Names must pass Ids::isValidIconName() (no '/', no leading
// '.'), so a name can never walk out of a theme directory.
class IconLocator
{
public:
    struct Config {
        QStringList baseDirs;     // e.g. ~/.local/share/icons, /usr/share/icons
        QStringList pixmapDirs;   // e.g. /usr/share/pixmaps
        QStringList themes;       // preferred first; hicolor is always appended
    };

    explicit IconLocator(Config config);

    // From the environment: $XDG_DATA_HOME, the Flatpak export dirs and
    // $XDG_DATA_DIRS (same order as AppRegistry), ~/.icons. `themeName` is
    // QIcon::themeName() captured on the GUI thread (may be empty).
    static Config defaultConfig(const QString &themeName);

    // Process-wide instance (created with defaultConfig() on first use).
    static IconLocator &shared();
    static void setSharedThemeName(const QString &themeName);

    // Absolute path of the best icon for `size` px, or empty.
    QString find(const QString &name, int size) const;
    // Drops the index if any indexed directory (or a base/pixmap dir)
    // changed since it was built. Does I/O; call it off the GUI thread.
    // Returns true if the index was dropped.
    bool refreshIfChanged() const;
    void invalidate() const;

    const Config &config() const { return m_config; }

private:
    enum class DirType { Fixed, Scalable, Threshold };
    struct ThemeDir {
        QString path;             // absolute, existing
        QSet<QString> files;      // file names in it
        qint64 mtime = 0;         // when it was listed
        DirType type = DirType::Threshold;
        int size = 0, minSize = 0, maxSize = 0, threshold = 2, scale = 1;
    };
    struct Theme {
        QList<ThemeDir> dirs;
        QStringList inherits;
        QHash<QString, qint64> rootMtimes;   // <base>/<theme> -> mtime
    };
    using ThemeMap = QHash<QString, Theme>;

    static Theme loadTheme(const Config &config, const QString &name);   // I/O, no locks
    static int distance(const ThemeDir &dir, int size);
    QHash<QString, qint64> baseMtimes() const;

    Config m_config;
    mutable QMutex m_mutex;
    mutable std::shared_ptr<const ThemeMap> m_themes;
    mutable QHash<QString, QString> m_cache;    // "name@size" -> path ("" = none)
    mutable QHash<QString, qint64> m_baseMtimes;
    mutable quint64 m_generation = 0;
};
