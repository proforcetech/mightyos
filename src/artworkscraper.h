#pragma once

#include <QDateTime>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <optional>

class ApiKeyStore;
class HttpFetcher;
class QJSEngine;
class QQmlEngine;

// SteamGridDB artwork for library tiles (PLAN D11). Opt-in: nothing is
// requested until the user has entered their own API key (Settings >
// Artwork). Failures of any kind degrade silently to the tinted
// placeholder tiles.
//
// Flow per game: [search by title (ROMs) | Steam appid] -> grids (600x900,
// static PNG/JPEG, no NSFW/humor) -> image from the CDN -> validated off
// the GUI thread -> <data>/mightyos/artwork/<sha1(item key)>.<png|jpg>.
// File names never come from the network.
//
// Politeness and robustness:
//  - one request at a time; API calls at least kApiIntervalMs apart,
//    image downloads kImageIntervalMs apart;
//  - 429 / 5xx / network errors back off exponentially (kBackoffBaseMs,
//    doubling, capped at kBackoffMaxMs, or the server's Retry-After) and
//    retry the same item; 401/403 stop everything until a new key is set;
//  - "no artwork found" is remembered for kNegativeTtlDays;
//  - paused while a game runs (`paused`, bound to Launcher.running).
//
// Disk cache: index.json (validated, capped) + files; an LRU budget
// (kDiskBudgetBytes) by last use, trimmed at startup and every
// kTrimEveryDownloads downloads; orphaned files are removed. Loading and
// saving the index and trimming run on worker threads; lookups before the
// index has loaded are answered when a queued request reaches the pump
// (a cached file is then reported without any network request).
//
// The key is read, written and cleared on one worker thread, in order (the
// Secret Service can block) and is only held in memory; maskedKey shows
// the last 4 chars. Every network or image callback carries a generation
// number; clearing or changing the key bumps it, so late callbacks of an
// abandoned request are ignored.
class ArtworkScraper : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Scraper)
    QML_SINGLETON

    Q_PROPERTY(bool hasKey READ hasKey NOTIFY keyChanged FINAL)
    Q_PROPERTY(QString maskedKey READ maskedKey NOTIFY keyChanged FINAL)
    Q_PROPERTY(bool keyBusy READ isKeyBusy NOTIFY keyBusyChanged FINAL)
    Q_PROPERTY(QString keyStorage READ keyStorage NOTIFY keyChanged FINAL)
    Q_PROPERTY(bool paused READ isPaused WRITE setPaused NOTIFY pausedChanged FINAL)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged FINAL)
    Q_PROPERTY(int queued READ queued NOTIFY statusChanged FINAL)
    Q_PROPERTY(int fetched READ fetched NOTIFY statusChanged FINAL)

