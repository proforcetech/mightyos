#include "artworkprovider.h"
#include "iconlocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QQuickTextureFactory>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <sys/stat.h>
#include <vector>

// ArtworkProvider + IconLocator: theme lookup, decode at the requested
// size, disk and memory cache hits, invalidation on change, untrusted ids.
class TestArtwork : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void locatorFindsBestSize();
    void locatorRejectsTraversal();
    void urlRoundTrip();
    void decodesAtRequestedSize();
    void cacheHits();
    void changedSourceIsRedecoded();
    void rejectsUntrustedSpecs_data();
    void rejectsUntrustedSpecs();
    void asyncResponse();
    void smallRastersSkipDisk();
    void lruEviction();
    void wrongContentIsRejected();
    void teardownWithJobsInFlight();
    void refreshOnlyWhenDirsChange();

private:
    void writePng(const QString &path, int size, QColor colour);
    void writeSvg(const QString &path);
    IconLocator::Config locatorConfig() const;

    std::unique_ptr<QTemporaryDir> m_tmp;
    QString m_icons, m_cache;
};

void TestArtwork::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());
    m_icons = m_tmp->path() + QStringLiteral("/icons");
    m_cache = m_tmp->path() + QStringLiteral("/cache/thumbs");

    // A tiny "Test" theme inheriting hicolor, plus hicolor itself.
    QVERIFY(QDir().mkpath(m_icons + QStringLiteral("/Test/48x48/apps")));
    QVERIFY(QDir().mkpath(m_icons + QStringLiteral("/hicolor/128x128/apps")));
    QVERIFY(QDir().mkpath(m_icons + QStringLiteral("/hicolor/32x32/apps")));
    QVERIFY(QDir().mkpath(m_icons + QStringLiteral("/hicolor/scalable/apps")));
    QFile index(m_icons + QStringLiteral("/Test/index.theme"));
    QVERIFY(index.open(QIODevice::WriteOnly));
    index.write("[Icon Theme]\nName=Test\nInherits=hicolor\nDirectories=48x48/apps\n\n"
                "[48x48/apps]\nSize=48\nType=Fixed\n");
    index.close();
    QFile hicolor(m_icons + QStringLiteral("/hicolor/index.theme"));
    QVERIFY(hicolor.open(QIODevice::WriteOnly));
    hicolor.write("[Icon Theme]\nName=Hicolor\nDirectories=32x32/apps,128x128/apps,scalable/apps,missing/apps\n\n"
                  "[32x32/apps]\nSize=32\nType=Threshold\n\n[128x128/apps]\nSize=128\nType=Threshold\n\n"
                  "[scalable/apps]\nSize=128\nMinSize=8\nMaxSize=512\nType=Scalable\n");
    hicolor.close();

    writePng(m_icons + QStringLiteral("/Test/48x48/apps/themed.png"), 48, Qt::red);
    writePng(m_icons + QStringLiteral("/hicolor/32x32/apps/sized.png"), 32, Qt::green);
    writePng(m_icons + QStringLiteral("/hicolor/128x128/apps/sized.png"), 128, Qt::blue);
    writeSvg(m_icons + QStringLiteral("/hicolor/scalable/apps/vector.svg"));
    writePng(m_icons + QStringLiteral("/hicolor/128x128/apps/vector.png"), 128, Qt::black);
}

void TestArtwork::writePng(const QString &path, int size, QColor colour)
{
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(colour);
    QVERIFY(image.save(path, "png"));
}

void TestArtwork::writeSvg(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
               "<rect width='24' height='24' fill='#ff0000'/></svg>");
}

IconLocator::Config TestArtwork::locatorConfig() const
{
    return { { m_icons }, { m_tmp->path() + QStringLiteral("/pixmaps") }, { QStringLiteral("Test") } };
}

