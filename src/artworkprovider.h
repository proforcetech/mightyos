#pragma once

#include <QAtomicInt>
#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QSize>
#include <QString>
#include <QThreadPool>
#include <atomic>

class IconLocator;
class QQmlEngine;

// image://art/<id> — offline artwork for tiles (PLAN M2; SteamGridDB is M4).
//
// <id> is base64url (no padding) of a spec:
//   "theme:<icon name>"  freedesktop icon theme lookup (IconLocator)
//   "file:<abs path>"    an absolute icon path from a .desktop file/manifest
// urlFor(spec) builds the URL; models put it in their `artwork` role, so
// QML never builds or escapes paths itself.
//
// Each request decodes on the provider's own small thread pool. Requested
// sizes are rounded up to multiples of kSizeBucket (so 112 and 120 px share
// one entry); SVGs are rendered at that size, large rasters decoded scaled
// down. Results go to
//   - a memory LRU bounded by bytes (kMemoryBudgetBytes), and
//   - a disk cache, $XDG_CACHE_HOME/mightyos/thumbs/<sha1>.png, keyed on
//     the resolved file path + its mtime + its byte size + the size bucket
//     (a changed icon is re-rendered). Small raster sources (at most
//     kSmallRasterBytes) are not written: decoding them again is about as
//     cheap as reading a thumbnail. The disk cache is an LRU by total bytes
//     (diskBudget(), default kDefaultDiskBudgetBytes): a hit refreshes the
//     file's mtime, and trimDiskCache() removes the oldest files. It runs at
//     startup and after every kTrimEveryWrites new thumbnails, on the pool.
// Anything that fails validation or decoding is an error response; Tile
// then keeps showing its built-in vector icon (the fallback).
//
// Untrusted input: names must pass Ids::isValidIconName(), paths must be
// clean absolute paths with an image extension (png, svg, xpm, jpg/jpeg;
// no compressed svgz, whose decompressed size can't be capped), files are
// read through an fd check (regular file, capped at kMaxSourceBytes), the
// decoder is forced to the format the extension names (no content
// sniffing), and dimensions are capped before decoding.
class ArtworkProvider : public QQuickAsyncImageProvider
{
public:
    static constexpr qint64 kMaxSourceBytes = 8 * 1024 * 1024;
    static constexpr int kMaxSourceDimension = 4096;
    static constexpr int kMaxRequestDimension = 1024;
    static constexpr int kDefaultSize = 256;
    static constexpr int kSizeBucket = 64;
    static constexpr qsizetype kMemoryBudgetBytes = 24 * 1024 * 1024;
    static constexpr qint64 kDefaultDiskBudgetBytes = 256LL * 1024 * 1024;
    static constexpr qint64 kSmallRasterBytes = 16 * 1024;
    static constexpr int kTrimEveryWrites = 32;

    struct Stats {
        int memoryHits = 0;
        int diskHits = 0;
        int decodes = 0;
        int failures = 0;
        int diskWrites = 0;
        int evictions = 0;
    };

    // `cacheDir` empty: default ($XDG_CACHE_HOME/mightyos/thumbs).
    // `icons` null: IconLocator::shared(). Not owned.
    explicit ArtworkProvider(const QString &cacheDir = QString(), const IconLocator *icons = nullptr,
                             qint64 diskBudget = kDefaultDiskBudgetBytes);
    ~ArtworkProvider() override;

    // Registers a new provider as "art" on `engine` (which takes ownership).
    static void install(QQmlEngine *engine);

    static QString urlFor(const QString &spec);
    static QString defaultCacheDir();
    // Decodes a URL id back to its spec; empty if malformed.
    static QString specFromId(const QString &id);
    // The size actually decoded for a request (bucketed, capped).
    static QSize bucketSize(const QSize &requested);

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // Synchronous load (what the worker runs). Thread-safe.
    QImage load(const QString &spec, const QSize &requestedSize, QString *error = nullptr);

    qint64 diskBudget() const { return m_diskBudget.load(); }
    void setDiskBudget(qint64 bytes) { m_diskBudget.store(qMax<qint64>(0, bytes)); }
    // Deletes the least recently used thumbnails until the cache fits the
    // budget. Thread-safe; returns the number of files removed.
    int trimDiskCache();

    Stats stats() const;
    QString cacheDir() const { return m_cacheDir; }
    QThreadPool *pool() { return &m_pool; }

private:
    QString resolve(const QString &spec, int size, QString *error) const;
    void scheduleTrim();

    QString m_cacheDir;
    const IconLocator *m_icons;
    std::atomic<qint64> m_diskBudget;
    QThreadPool m_pool;
    mutable QMutex m_mutex;
    QMutex m_trimMutex;
    QCache<QString, QImage> m_memory;
    QAtomicInt m_memoryHits, m_diskHits, m_decodes, m_failures, m_diskWrites, m_evictions;
    QAtomicInt m_writesSinceTrim;
};
