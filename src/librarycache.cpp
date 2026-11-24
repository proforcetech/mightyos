#include "librarycache.h"

#include "ids.h"
#include "systemregistry.h"
#include "textsanitize.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include <sys/stat.h>

Q_LOGGING_CATEGORY(lcLibraryCache, "mightyos.library.cache", QtWarningMsg)

LibraryCache::LibraryCache(const QString &path)
    : m_path(path)
    , m_connection(QStringLiteral("mightyos-library-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

LibraryCache::~LibraryCache()
{
    {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        if (db.isValid())
            db.close();
    }
    if (QSqlDatabase::contains(m_connection))
        QSqlDatabase::removeDatabase(m_connection);
}

QString LibraryCache::defaultPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/mightyos/library.sqlite");
}

void LibraryCache::recreate()
{
    {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        if (db.isValid())
            db.close();
    }
    for (const char *suffix : { "", "-journal", "-wal", "-shm" })
        QFile::remove(m_path + QLatin1String(suffix));
}

bool LibraryCache::open()
{
    if (m_open)
        return true;
    if (m_path.isEmpty() || !Ids::isCleanAbsolutePath(m_path))
        return false;
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        qCWarning(lcLibraryCache) << "Qt SQLite driver missing (libqt6sql6-sqlite); running without a cache";
        return false;
    }
    const QString dir = QFileInfo(m_path).absolutePath();
    if (!QDir().mkpath(dir))
        return false;
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    // A symlink, FIFO or directory under our name is not ours: replace it.
    struct stat st {};
    if (::lstat(QFile::encodeName(m_path).constData(), &st) == 0 && !S_ISREG(st.st_mode)) {
        qCWarning(lcLibraryCache) << "library cache is not a regular file; recreating it";
        if (S_ISDIR(st.st_mode) || !QFile::remove(m_path))
            return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        QSqlDatabase db = QSqlDatabase::contains(m_connection)
            ? QSqlDatabase::database(m_connection, false)
            : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
        db.setDatabaseName(m_path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_NOFOLLOW;QSQLITE_BUSY_TIMEOUT=2000"));
        if (db.open()) {
            QFile::setPermissions(m_path, QFile::ReadOwner | QFile::WriteOwner);
            if (createSchema()) {
                m_open = true;
                return true;
            }
        }
        qCWarning(lcLibraryCache) << "library cache unusable, recreating:" << db.lastError().text();
        recreate();
    }
    return false;
}

bool LibraryCache::createSchema()
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA journal_mode=WAL")) || !q.exec(QStringLiteral("PRAGMA synchronous=NORMAL")))
        return false;
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)")))
        return false;
    if (!q.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'schema'")))
        return false;
    if (q.next()) {
        if (q.value(0).toString() != QString::number(kSchemaVersion))
            return false;   // another layout: recreate
    } else {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral("INSERT INTO meta (key, value) VALUES ('schema', ?)"));
        ins.addBindValue(QString::number(kSchemaVersion));
        if (!ins.exec())
            return false;
    }
    return q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS roms (id TEXT PRIMARY KEY, system TEXT NOT NULL, root TEXT NOT NULL, "
        "relpath TEXT NOT NULL, title TEXT NOT NULL, region TEXT, tags TEXT, discs INTEGER, "
        "disc INTEGER, size INTEGER, mtime INTEGER)"))
        && q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS roms_system ON roms(system)"));
}

QHash<QString, RomRecord> LibraryCache::load()
{
    QHash<QString, RomRecord> out;
    if (!m_open)
        return out;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT id, system, root, relpath, title, region, tags, discs, disc, size, mtime FROM roms")))
        return out;
    int rejected = 0;
    while (q.next()) {
        RomRecord r;
        r.id = q.value(0).toString();
        r.system = q.value(1).toString();
        r.root = q.value(2).toString();
        r.relPath = q.value(3).toString();
        // Display strings: sanitised like the scanner does (the file may
        // have been edited by anything running as this user).
        r.title = TextSanitize::label(q.value(4).toString(), 120);
        r.region = TextSanitize::label(q.value(5).toString(), 64);
        r.tags = TextSanitize::label(q.value(6).toString(), 120);
        r.discs = qBound(1, q.value(7).toInt(), 99);
        r.disc = qBound(0, q.value(8).toInt(), 99);
        r.size = q.value(9).toLongLong();
        r.mtime = q.value(10).toLongLong();
        const bool ok = SystemRegistry::isValidSystemId(r.system) && Ids::isCleanAbsolutePath(r.root)
                        && !r.relPath.isEmpty() && !r.relPath.startsWith(QLatin1Char('/'))
                        && Ids::isCleanAbsolutePath(r.root + QLatin1Char('/') + r.relPath)
                        && r.id == RomScanner::romId(r.system, r.relPath)
                        && !r.title.isEmpty();
        if (!ok) {
            ++rejected;
            continue;
        }
        out.insert(r.id, r);
    }
    if (rejected > 0)
        qCWarning(lcLibraryCache) << "ignored" << rejected << "invalid cache rows";
    return out;
}

bool LibraryCache::sync(const QList<RomRecord> &records, const QSet<QString> &systems,
                        const QHash<QString, RomRecord> &previous, int *written, int *deleted,
                        const std::atomic_bool *cancel)
{
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };
    if (!m_open)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSet<QString> keep;
    keep.reserve(records.size());
    for (const RomRecord &r : records)
        keep.insert(r.id);
    int writes = 0, deletes = 0;
    if (!db.transaction())
        return false;
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM roms WHERE id = ?"));
    for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
        if (!systems.contains(it->system) || keep.contains(it.key()))
            continue;
        del.addBindValue(it.key());
        if (cancelled() || !del.exec()) {
            db.rollback();
            return false;
        }
        ++deletes;
    }
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral("INSERT OR REPLACE INTO roms (id, system, root, relpath, title, region, tags, "
                               "discs, disc, size, mtime) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    for (const RomRecord &r : records) {
        const auto prev = previous.constFind(r.id);
        if (prev != previous.cend() && *prev == r)
            continue;
        ins.addBindValue(r.id);
        ins.addBindValue(r.system);
        ins.addBindValue(r.root);
        ins.addBindValue(r.relPath);
        ins.addBindValue(r.title);
        ins.addBindValue(r.region);
        ins.addBindValue(r.tags);
        ins.addBindValue(r.discs);
        ins.addBindValue(r.disc);
        ins.addBindValue(r.size);
        ins.addBindValue(r.mtime);
        if (cancelled() || !ins.exec()) {
            db.rollback();
            return false;
        }
        ++writes;
    }
    if (!db.commit())
        return false;
    if (written)
        *written = writes;
    if (deleted)
        *deleted = deletes;
    return true;
}