public:
    static constexpr int kApiIntervalMs = 1100;
    static constexpr int kImageIntervalMs = 300;
    static constexpr int kBackoffBaseMs = 5000;
    static constexpr int kBackoffMaxMs = 10 * 60 * 1000;
    static constexpr int kNegativeTtlDays = 7;
    static constexpr int kMaxQueue = 50000;
    static constexpr int kMaxIndexEntries = 60000;
    static constexpr qint64 kMaxIndexBytes = 8 * 1024 * 1024;
    static constexpr qint64 kDiskBudgetBytes = 256LL * 1024 * 1024;
    static constexpr int kTrimEveryDownloads = 20;

    struct Config {
        QString dir;           // artwork dir; empty: default
        QString configDir;     // for ApiKeyStore; empty: default
        bool useSecretService = true;
        qint64 diskBudget = kDiskBudgetBytes;
        int apiIntervalMs = kApiIntervalMs;
        int imageIntervalMs = kImageIntervalMs;
        int backoffBaseMs = kBackoffBaseMs;
    };

    // Takes ownership of `fetcher` (null: the real network).
    explicit ArtworkScraper(const Config &config, HttpFetcher *fetcher = nullptr, QObject *parent = nullptr);
    ~ArtworkScraper() override;
    static ArtworkScraper *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);
    static QString defaultDir();

    bool hasKey() const { return !m_key.isEmpty(); }
    QString maskedKey() const;
    bool isKeyBusy() const { return m_keyBusy; }
    QString keyStorage() const { return m_keyStorage; }
    bool isPaused() const { return m_paused; }
    void setPaused(bool paused);
    QString status() const;
    int queued() const { return static_cast<int>(m_queue.size()); }
    int fetched() const { return m_fetched; }

    // Validates synchronously (returns false for an invalid key), stores
    // asynchronously; keySaved() reports the outcome.
    Q_INVOKABLE bool setApiKey(const QString &key);
    Q_INVOKABLE void clearApiKey();

    // Library side. Path of cached artwork for `key` from the index, or ""
    // (no file system access: called for every game of a grid).
    QString cachedPath(const QString &key);
    // Queue a lookup (ignored without a key, when cached, recently not
    // found or already queued). `steamAppId` non-empty: Steam grids by id,
    // else a title search for `term`. `front`: before older requests (the
    // grid the user is looking at).
    void request(const QString &key, const QString &term, const QString &steamAppId, bool front = false);
    // Waits for pending key I/O and the index load (tests).
    void waitForKeyIo();
    // Trims on a worker and waits for it (tests; normal trims don't wait).
    // Returns the number of files removed.
    int trimDiskCache();
    QString directory() const { return m_dir; }

signals:
    void keyChanged();
    void keyBusyChanged();
    void keySaved(bool ok, const QString &message);
    void pausedChanged();
    void statusChanged();
    void artworkReady(const QString &key, const QString &path);

private:
    struct Entry {
        QString file;       // "<sha1>.png" / ".jpg", or "" = not found
        qint64 checked = 0; // ms since epoch
        qint64 used = 0;
        qint64 bytes = 0;
    };
    struct Job {
        QString key, term, appid;
        qint64 gameId = 0;
        QString imageUrl;
        int stage = 0;   // 0 search/grids, 1 grids (after search), 2 image
    };

    using Index = QHash<QString, Entry>;
    struct TrimResult { QStringList removedKeys; int removedFiles = 0; };
    static Index readIndex(const QString &dir);
    static bool writeIndex(const QString &dir, const Index &index);
    static TrimResult trim(const QString &dir, const Index &index, qint64 budget);
    void applyTrim(const TrimResult &result);
    void scheduleTrim();
    void saveIndexLater();
    void saveIndex();
    void pump();
    void schedule(int delayMs);
    void fail(bool transient, int retryAfterSeconds = -1);
    void finishJob(const QString &file, qint64 bytes);
    void startKeyLoad();
    void startKeyTask(std::function<void()> work, std::function<void()> done);
    void setStatusText(const QString &text);
    static QString fileStem(const QString &key);

    Config m_config;
    QString m_dir;
    std::unique_ptr<HttpFetcher> m_fetcher;
    std::shared_ptr<ApiKeyStore> m_store;
    QString m_key;
    QString m_keyStorage;
    bool m_keyBusy = false;
    bool m_paused = false;
    bool m_authFailed = false;
    bool m_inFlight = false;
    bool m_imageBusy = false;
    bool m_indexLoaded = false;
    bool m_saveAgain = false;
    quint64 m_generation = 0;
    int m_failures = 0;
    int m_fetched = 0;
    int m_downloadsSinceTrim = 0;
    qint64 m_nextApiAt = 0;
    qint64 m_nextImageAt = 0;
    QString m_statusText;
    QList<Job> m_queue;
    QSet<QString> m_queuedKeys;
    std::optional<Job> m_current;
    QHash<QString, Entry> m_index;
    QTimer m_pumpTimer;
    QTimer m_saveTimer;
    QThreadPool m_keyPool;   // one thread: key tasks run in order
    QList<QFuture<void>> m_keyTasks;
    QFutureWatcher<QString> m_imageIo;
    QFutureWatcher<Index> m_indexLoad;
    QFutureWatcher<bool> m_indexSave;
    QFutureWatcher<TrimResult> m_trim;
};
