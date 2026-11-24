#include "artworkscraper.h"

#include "apikeystore.h"
#include "jsonfile.h"
#include "steamgriddb.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lcScraper, "mightyos.artwork.scraper", QtWarningMsg)

namespace {

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString defaultConfigDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/mightyos");
}

const QRegularExpression &fileNameRe()
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{40}\\.(png|jpg)$"));
    return re;
}

QString fileStemOf(const QString &key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
}

} // namespace

QString ArtworkScraper::defaultDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/mightyos/artwork");
}

ArtworkScraper::ArtworkScraper(const Config &config, HttpFetcher *fetcher, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_dir(config.dir.isEmpty() ? defaultDir() : config.dir)
    , m_fetcher(fetcher ? fetcher : new NetworkFetcher)
    , m_store(std::make_shared<ApiKeyStore>(config.configDir.isEmpty() ? defaultConfigDir() : config.configDir,
                                            config.useSecretService))
{
    m_fetcher->setParent(nullptr);   // owned by the unique_ptr
    m_keyPool.setMaxThreadCount(1);
    m_keyPool.setObjectName(QStringLiteral("mightyos-apikey"));
    m_pumpTimer.setSingleShot(true);
    connect(&m_pumpTimer, &QTimer::timeout, this, &ArtworkScraper::pump);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(5000);
    connect(&m_saveTimer, &QTimer::timeout, this, &ArtworkScraper::saveIndex);

    // Index: read on a worker; entries added meanwhile win.
    connect(&m_indexLoad, &QFutureWatcher<Index>::finished, this, [this] {
        Index loaded = m_indexLoad.result();
        for (auto it = m_index.cbegin(); it != m_index.cend(); ++it)
            loaded.insert(it.key(), it.value());
        m_index = std::move(loaded);
        m_indexLoaded = true;
        schedule(0);
        QTimer::singleShot(2000, this, &ArtworkScraper::scheduleTrim);
    });
    const QString dir = m_dir;
    m_indexLoad.setFuture(QtConcurrent::run([dir] { return readIndex(dir); }));

    connect(&m_indexSave, &QFutureWatcher<bool>::finished, this, [this] {
        if (!m_indexSave.result())
            qCWarning(lcScraper) << "could not write the artwork index";
        if (std::exchange(m_saveAgain, false))
            saveIndex();
    });
    connect(&m_trim, &QFutureWatcher<TrimResult>::finished, this, [this] { applyTrim(m_trim.result()); });
    startKeyLoad();
}

ArtworkScraper::~ArtworkScraper()
{
    // The fetcher first: no network callback may run into a half-destroyed
    // scraper (its queued replies die with it).
    m_fetcher.reset();
    m_keyPool.waitForDone();
    m_imageIo.waitForFinished();
    m_indexLoad.waitForFinished();
    m_trim.waitForFinished();
    m_indexSave.waitForFinished();
    if (m_saveTimer.isActive() || m_saveAgain)
        writeIndex(m_dir, m_index);
}

ArtworkScraper *ArtworkScraper::create(QQmlEngine *, QJSEngine *)
{
    return new ArtworkScraper(Config());
}

QString ArtworkScraper::fileStem(const QString &key)
{
    return fileStemOf(key);
}

// --- API key ---------------------------------------------------------------

void ArtworkScraper::startKeyTask(std::function<void()> work, std::function<void()> done)
{
    m_keyBusy = true;
    emit keyBusyChanged();
    auto *watcher = new QFutureWatcher<void>(this);
    const QFuture<void> future = QtConcurrent::run(&m_keyPool, std::move(work));
    m_keyTasks.append(future);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, done = std::move(done)] {
        watcher->deleteLater();
        m_keyTasks.removeIf([](const QFuture<void> &f) { return f.isFinished(); });
        if (m_keyTasks.isEmpty()) {
            m_keyBusy = false;
            emit keyBusyChanged();
        }
        done();
    });
    watcher->setFuture(future);
}