void TestArtwork::locatorFindsBestSize()
{
    IconLocator locator(locatorConfig());
    QCOMPARE(locator.find(QStringLiteral("themed"), 48), m_icons + QStringLiteral("/Test/48x48/apps/themed.png"));
    // Inherited theme; the closest size wins.
    QCOMPARE(locator.find(QStringLiteral("sized"), 24), m_icons + QStringLiteral("/hicolor/32x32/apps/sized.png"));
    QCOMPARE(locator.find(QStringLiteral("sized"), 200), m_icons + QStringLiteral("/hicolor/128x128/apps/sized.png"));
    // A scalable SVG beats a PNG of the same distance.
    QCOMPARE(locator.find(QStringLiteral("vector"), 128), m_icons + QStringLiteral("/hicolor/scalable/apps/vector.svg"));
    QCOMPARE(locator.find(QStringLiteral("nothing"), 48), QString());
    // Pixmaps fallback; invalidate() picks up new files.
    QVERIFY(QDir().mkpath(m_tmp->path() + QStringLiteral("/pixmaps")));
    writePng(m_tmp->path() + QStringLiteral("/pixmaps/legacy.png"), 16, Qt::gray);
    QCOMPARE(locator.find(QStringLiteral("legacy"), 48), m_tmp->path() + QStringLiteral("/pixmaps/legacy.png"));
    writePng(m_icons + QStringLiteral("/Test/48x48/apps/late.png"), 48, Qt::red);
    QCOMPARE(locator.find(QStringLiteral("late"), 48), QString());     // index cached
    locator.invalidate();
    QVERIFY(!locator.find(QStringLiteral("late"), 48).isEmpty());
}

void TestArtwork::locatorRejectsTraversal()
{
    IconLocator locator(locatorConfig());
    QVERIFY(locator.find(QStringLiteral("../hicolor/32x32/apps/sized"), 32).isEmpty());
    QVERIFY(locator.find(QStringLiteral("/etc/passwd"), 32).isEmpty());
    QVERIFY(locator.find(QStringLiteral(".."), 32).isEmpty());
    QVERIFY(locator.find(QString(), 32).isEmpty());
}

