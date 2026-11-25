#include "artworkprovider.h"

#include "iconlocator.h"
#include "ids.h"
#include "jsonfile.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QRunnable>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <fcntl.h>
#include <memory>
#include <sys/stat.h>

Q_LOGGING_CATEGORY(lcArtwork, "mightyos.artwork", QtWarningMsg)

namespace {

// Extension -> the only image format the decoder may use for it.
QByteArray formatFor(const QString &suffix)
{
    if (suffix == QLatin1String("png")) return "png";
    if (suffix == QLatin1String("svg")) return "svg";
    if (suffix == QLatin1String("xpm")) return "xpm";
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) return "jpeg";
    return {};
}

} // namespace

QSize ArtworkProvider::bucketSize(const QSize &requested)
{
    int w = requested.width() > 0 ? requested.width() : 0;
    int h = requested.height() > 0 ? requested.height() : 0;
    if (w == 0 && h == 0)
        w = h = kDefaultSize;
    else if (w == 0)
        w = h;
    else if (h == 0)
        h = w;
    const int longest = qMax(w, h);
    int bucket = ((longest + kSizeBucket - 1) / kSizeBucket) * kSizeBucket;
    bucket = qBound(kSizeBucket, bucket, kMaxRequestDimension);
    return { qMax(1, qRound(double(w) * bucket / longest)), qMax(1, qRound(double(h) * bucket / longest)) };
}

namespace {

// Qt's documented async-provider pattern: a QObject runnable that emits
// its result; the response receives it through a queued connection, which
// Qt drops automatically if the response is gone by then.
class ArtJob : public QObject, public QRunnable
{
    Q_OBJECT

public:
    ArtJob(ArtworkProvider *provider, QString spec, QSize size, std::shared_ptr<std::atomic_bool> cancelled)
        : m_provider(provider), m_spec(std::move(spec)), m_size(size), m_cancelled(std::move(cancelled))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QImage image;
        QString error;
        if (m_cancelled->load())
            error = QStringLiteral("cancelled");
        else
            image = m_provider->load(m_spec, m_size, &error);
        emit done(image, image.isNull() && error.isEmpty() ? QStringLiteral("no image") : error);
    }

signals:
    void done(const QImage &image, const QString &error);

private:
    ArtworkProvider *m_provider;   // outlives the job: its destructor waits for the pool
    QString m_spec;
    QSize m_size;
    std::shared_ptr<std::atomic_bool> m_cancelled;
};

class ArtResponse : public QQuickImageResponse
{
    Q_OBJECT

public:
    // `pool` null: fail with `error` (asynchronously; the engine connects
    // to finished() only after requestImageResponse() returns).
    ArtResponse(ArtworkProvider *provider, const QString &spec, const QSize &size, QThreadPool *pool,
                const QString &error = QString())
    {
        if (!pool) {
            QMetaObject::invokeMethod(this, [this, error] { handleDone({}, error); }, Qt::QueuedConnection);
            return;
        }
        auto *job = new ArtJob(provider, spec, size, m_cancelled);
        connect(job, &ArtJob::done, this, &ArtResponse::handleDone, Qt::QueuedConnection);
        pool->start(job);
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }
    void cancel() override { m_cancelled->store(true); }

private:
    void handleDone(const QImage &image, const QString &error)
    {
        m_image = image;
        m_error = image.isNull() ? error : QString();
        emit finished();
    }

    QImage m_image;
    QString m_error;
    std::shared_ptr<std::atomic_bool> m_cancelled = std::make_shared<std::atomic_bool>(false);
};

} // namespace

ArtworkProvider::ArtworkProvider(const QString &cacheDir, const IconLocator *icons, qint64 diskBudget)
    : m_cacheDir(cacheDir.isEmpty() ? defaultCacheDir() : cacheDir)
    , m_icons(icons ? icons : &IconLocator::shared())
    , m_diskBudget(diskBudget)
{
    // Two decoders keep a page of new tiles quick without starving the
    // render thread on 2-4 core mini PCs.
    m_pool.setMaxThreadCount(2);
    m_pool.setObjectName(QStringLiteral("mightyos-artwork"));
    m_memory.setMaxCost(kMemoryBudgetBytes);
    scheduleTrim();   // startup trim, off the GUI thread
}

