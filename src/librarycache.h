#pragma once

#include "romscanner.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

// The ROM library cache, $XDG_CACHE_HOME/mightyos/library.sqlite (PLAN
// section 2), so the grids fill instantly at startup and re-scans only
// write what changed. Shared by all profiles.
//
// A LibraryCache object owns one QSqlDatabase connection with a unique
// name and must be created, used and destroyed on one thread (the scan
// worker). The file is opened with QSQLITE_OPEN_NOFOLLOW; the directory is
// created 0700 and the file chmod 0600. Anything wrong with it (not a
// regular file, a corrupt database, another schema version) means it is
// deleted and recreated: it only caches what a scan can rebuild.
//
// Schema v2:  meta(key TEXT PRIMARY KEY, value TEXT)
//             roms(id TEXT PRIMARY KEY, system, root, relpath, title,
//                  region, tags, discs, disc, size, mtime)
// (v1 lacked `disc`; any other version is recreated.) Every row read back
// is validated again (ids, clean paths, lengths) and its display strings
// sanitised (TextSanitize::label), like any other file under the user's
// home.
class LibraryCache
{
public:
    static constexpr int kSchemaVersion = 2;

    explicit LibraryCache(const QString &path);
    ~LibraryCache();
    LibraryCache(const LibraryCache &) = delete;
    LibraryCache &operator=(const LibraryCache &) = delete;

    static QString defaultPath();

    bool open();
    bool isOpen() const { return m_open; }
    QString path() const { return m_path; }

    // All cached ROMs (validated), by id.
    QHash<QString, RomRecord> load();
    // Makes the cache equal to `records` for the systems in `systems`:
    // deletes rows that are gone, inserts or replaces new and changed ones,
    // in one transaction. `previous` is what load() returned (the diff
    // base). Returns false (and changes nothing) on error.
    // `cancel` (may be null) is checked between rows: a cancelled sync rolls
    // back and returns false.
    bool sync(const QList<RomRecord> &records, const QSet<QString> &systems,
              const QHash<QString, RomRecord> &previous, int *written = nullptr, int *deleted = nullptr,
              const std::atomic_bool *cancel = nullptr);

private:
    bool createSchema();
    void recreate();

    QString m_path;
    QString m_connection;
    bool m_open = false;
};