void TestArtwork::urlRoundTrip()
{
    const QString spec = QStringLiteral("file:/usr/share/pixmaps/odd name+%20é.png");
    const QString url = ArtworkProvider::urlFor(spec);
    QVERIFY(url.startsWith(QStringLiteral("image://art/")));
    const QString id = url.mid(12);
    for (const QChar c : id)
        QVERIFY(c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'));
    QCOMPARE(ArtworkProvider::specFromId(id), spec);
    QCOMPARE(ArtworkProvider::specFromId(QStringLiteral("not base64!")), QString());
    QCOMPARE(ArtworkProvider::urlFor(QString()), QString());
}

void TestArtwork::decodesAtRequestedSize()
{
    IconLocator locator(locatorConfig());
    ArtworkProvider provider(m_cache, &locator);
    QString error;
    const QImage svg = provider.load(QStringLiteral("theme:vector"), QSize(96, 96), &error);
    QVERIFY2(!svg.isNull(), qPrintable(error));
    QCOMPARE(svg.size(), QSize(128, 128));                  // 96 -> bucket 128, rendered at size
    QCOMPARE(ArtworkProvider::bucketSize(QSize(112, 112)), QSize(128, 128));
    QCOMPARE(ArtworkProvider::bucketSize(QSize(120, 60)), QSize(128, 64));
    QCOMPARE(ArtworkProvider::bucketSize(QSize(10, 10)), QSize(64, 64));
    QCOMPARE(ArtworkProvider::bucketSize(QSize()), QSize(256, 256));
    const QImage png = provider.load(QStringLiteral("file:") + m_icons + QStringLiteral("/hicolor/128x128/apps/sized.png"),
                                     QSize(64, 0), &error);
    QCOMPARE(png.size(), QSize(64, 64));
    const QImage huge = provider.load(QStringLiteral("theme:vector"), QSize(99999, 99999), &error);
    QCOMPARE(huge.width(), ArtworkProvider::kMaxRequestDimension);
}

void TestArtwork::cacheHits()
{
    IconLocator locator(locatorConfig());
    const QString spec = QStringLiteral("theme:vector");   // SVG: always disk-cached
    {
        ArtworkProvider provider(m_cache, &locator);
        QVERIFY(!provider.load(spec, QSize(64, 64)).isNull());
        QCOMPARE(provider.stats().decodes, 1);
        QCOMPARE(provider.stats().memoryHits, 0);
        QVERIFY(!provider.load(spec, QSize(64, 64)).isNull());
        QCOMPARE(provider.stats().memoryHits, 1);             // second request: memory
        QVERIFY(!provider.load(spec, QSize(60, 60)).isNull());  // same 64 px bucket
        QCOMPARE(provider.stats().memoryHits, 2);
        QCOMPARE(provider.stats().decodes, 1);
        QVERIFY(!provider.load(spec, QSize(128, 128)).isNull());  // another bucket
        QCOMPARE(provider.stats().decodes, 2);
        QCOMPARE(provider.stats().diskWrites, 2);
        const QStringList thumbs = QDir(m_cache).entryList({ QStringLiteral("*.png") }, QDir::Files);
        QCOMPARE(thumbs.size(), 2);
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{40}\\.png$")).match(thumbs.first()).hasMatch());
        const auto perms = QFileInfo(m_cache).permissions();
        QVERIFY(!(perms & QFileDevice::ReadOther));            // cache dir is private (0700)
    }
    // A new process (empty memory): served from the disk cache.
    ArtworkProvider provider(m_cache, &locator);
    const QImage image = provider.load(spec, QSize(64, 64));
    QCOMPARE(image.size(), QSize(64, 64));
    QCOMPARE(provider.stats().diskHits, 1);
    QCOMPARE(provider.stats().decodes, 0);
}

void TestArtwork::changedSourceIsRedecoded()
{
    IconLocator locator(locatorConfig());
    const QString path = m_icons + QStringLiteral("/hicolor/128x128/apps/sized.png");
    const QString spec = QStringLiteral("file:") + path;
    ArtworkProvider provider(m_cache, &locator);
    QCOMPARE(provider.load(spec, QSize(16, 16)).pixelColor(8, 8), QColor(Qt::blue));
    // Replace the icon (new mtime and size): the cache key changes.
    QTest::qWait(20);
    QImage image(100, 100, QImage::Format_ARGB32);
    image.fill(Qt::yellow);
    QVERIFY(image.save(path, "png"));
    QFile(path).setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime);
    QCOMPARE(provider.load(spec, QSize(16, 16)).pixelColor(8, 8), QColor(Qt::yellow));
    QCOMPARE(provider.stats().decodes, 2);
}

void TestArtwork::rejectsUntrustedSpecs_data()
{
    QTest::addColumn<QString>("spec");
    QTest::newRow("theme traversal") << QStringLiteral("theme:../../../../etc/passwd");
    QTest::newRow("theme slash") << QStringLiteral("theme:apps/sized");
    QTest::newRow("relative file") << QStringLiteral("file:icons/foo.png");
    QTest::newRow("dotdot file") << QStringLiteral("file:/usr/share/../share/pixmaps/x.png");
    QTest::newRow("not an image type") << QStringLiteral("file:/etc/passwd");
    QTest::newRow("missing") << QStringLiteral("file:/nonexistent/x.png");
    QTest::newRow("unknown scheme") << QStringLiteral("http://example.com/x.png");
    QTest::newRow("empty") << QString();
}

void TestArtwork::rejectsUntrustedSpecs()
{
    QFETCH(QString, spec);
    IconLocator locator(locatorConfig());
    ArtworkProvider provider(m_cache, &locator);
    QString error;
    QVERIFY(provider.load(spec, QSize(64, 64), &error).isNull());
    QVERIFY(!error.isEmpty());
    QCOMPARE(provider.stats().failures, 1);
    QVERIFY(!QFileInfo::exists(m_cache) || QDir(m_cache).isEmpty());
}