void ArtworkScraper::startKeyLoad()
{
    const std::shared_ptr<ApiKeyStore> store = m_store;
    auto result = std::make_shared<std::optional<QString>>();
    auto backend = std::make_shared<ApiKeyStore::Backend>(ApiKeyStore::Backend::None);
    const quint64 generation = m_generation;
    startKeyTask([store, result, backend] {
        QString error;
        *result = store->read(&error);
        *backend = store->lastBackend();
        if (!error.isEmpty())
            qCWarning(lcScraper).noquote() << "SteamGridDB key:" << error;   // never the key
    }, [this, result, backend, generation] {
        if (generation != m_generation)
            return;   // a clear or a new key came after this read
        m_key = result->value_or(QString());
        if (m_key.isEmpty()) {   // requests queued while the key loaded
            m_queue.clear();
            m_queuedKeys.clear();
        }
        m_keyStorage = m_key.isEmpty() ? QString()
                     : *backend == ApiKeyStore::Backend::Secret ? tr("system keyring") : tr("private file");
        emit keyChanged();
        emit statusChanged();
        schedule(0);
    });
}

QString ArtworkScraper::maskedKey() const
{
    return ApiKeyStore::masked(m_key);
}

bool ArtworkScraper::setApiKey(const QString &rawKey)
{
    const QString key = ApiKeyStore::normalized(rawKey);
    if (!ApiKeyStore::isValidKey(key))
        return false;
    const std::shared_ptr<ApiKeyStore> store = m_store;
    auto ok = std::make_shared<bool>(false);
    auto backend = std::make_shared<ApiKeyStore::Backend>(ApiKeyStore::Backend::None);
    const quint64 generation = ++m_generation;
    startKeyTask([store, ok, backend, key] {
        QString error;
        *ok = store->write(key, &error);
        *backend = store->lastBackend();
        if (!*ok)
            qCWarning(lcScraper).noquote() << "could not store the SteamGridDB key:" << error;
    }, [this, ok, backend, key, generation] {
        if (generation != m_generation)
            return;   // cleared (or replaced) since
        if (!*ok) {
            emit keySaved(false, tr("Could not save the API key"));
            return;
        }
        m_key = key;
        m_authFailed = false;
        m_failures = 0;
        m_inFlight = false;
        m_keyStorage = *backend == ApiKeyStore::Backend::Secret ? tr("system keyring") : tr("private file");
        emit keyChanged();
        emit statusChanged();
        emit keySaved(true, tr("API key saved (%1)").arg(m_keyStorage));
        schedule(0);
    });
    return true;
}

void ArtworkScraper::clearApiKey()
{
    ++m_generation;   // late key, fetch and image callbacks are ignored
    m_key.clear();
    m_keyStorage.clear();
    m_queue.clear();
    m_queuedKeys.clear();
    m_current.reset();
    m_inFlight = false;
    m_authFailed = false;
    const std::shared_ptr<ApiKeyStore> store = m_store;
    startKeyTask([store] { store->clear(); }, [] {});   // after any pending write, in order
    emit keyChanged();
    emit statusChanged();
}

void ArtworkScraper::waitForKeyIo()
{
    m_keyPool.waitForDone();
    m_indexLoad.waitForFinished();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

// --- status ----------------------------------------------------------------

void ArtworkScraper::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    emit pausedChanged();
    emit statusChanged();
    if (!paused)
        schedule(0);
}

void ArtworkScraper::setStatusText(const QString &text)
{
    if (m_statusText == text)
        return;
    m_statusText = text;
    emit statusChanged();
}

QString ArtworkScraper::status() const
{
    if (m_key.isEmpty())
        return tr("Off: no API key");
    if (m_authFailed)
        return tr("SteamGridDB rejected the API key");
    if (m_paused && (!m_queue.isEmpty() || m_current))
        return tr("Paused while a game runs");
    if (!m_statusText.isEmpty())
        return m_statusText;
    return m_queue.isEmpty() && !m_current ? tr("Ready") : tr("Fetching artwork (%1 left)").arg(m_queue.size());
}

// --- index -----------------------------------------------------------------