ArtworkProvider::~ArtworkProvider()
{
    m_pool.clear();
    m_pool.waitForDone();
}

ArtworkProvider *ArtworkProvider::install(QQmlEngine *engine)
{
    // QIcon's theme name is only safe to read on the GUI thread.
    IconLocator::setSharedThemeName(QIcon::themeName());
    auto *provider = new ArtworkProvider;
    engine->addImageProvider(QStringLiteral("art"), provider);
    return provider;
}

qsizetype ArtworkProvider::trimMemory()
{
    QMutexLocker locker(&m_mutex);
    const qsizetype bytes = m_memory.totalCost();
    m_memory.clear();
    return bytes;
}

QString ArtworkProvider::defaultCacheDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/mightyos/thumbs");
}

QString ArtworkProvider::urlFor(const QString &spec)
{
    if (spec.isEmpty())
        return {};
    const QByteArray encoded = spec.toUtf8().toBase64(QByteArray::Base64UrlEncoding
                                                     | QByteArray::OmitTrailingEquals);
    return QStringLiteral("image://art/") + QString::fromLatin1(encoded);
}

QString ArtworkProvider::specFromId(const QString &id)
{
    if (id.isEmpty() || id.size() > 8192)
        return {};
    for (const QChar c : id) {
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('-') && c != QLatin1Char('_'))
            return {};
    }
    const auto decoded = QByteArray::fromBase64Encoding(id.toLatin1(), QByteArray::Base64UrlEncoding
                                                        | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return {};
    return QString::fromUtf8(*decoded);
}

QQuickImageResponse *ArtworkProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    const QString spec = specFromId(id);
    if (spec.isEmpty())
        return new ArtResponse(this, spec, requestedSize, nullptr, QStringLiteral("malformed artwork id"));
    return new ArtResponse(this, spec, requestedSize, &m_pool);
}

QString ArtworkProvider::resolve(const QString &spec, int size, QString *error) const
{
    auto fail = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return QString();
    };
    QString path;
    if (spec.startsWith(QLatin1String("theme:"))) {
        const QString name = spec.mid(6);
        if (!Ids::isValidIconName(name))
            return fail("invalid icon name");
        path = m_icons->find(name, size);
        if (path.isEmpty())
            return fail("icon not found");
    } else if (spec.startsWith(QLatin1String("file:"))) {
        path = spec.mid(5);
        if (!Ids::isCleanAbsolutePath(path))
            return fail("not a clean absolute path");
    } else {
        return fail("unknown artwork scheme");
    }
    const QFileInfo info(path);
    if (formatFor(info.suffix().toLower()).isEmpty())
        return fail("not an image file type");
    if (!info.isFile() || !info.isReadable())
        return fail("missing or unreadable");
    if (info.size() > kMaxSourceBytes)
        return fail("image file too large");
    return path;
}