void TestArtwork::asyncResponse()
{
    IconLocator locator(locatorConfig());
    ArtworkProvider provider(m_cache, &locator);
    const QString id = ArtworkProvider::urlFor(QStringLiteral("theme:themed")).mid(12);
    std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(id, QSize(48, 48)));
    QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
    QVERIFY(finished.wait(5000));
    QVERIFY(response->errorString().isEmpty());
    std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
    QVERIFY(factory);
    QCOMPARE(factory->textureSize(), QSize(64, 64));          // 48 -> 64 px bucket

    std::unique_ptr<QQuickImageResponse> bad(provider.requestImageResponse(QStringLiteral("!!"), QSize(48, 48)));
    QSignalSpy badFinished(bad.get(), &QQuickImageResponse::finished);
    QVERIFY(badFinished.wait(5000));
    QVERIFY(!bad->errorString().isEmpty());
    QVERIFY(!bad->textureFactory());
}

void TestArtwork::smallRastersSkipDisk()
{
    IconLocator locator(locatorConfig());
    ArtworkProvider provider(m_cache, &locator);
    // A 128 px PNG of a flat colour is a few hundred bytes: re-decoding it
    // is as cheap as reading a thumbnail, so it gets no disk entry.
    QVERIFY(!provider.load(QStringLiteral("theme:sized"), QSize(64, 64)).isNull());
    QCOMPARE(provider.stats().diskWrites, 0);
    QVERIFY(!QFileInfo::exists(m_cache) || QDir(m_cache).isEmpty());
    // A large raster is cached.
    QImage noise(1024, 1024, QImage::Format_ARGB32);
    for (int y = 0; y < noise.height(); ++y)
        for (int x = 0; x < noise.width(); ++x)
            noise.setPixel(x, y, qRgba((x * 7) ^ y, (y * 13) ^ x, (x * y) & 0xff, 255));
    const QString big = m_tmp->path() + QStringLiteral("/big.png");
    QVERIFY(noise.save(big, "png"));
    QVERIFY(QFileInfo(big).size() > ArtworkProvider::kSmallRasterBytes);
    QVERIFY(!provider.load(QStringLiteral("file:") + big, QSize(64, 64)).isNull());
    QCOMPARE(provider.stats().diskWrites, 1);
}

void TestArtwork::lruEviction()
{
    IconLocator locator(locatorConfig());
    ArtworkProvider provider(m_cache, &locator);
    provider.pool()->waitForDone();                        // startup trim (empty dir)
    // Five thumbnails of growing size buckets, oldest first.
    for (int i = 1; i <= 5; ++i) {
        QVERIFY(!provider.load(QStringLiteral("theme:vector"), QSize(64 * i, 64 * i)).isNull());
        QTest::qWait(15);                                  // distinct mtimes
    }
    QCOMPARE(provider.stats().diskWrites, 5);
    QFileInfoList files = QDir(m_cache).entryInfoList(QDir::Files, QDir::Time | QDir::Reversed);
    QCOMPARE(files.size(), 5);
    const QString oldest = files.first().fileName();
    const QString newest = files.last().fileName();
    qint64 total = 0;
    for (const QFileInfo &f : std::as_const(files))
        total += f.size();
    // A disk hit refreshes the oldest one: it becomes the most recent.
    {
        ArtworkProvider fresh(m_cache, &locator);
        fresh.pool()->waitForDone();
        QVERIFY(!fresh.load(QStringLiteral("theme:vector"), QSize(64, 64)).isNull());
        QCOMPARE(fresh.stats().diskHits, 1);
    }
    // Budget: everything but about one file.
    provider.setDiskBudget(total - files.at(1).size());
    const QString second = files.at(1).fileName();
    QVERIFY(provider.trimDiskCache() >= 1);
    QVERIFY(QFileInfo::exists(m_cache + QLatin1Char('/') + oldest));   // refreshed by the hit
    QVERIFY(!QFileInfo::exists(m_cache + QLatin1Char('/') + second));  // now the oldest: evicted
    QVERIFY(QFileInfo::exists(m_cache + QLatin1Char('/') + newest));
    QVERIFY(provider.stats().evictions >= 1);
    // Foreign files in the dir are never touched.
    QFile foreign(m_cache + QStringLiteral("/notes.txt"));
    QVERIFY(foreign.open(QIODevice::WriteOnly));
    foreign.write(QByteArray(4096, 'x'));
    foreign.close();
    provider.setDiskBudget(0);
    provider.trimDiskCache();
    QVERIFY(foreign.exists());
    QCOMPARE(QDir(m_cache).entryList({ QStringLiteral("*.png") }, QDir::Files).size(), 0);
}