ArtworkScraper::Index ArtworkScraper::readIndex(const QString &dir)
{
    Index index;
    if (dir.isEmpty())
        return index;
    const auto root = JsonFile::read(dir + QStringLiteral("/index.json"), kMaxIndexBytes);
    if (!root || root->value(QStringLiteral("version")).toInt(-1) != 1)
        return index;
    const QJsonObject items = root->value(QStringLiteral("items")).toObject();
    for (auto it = items.begin(); it != items.end() && index.size() < kMaxIndexEntries; ++it) {
        const QJsonObject o = it.value().toObject();
        Entry e;
        e.file = o.value(QStringLiteral("file")).toString();
        e.checked = qint64(o.value(QStringLiteral("checked")).toDouble());
        e.used = qint64(o.value(QStringLiteral("used")).toDouble());
        e.bytes = qint64(o.value(QStringLiteral("bytes")).toDouble());
        if (it.key().isEmpty() || it.key().size() > 512)
            continue;
        if (!e.file.isEmpty() && (!fileNameRe().match(e.file).hasMatch() || !e.file.startsWith(fileStemOf(it.key()))))
            continue;
        index.insert(it.key(), e);
    }
    return index;
}

bool ArtworkScraper::writeIndex(const QString &dir, const Index &index)
{
    if (dir.isEmpty() || !QDir().mkpath(dir))
        return false;
    QJsonObject items;
    for (auto it = index.cbegin(); it != index.cend(); ++it) {
        items.insert(it.key(), QJsonObject {
            { QStringLiteral("file"), it->file }, { QStringLiteral("checked"), double(it->checked) },
            { QStringLiteral("used"), double(it->used) }, { QStringLiteral("bytes"), double(it->bytes) } });
    }
    return JsonFile::write(dir + QStringLiteral("/index.json"),
                           QJsonObject { { QStringLiteral("version"), 1 }, { QStringLiteral("items"), items } });
}

void ArtworkScraper::saveIndexLater()
{
    if (!m_saveTimer.isActive())
        m_saveTimer.start();
}

void ArtworkScraper::saveIndex()
{
    m_saveTimer.stop();
    if (!m_indexLoaded)
        return saveIndexLater();   // never overwrite an index we haven't read
    if (m_indexSave.isRunning()) {
        m_saveAgain = true;
        return;
    }
    const QString dir = m_dir;
    const Index snapshot = m_index;   // implicitly shared: O(1)
    m_indexSave.setFuture(QtConcurrent::run([dir, snapshot] { return writeIndex(dir, snapshot); }));
}

QString ArtworkScraper::cachedPath(const QString &key)
{
    auto it = m_index.find(key);
    if (it == m_index.end() || it->file.isEmpty())
        return {};
    // LRU "use", at most once a day per item (no write per frame).
    if (now() - it->used > 24LL * 3600 * 1000) {
        it->used = now();
        saveIndexLater();
    }
    return m_dir + QLatin1Char('/') + it->file;
}

ArtworkScraper::TrimResult ArtworkScraper::trim(const QString &dir, const Index &index, qint64 budget)
{
    TrimResult result;
    if (dir.isEmpty())
        return result;
    const QDir d(dir);
    QSet<QString> known;
    for (const Entry &e : index) {
        if (!e.file.isEmpty())
            known.insert(e.file);
    }
    // Orphans: our naming pattern but not in the index.
    const QStringList files = d.entryList(QDir::Files | QDir::NoSymLinks | QDir::Hidden);
    const QSet<QString> present(files.cbegin(), files.cend());
    for (const QString &name : files) {
        if (fileNameRe().match(name).hasMatch() && !known.contains(name) && QFile::remove(d.absoluteFilePath(name)))
            ++result.removedFiles;
    }
    struct Item { QString key; QString file; qint64 used; qint64 bytes; };
    QList<Item> items;
    qint64 total = 0;
    for (auto it = index.cbegin(); it != index.cend(); ++it) {
        if (it->file.isEmpty())
            continue;
        if (!present.contains(it->file)) {
            result.removedKeys.append(it.key());   // the file is gone: forget it
            continue;
        }
        items.append({ it.key(), it->file, it->used, it->bytes });
        total += it->bytes;
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.used < b.used; });
    for (const Item &item : std::as_const(items)) {
        if (total <= budget)
            break;
        QFile::remove(d.absoluteFilePath(item.file));
        result.removedKeys.append(item.key);   // re-fetched if it's shown again
        total -= item.bytes;
        ++result.removedFiles;
    }
    return result;
}

void ArtworkScraper::applyTrim(const TrimResult &result)
{
    for (const QString &key : result.removedKeys)
        m_index.remove(key);
    if (!result.removedKeys.isEmpty())
        saveIndexLater();
}