QImage ArtworkProvider::load(const QString &spec, const QSize &requestedSize, QString *error)
{
    auto failed = [this, error](const QString &why) {
        m_failures.ref();
        if (error && error->isEmpty())
            *error = why;
        return QImage();
    };
    if (m_disabled.load())
        return failed(QStringLiteral("safe mode"));
    const QSize target = bucketSize(requestedSize);
    const QString path = resolve(spec, qMax(target.width(), target.height()), error);
    if (path.isEmpty())
        return failed(QStringLiteral("unresolved"));
    const QFileInfo info(path);
    const QByteArray format = formatFor(info.suffix().toLower());
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        (path + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch())
         + QLatin1Char('|') + QString::number(info.size()) + QLatin1Char('|')
         + QString::number(target.width()) + QLatin1Char('x') + QString::number(target.height())).toUtf8(),
        QCryptographicHash::Sha1).toHex());

    {
        QMutexLocker lock(&m_mutex);
        if (const QImage *cached = m_memory.object(key)) {
            m_memoryHits.ref();
            return *cached;
        }
    }

    auto remember = [this, &key](const QImage &image) {
        QMutexLocker lock(&m_mutex);
        const qsizetype cost = qMax<qsizetype>(1, image.sizeInBytes());
        m_memory.insert(key, new QImage(image), cost);
    };

    const QString cacheFile = m_cacheDir.isEmpty() ? QString()
                                                   : m_cacheDir + QLatin1Char('/') + key + QStringLiteral(".png");
    if (!cacheFile.isEmpty()) {
        if (const auto bytes = JsonFile::readRegularFile(cacheFile, kMaxSourceBytes, false)) {
            QBuffer buffer;
            buffer.setData(*bytes);
            QImageReader reader(&buffer, "png");
            reader.setAutoDetectImageFormat(false);
            const QImage image = reader.read();
            if (!image.isNull()) {
                m_diskHits.ref();
                // LRU: a hit makes the thumbnail the most recently used.
                ::utimensat(AT_FDCWD, QFile::encodeName(cacheFile).constData(), nullptr, 0);
                remember(image);
                return image;
            }
        }
    }

    // Read through the fd checks (regular file, capped; a FIFO or device
    // named like an icon can't block or flood the decoder), then decode
    // strictly as the format the extension names.
    const auto bytes = JsonFile::readRegularFile(path, kMaxSourceBytes, true);
    if (!bytes)
        return failed(QStringLiteral("unreadable or too large"));
    QBuffer buffer;
    buffer.setData(*bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format);
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    reader.setAutoTransform(true);
    if (!reader.canRead())
        return failed(QStringLiteral("content does not match the file type"));
    const QSize source = reader.size();
    if (source.isValid() && (source.width() > kMaxSourceDimension || source.height() > kMaxSourceDimension))
        return failed(QStringLiteral("image dimensions too large"));
    if (source.isValid() && !source.isEmpty())
        reader.setScaledSize(source.scaled(target, Qt::KeepAspectRatio));
    else
        reader.setScaledSize(target);
    QImage image = reader.read();
    if (image.isNull())
        return failed(reader.errorString());
    if (image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_decodes.ref();
    remember(image);

    // Small rasters decode about as fast as a thumbnail reads: no disk entry.
    const bool worthCaching = format == "svg" || info.size() > kSmallRasterBytes;
    if (worthCaching && !cacheFile.isEmpty() && QDir().mkpath(m_cacheDir)) {
        QFile::setPermissions(m_cacheDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QSaveFile out(cacheFile);
        if (out.open(QIODevice::WriteOnly) && image.save(&out, "png") && out.commit()) {
            m_diskWrites.ref();
            if (m_writesSinceTrim.fetchAndAddRelaxed(1) + 1 >= kTrimEveryWrites) {
                m_writesSinceTrim.storeRelaxed(0);
                scheduleTrim();
            }
        } else {
            qCDebug(lcArtwork) << "could not write thumbnail";
        }
    }
    return image;
}

void ArtworkProvider::scheduleTrim()
{
    if (m_cacheDir.isEmpty())
        return;
    m_pool.start([this] { trimDiskCache(); });
}

int ArtworkProvider::trimDiskCache()
{
    if (m_cacheDir.isEmpty())
        return 0;
    QMutexLocker lock(&m_trimMutex);   // one trim at a time
    QDir dir(m_cacheDir);
    // Only our own files: 40 hex digits + ".png", regular, not symlinks.
    static const QRegularExpression nameRe(QStringLiteral("^[0-9a-f]{40}\\.png$"));
    QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoSymLinks | QDir::Hidden, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    QFileInfoList ours;
    for (const QFileInfo &file : std::as_const(files)) {
        if (!nameRe.match(file.fileName()).hasMatch())
            continue;
        ours.append(file);
        total += file.size();
    }
    const qint64 budget = diskBudget();
    int removed = 0;
    for (const QFileInfo &file : std::as_const(ours)) {   // oldest first
        if (total <= budget)
            break;
        if (QFile::remove(file.absoluteFilePath())) {
            total -= file.size();
            ++removed;
        }
    }
    m_evictions.fetchAndAddRelaxed(removed);
    return removed;
}

ArtworkProvider::Stats ArtworkProvider::stats() const
{
    return { m_memoryHits.loadRelaxed(), m_diskHits.loadRelaxed(), m_decodes.loadRelaxed(),
             m_failures.loadRelaxed(), m_diskWrites.loadRelaxed(), m_evictions.loadRelaxed() };
}

#include "artworkprovider.moc"