void TestArtwork::wrongContentIsRejected()
{
    IconLocator locator(locatorConfig());
    ArtworkProvider provider(m_cache, &locator);
    QString error;
    // An SVG saved as .png: the decoder is pinned to PNG, no sniffing.
    const QString fake = m_tmp->path() + QStringLiteral("/fake.png");
    QFile::copy(m_icons + QStringLiteral("/hicolor/scalable/apps/vector.svg"), fake);
    QVERIFY(provider.load(QStringLiteral("file:") + fake, QSize(64, 64), &error).isNull());
    QVERIFY(!error.isEmpty());
    const QString text = m_tmp->path() + QStringLiteral("/text.png");
    QFile f(text);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not an image at all");
    f.close();
    QVERIFY(provider.load(QStringLiteral("file:") + text, QSize(64, 64)).isNull());
    // Compressed SVG is not accepted (its decompressed size can't be capped).
    const QString svgz = m_tmp->path() + QStringLiteral("/icon.svgz");
    QFile::copy(m_icons + QStringLiteral("/hicolor/scalable/apps/vector.svg"), svgz);
    QVERIFY(provider.load(QStringLiteral("file:") + svgz, QSize(64, 64)).isNull());
    // A FIFO named like an icon neither blocks nor decodes.
    const QString fifo = m_tmp->path() + QStringLiteral("/fifo.png");
    QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
    QVERIFY(provider.load(QStringLiteral("file:") + fifo, QSize(64, 64)).isNull());
    QCOMPARE(provider.stats().decodes, 0);
}

void TestArtwork::teardownWithJobsInFlight()
{
    IconLocator locator(locatorConfig());
    const QString id = ArtworkProvider::urlFor(QStringLiteral("theme:vector")).mid(12);
    // 1. Provider destroyed while jobs are queued/running; responses after.
    {
        auto provider = std::make_unique<ArtworkProvider>(m_cache, &locator);
        std::vector<std::unique_ptr<QQuickImageResponse>> responses;
        for (int i = 0; i < 40; ++i)
            responses.emplace_back(provider->requestImageResponse(id, QSize(64 * (1 + i % 16), 64)));
        provider.reset();              // clears the queue, waits for running jobs
        QCoreApplication::processEvents();
        responses.clear();
        QCoreApplication::processEvents();
    }
    // 2. Responses destroyed (cancelled) before their jobs finish.
    {
        ArtworkProvider provider(m_cache, &locator);
        for (int i = 0; i < 40; ++i) {
            std::unique_ptr<QQuickImageResponse> r(provider.requestImageResponse(id, QSize(64 * (1 + i % 16), 64)));
            r->cancel();
        }                               // each response deleted right away
        provider.pool()->waitForDone();
        QCoreApplication::processEvents();   // queued done() to dead responses: dropped
    }
    QVERIFY(true);                      // no crash, no use-after-free (run under ASan to be sure)
}

void TestArtwork::refreshOnlyWhenDirsChange()
{
    IconLocator locator(locatorConfig());
    QVERIFY(!locator.find(QStringLiteral("themed"), 48).isEmpty());
    QVERIFY(!locator.refreshIfChanged());                 // nothing changed
    QVERIFY(!locator.refreshIfChanged());
    QTest::qWait(20);
    writePng(m_icons + QStringLiteral("/Test/48x48/apps/installed.png"), 48, Qt::red);
    QCOMPARE(locator.find(QStringLiteral("installed"), 48), QString());   // index still the old one
    QVERIFY(locator.refreshIfChanged());                  // dir mtime changed
    QVERIFY(!locator.find(QStringLiteral("installed"), 48).isEmpty());
}

QTEST_MAIN(TestArtwork)
#include "tst_artwork.moc"