void ArtworkScraper::scheduleTrim()
{
    if (m_trim.isRunning() || !m_indexLoaded)
        return;
    const QString dir = m_dir;
    const Index snapshot = m_index;
    const qint64 budget = m_config.diskBudget;
    m_trim.setFuture(QtConcurrent::run([dir, snapshot, budget] { return trim(dir, snapshot, budget); }));
}

int ArtworkScraper::trimDiskCache()
{
    m_indexLoad.waitForFinished();
    m_trim.waitForFinished();
    QCoreApplication::processEvents();   // apply a finished background trim first
    const QString dir = m_dir;
    const Index snapshot = m_index;
    const qint64 budget = m_config.diskBudget;
    QFuture<TrimResult> future = QtConcurrent::run([dir, snapshot, budget] { return trim(dir, snapshot, budget); });
    future.waitForFinished();
    const TrimResult result = future.result();
    applyTrim(result);
    return result.removedFiles;
}

// --- queue -----------------------------------------------------------------

void ArtworkScraper::request(const QString &key, const QString &term, const QString &steamAppId, bool front)
{
    if (m_key.isEmpty() && !m_keyBusy)
        return;   // opt-in: no key, no requests
    if (key.isEmpty() || m_queuedKeys.contains(key) || (m_current && m_current->key == key))
        return;
    if (term.trimmed().isEmpty() && steamAppId.isEmpty())
        return;
    const auto it = m_index.constFind(key);
    if (it != m_index.cend()) {
        if (!it->file.isEmpty())
            return;   // cached (a vanished file is noticed by the next trim)
        if (now() - it->checked < kNegativeTtlDays * 24LL * 3600 * 1000)
            return;
    }
    if (m_queue.size() >= kMaxQueue)
        return;
    Job job;
    job.key = key;
    job.term = term;
    job.appid = steamAppId;
    if (front)
        m_queue.prepend(job);
    else
        m_queue.append(job);
    m_queuedKeys.insert(key);
    emit statusChanged();
    schedule(0);
}

void ArtworkScraper::schedule(int delayMs)
{
    if (!m_pumpTimer.isActive() || m_pumpTimer.remainingTime() > delayMs)
        m_pumpTimer.start(qMax(0, delayMs));
}

void ArtworkScraper::pump()
{
    if (m_key.isEmpty() || m_authFailed || m_paused || m_inFlight || m_imageBusy || !m_indexLoaded)
        return;
    if (!m_current) {
        // Requests queued before the index loaded may be cached already.
        while (!m_queue.isEmpty()) {
            Job next = m_queue.takeFirst();
            m_queuedKeys.remove(next.key);
            const auto cached = m_index.constFind(next.key);
            if (cached != m_index.cend() && !cached->file.isEmpty()) {
                emit artworkReady(next.key, m_dir + QLatin1Char('/') + cached->file);
                continue;
            }
            if (cached != m_index.cend() && now() - cached->checked < kNegativeTtlDays * 24LL * 3600 * 1000)
                continue;
            m_current = next;
            break;
        }
        if (!m_current) {
            setStatusText(QString());
            emit statusChanged();
            return;
        }
    }
    Job &job = *m_current;
    const bool image = job.stage == 2;
    const qint64 at = image ? m_nextImageAt : m_nextApiAt;
    if (now() < at) {
        schedule(int(at - now()));
        return;
    }
    QUrl url;
    if (image)
        url = QUrl(job.imageUrl);
    else if (job.stage == 1)
        url = SteamGridDb::gridsForGameUrl(job.gameId);
    else if (!job.appid.isEmpty())
        url = SteamGridDb::gridsForSteamUrl(job.appid);
    else
        url = SteamGridDb::searchUrl(job.term);
    if (!url.isValid()) {
        finishJob(QString(), 0);
        return;
    }
    if (image)
        m_nextImageAt = now() + m_config.imageIntervalMs;
    else
        m_nextApiAt = now() + m_config.apiIntervalMs;
    m_inFlight = true;
    const quint64 generation = m_generation;
    const qint64 cap = image ? SteamGridDb::kMaxImageBytes : SteamGridDb::kMaxJsonBytes;
    m_fetcher->get(url, image ? QString() : m_key, cap, [this, generation](const HttpResponse &r) {
        if (generation != m_generation || !m_current)
            return;   // abandoned (key cleared or changed): ignore
        m_inFlight = false;
        Job &job = *m_current;
        if (r.error == HttpResponse::Error::Network || r.error == HttpResponse::Error::Timeout) {
            fail(true);
            return;
        }
        if (r.error != HttpResponse::Error::None) {
            fail(false);   // blocked host, redirect, oversized: this item only
            return;
        }
        if (r.status == 401 || r.status == 403) {
            if (job.stage == 2) {
                fail(false);
                return;
            }
            m_authFailed = true;
            m_queue.prepend(job);
            m_queuedKeys.insert(job.key);
            m_current.reset();
            qCWarning(lcScraper) << "SteamGridDB rejected the API key (HTTP" << r.status << ")";
            emit statusChanged();
            return;
        }
        if (r.status == 429 || r.status >= 500) {
            fail(true, r.retryAfterSeconds);
            return;
        }
        if (r.status != 200) {
            fail(false);   // 404: nothing for this game
            return;
        }
        m_failures = 0;
        if (job.stage == 0 && job.appid.isEmpty()) {
            const auto id = SteamGridDb::parseSearch(r.body);
            if (!id) {
                fail(false);
                return;
            }
            job.gameId = *id;
            job.stage = 1;
            schedule(0);
            return;
        }
        if (job.stage < 2) {
            const QUrl imageUrl = SteamGridDb::parseGrids(r.body);
            if (!imageUrl.isValid()) {
                fail(false);
                return;
            }
            job.imageUrl = imageUrl.toString(QUrl::FullyEncoded);
            job.stage = 2;
            schedule(0);
            return;
        }
        // Validate and write off the GUI thread.
        const QString dir = m_dir;
        const QString stem = fileStem(job.key);
        const QByteArray body = r.body;
        const QByteArray type = r.contentType;
        m_imageBusy = true;
        m_imageIo.disconnect(this);
        connect(&m_imageIo, &QFutureWatcher<QString>::finished, this, [this, body, generation] {
            m_imageBusy = false;
            if (generation != m_generation || !m_current) {
                schedule(0);
                return;
            }
            const QString file = m_imageIo.result();
            if (file.isEmpty())
                fail(false);
            else
                finishJob(file, body.size());
        });
        m_imageIo.setFuture(QtConcurrent::run([dir, stem, body, type]() -> QString {
            QString error;
            const QString ext = SteamGridDb::validateImage(body, type, &error);
            if (ext.isEmpty() || dir.isEmpty() || !QDir().mkpath(dir))
                return {};
            QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            const QString name = stem + QLatin1Char('.') + ext;
            QSaveFile out(dir + QLatin1Char('/') + name);
            if (!out.open(QIODevice::WriteOnly) || out.write(body) != body.size() || !out.commit())
                return {};
            QFile::remove(dir + QLatin1Char('/') + stem + (ext == QLatin1String("png") ? QStringLiteral(".jpg")
                                                                                        : QStringLiteral(".png")));
            return name;
        }));
    });
}

void ArtworkScraper::fail(bool transient, int retryAfterSeconds)
{
    if (!m_current)
        return;
    if (transient) {
        ++m_failures;
        const qint64 backoff = qMin<qint64>(kBackoffMaxMs, qint64(m_config.backoffBaseMs) << qMin(m_failures - 1, 16));
        const qint64 delay = retryAfterSeconds >= 0 ? qMax<qint64>(retryAfterSeconds * 1000LL, m_config.backoffBaseMs)
                                                    : backoff;
        setStatusText(tr("Offline or busy, retrying in %1 s").arg((delay + 999) / 1000));
        schedule(int(delay));   // the same job is retried
        return;
    }
    finishJob(QString(), 0);
}

void ArtworkScraper::finishJob(const QString &file, qint64 bytes)
{
    if (!m_current)
        return;
    const QString key = m_current->key;
    m_current.reset();
    Entry e;
    e.file = file;
    e.checked = now();
    e.used = now();
    e.bytes = bytes;
    if (m_index.size() < kMaxIndexEntries || m_index.contains(key))
        m_index.insert(key, e);
    saveIndexLater();
    setStatusText(QString());
    if (!file.isEmpty()) {
        ++m_fetched;
        emit artworkReady(key, m_dir + QLatin1Char('/') + file);
        if (++m_downloadsSinceTrim >= kTrimEveryDownloads) {
            m_downloadsSinceTrim = 0;
            scheduleTrim();
        }
    }
    emit statusChanged();
    schedule(0);
}
