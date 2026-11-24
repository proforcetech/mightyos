#include "apikeystore.h"
#include "artworkscraper.h"
#include "steamgriddb.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <functional>
#include <utility>

#include <sys/stat.h>

namespace {

// Scripted responses by URL prefix; records every request.
class FakeFetcher : public HttpFetcher
{
public:
    struct Request { QUrl url; QString bearer; };
    QList<Request> requests;
    QList<QPair<QString, HttpResponse>> script;   // prefix -> response (first match, consumed)
    bool hold = false;                            // keep callbacks for release()
    QList<std::function<void()>> held;
    void release()
    {
        const auto calls = std::exchange(held, {});
        for (const auto &call : calls)
            call();
    }
    QHash<QString, HttpResponse> fallback;        // prefix -> response (sticky)

    void get(const QUrl &url, const QString &bearer, qint64 maxBytes, Callback done) override
    {
        Q_UNUSED(maxBytes);
        requests.append({ url, bearer });
        HttpResponse response;
        response.error = HttpResponse::Error::Network;
        const QString text = url.toString();
        bool found = false;
        for (int i = 0; i < script.size(); ++i) {
            if (text.startsWith(script.at(i).first)) {
                response = script.takeAt(i).second;
                found = true;
                break;
            }
        }
        for (auto it = fallback.cbegin(); !found && it != fallback.cend(); ++it) {
            if (text.startsWith(it.key())) {
                response = it.value();
                found = true;
            }
        }
        if (hold) {
            held.append([done, response] { done(response); });
            return;
        }
        QMetaObject::invokeMethod(this, [done, response] { done(response); }, Qt::QueuedConnection);
    }
};

HttpResponse ok(const QByteArray &body, const QByteArray &type = "application/json")
{
    HttpResponse r;
    r.status = 200;
    r.body = body;
    r.contentType = type;
    return r;
}

HttpResponse status(int code, int retryAfter = -1)
{
    HttpResponse r;
    r.status = code;
    r.retryAfterSeconds = retryAfter;
    return r;
}

QByteArray png(int w = 60, int h = 90)
{
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "png");
    return bytes;
}

const QString kKey = QStringLiteral("0123456789abcdef0123456789abcdef");
const QString kApi = QStringLiteral("https://www.steamgriddb.com/api/v2/");
const QString kCdn = QStringLiteral("https://cdn2.steamgriddb.com/grid/");

} // namespace

// SteamGridDB (PLAN D11): the key store, URL/host rules, response parsing
// and image validation, and the scraper's queue, rate limiting, backoff
// and disk cache, all against a scripted fetcher (no network in tests).
class TestScraper : public QObject
{
    Q_OBJECT

private slots:
    void keyValidationAndMask();
    void keyFileIsPrivate();
    void keyFileRefusesUnsafe();
    void hostAllowlist();
    void urls();
    void parsing();
    void imageValidation();
    void noKeyNoRequests();
    void fullFlowWritesArtwork();
    void steamUsesAppId();
    void rejectedKeyStops();
    void backoffThenRetry();
    void notFoundIsRemembered();
    void diskBudget();
    void clearDuringRequestIgnoresLateReply();

private:
    ArtworkScraper::Config config(const QTemporaryDir &tmp) const
    {
        ArtworkScraper::Config c;
        c.dir = tmp.path() + QStringLiteral("/artwork");
        c.configDir = tmp.path() + QStringLiteral("/config");
        c.useSecretService = false;
        c.apiIntervalMs = 1;
        c.imageIntervalMs = 1;
        c.backoffBaseMs = 50;
        return c;
    }
    std::unique_ptr<ArtworkScraper> withKey(const QTemporaryDir &tmp, FakeFetcher *fetcher)
    {
        ApiKeyStore store(tmp.path() + QStringLiteral("/config"), false);
        if (!store.write(kKey))
            return nullptr;
        auto scraper = std::make_unique<ArtworkScraper>(config(tmp), fetcher);
        scraper->waitForKeyIo();
        return scraper;
    }
    static void script(FakeFetcher *f, const QString &prefix, const HttpResponse &r) { f->script.append({ prefix, r }); }
};

void TestScraper::keyValidationAndMask()
{
    QVERIFY(ApiKeyStore::isValidKey(kKey));
    QVERIFY(!ApiKeyStore::isValidKey(QStringLiteral("short")));
    QVERIFY(!ApiKeyStore::isValidKey(kKey + QStringLiteral("\nX-Evil: 1")));
    QVERIFY(!ApiKeyStore::isValidKey(QStringLiteral("0123456789abcdef 0123")));
    QCOMPARE(ApiKeyStore::normalized(QStringLiteral("  ") + kKey + QStringLiteral("\n")), kKey);
    const QString masked = ApiKeyStore::masked(kKey);
    QVERIFY(masked.endsWith(QLatin1String("cdef")));
    QVERIFY(!masked.contains(QLatin1String("0123")));
}

void TestScraper::keyFileIsPrivate()
{
    QTemporaryDir tmp;
    ApiKeyStore store(tmp.path() + QStringLiteral("/config"), false);
    QVERIFY(!store.read());
    QVERIFY(!store.write(QStringLiteral("bad key")));
    QVERIFY(store.write(QStringLiteral(" ") + kKey + QStringLiteral(" ")));
    QCOMPARE(store.lastBackend(), ApiKeyStore::Backend::File);
    struct stat st {};
    QCOMPARE(::stat(QFile::encodeName(store.filePath()).constData(), &st), 0);
    QCOMPARE(st.st_mode & 0777, mode_t(0600));
    QCOMPARE(::stat(QFile::encodeName(tmp.path() + QStringLiteral("/config/secrets")).constData(), &st), 0);
    QCOMPARE(st.st_mode & 0777, mode_t(0700));
    QCOMPARE(store.read().value_or(QString()), kKey);
    QVERIFY(store.write(QStringLiteral("ffffffffffffffffffffffffffffffff")));   // replace
    QCOMPARE(store.read().value_or(QString()), QStringLiteral("ffffffffffffffffffffffffffffffff"));
    QCOMPARE(QDir(tmp.path() + QStringLiteral("/config/secrets")).entryList(QDir::Files | QDir::Hidden).size(), 1);
    QVERIFY(store.clear());
    QVERIFY(!store.read());
}

void TestScraper::keyFileRefusesUnsafe()
{
    QTemporaryDir tmp;
    ApiKeyStore store(tmp.path() + QStringLiteral("/config"), false);
    QVERIFY(store.write(kKey));
    // Readable by others: refused (and not echoed in the error).
    QFile::setPermissions(store.filePath(), QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther);
    QString error;
    QVERIFY(!store.read(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!error.contains(kKey));
    // A symlink in its place: refused.
    QFile::remove(store.filePath());
    const QString elsewhere = tmp.path() + QStringLiteral("/elsewhere");
    {
        QFile f(elsewhere);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(kKey.toLatin1());
    }
    QFile::setPermissions(elsewhere, QFile::ReadOwner | QFile::WriteOwner);
    QVERIFY(QFile::link(elsewhere, store.filePath()));
    QVERIFY(!store.read());
    // The secrets dir as a symlink: writing refuses.
    QTemporaryDir tmp2;
    QDir().mkpath(tmp2.path() + QStringLiteral("/config"));
    QDir().mkpath(tmp2.path() + QStringLiteral("/target"));
    QVERIFY(QFile::link(tmp2.path() + QStringLiteral("/target"), tmp2.path() + QStringLiteral("/config/secrets")));
    ApiKeyStore linked(tmp2.path() + QStringLiteral("/config"), false);
    QVERIFY(!linked.write(kKey));
    QVERIFY(QDir(tmp2.path() + QStringLiteral("/target")).isEmpty());
}

void TestScraper::hostAllowlist()
{
    QVERIFY(SteamGridDb::isAllowedUrl(QUrl(kApi + QStringLiteral("grids/game/1"))));
    QVERIFY(SteamGridDb::isAllowedUrl(QUrl(kCdn + QStringLiteral("x.png"))));
    QVERIFY(SteamGridDb::isApiUrl(QUrl(kApi + QStringLiteral("grids/game/1"))));
    QVERIFY(!SteamGridDb::isApiUrl(QUrl(kCdn + QStringLiteral("x.png"))));
    for (const char *bad : { "http://www.steamgriddb.com/api/v2/x", "https://steamgriddb.com.evil.net/x",
                             "https://evilsteamgriddb.com/x", "https://user:pw@www.steamgriddb.com/x",
                             "https://www.steamgriddb.com:8443/x", "file:///etc/passwd", "https://127.0.0.1/x" })
        QVERIFY2(!SteamGridDb::isAllowedUrl(QUrl(QString::fromLatin1(bad))), bad);
}

void TestScraper::urls()
{
    const QUrl search = SteamGridDb::searchUrl(QStringLiteral("Zelda/../../users?x=1#y"));
    QVERIFY(SteamGridDb::isApiUrl(search));
    QVERIFY(search.toString(QUrl::FullyEncoded).startsWith(kApi + QStringLiteral("search/autocomplete/Zelda%2F..%2F")));
    QVERIFY(search.query().isEmpty());
    QVERIFY(search.fragment().isEmpty());
    QVERIFY(!SteamGridDb::searchUrl(QStringLiteral("   ")).isValid());
    const QUrl grids = SteamGridDb::gridsForSteamUrl(QStringLiteral("620"));
    QVERIFY(grids.toString().startsWith(kApi + QStringLiteral("grids/steam/620?")));
    QVERIFY(grids.query().contains(QLatin1String("nsfw=false")));
    QVERIFY(!SteamGridDb::gridsForSteamUrl(QStringLiteral("62/0")).isValid());
    QVERIFY(!SteamGridDb::gridsForGameUrl(0).isValid());
}

void TestScraper::parsing()
{
    const QByteArray j1 = R"({"success":true,"data":[{"id":4242,"name":"Z"}]})";
    QCOMPARE(SteamGridDb::parseSearch(j1).value_or(0), 4242);
    const QByteArray j2 = R"({"success":true,"data":[]})";
    QVERIFY(!SteamGridDb::parseSearch(j2));
    const QByteArray j3 = R"({"success":false,"data":[{"id":1}]})";
    QVERIFY(!SteamGridDb::parseSearch(j3));
    const QByteArray j4 = R"({"success":true,"data":[{"id":"1"}]})";
    QVERIFY(!SteamGridDb::parseSearch(j4));
    const QByteArray j5 = R"({"success":true,"data":[{"id":-5}]})";
    QVERIFY(!SteamGridDb::parseSearch(j5));
    QVERIFY(!SteamGridDb::parseSearch("not json"));
    const QByteArray grids = R"({"success":true,"data":[
        {"url":"https://evil.example/x.png","mime":"image/png"},
        {"url":"https://cdn2.steamgriddb.com/grid/a.webp","mime":"image/webp"},
        {"url":"https://cdn2.steamgriddb.com/grid/n.png","mime":"image/png","nsfw":true},
        {"url":"http://cdn2.steamgriddb.com/grid/h.png","mime":"image/png"},
        {"url":"https://cdn2.steamgriddb.com/grid/ok.jpg","mime":"image/jpeg"}]})";
    QCOMPARE(SteamGridDb::parseGrids(grids), QUrl(QStringLiteral("https://cdn2.steamgriddb.com/grid/ok.jpg")));
    const QByteArray j6 = R"({"success":true,"data":[{"url":"https://www.steamgriddb.com/api/v2/x","mime":"image/png"}]})";
    QVERIFY(!SteamGridDb::parseGrids(j6).isValid());
    QVERIFY(!SteamGridDb::parseGrids(QByteArray(SteamGridDb::kMaxJsonBytes + 1, ' ')).isValid());
}

void TestScraper::imageValidation()
{
    QCOMPARE(SteamGridDb::validateImage(png(), "image/png"), QStringLiteral("png"));
    QCOMPARE(SteamGridDb::validateImage(png(), "image/png; charset=binary"), QStringLiteral("png"));
    QVERIFY(SteamGridDb::validateImage(png(), "image/jpeg").isEmpty());         // type disagrees
    QVERIFY(SteamGridDb::validateImage(png().left(60), "image/png").isEmpty());  // truncated
    QVERIFY(SteamGridDb::validateImage("<svg/>", "image/svg+xml").isEmpty());
    QVERIFY(SteamGridDb::validateImage(png(5000, 10), "image/png").isEmpty());   // dimensions
    QVERIFY(SteamGridDb::validateImage(QByteArray(), "image/png").isEmpty());
}

void TestScraper::noKeyNoRequests()
{
    QTemporaryDir tmp;
    auto *fetcher = new FakeFetcher;
    ArtworkScraper scraper(config(tmp), fetcher);
    scraper.waitForKeyIo();
    QVERIFY(!scraper.hasKey());
    scraper.request(QStringLiteral("rom:snes:1"), QStringLiteral("Zelda"), QString());
    QTest::qWait(50);
    QVERIFY(fetcher->requests.isEmpty());
    QCOMPARE(scraper.queued(), 0);
}

void TestScraper::fullFlowWritesArtwork()
{
    QTemporaryDir tmp;
    auto *fetcher = new FakeFetcher;
    script(fetcher, kApi + QStringLiteral("search/"), ok(R"({"success":true,"data":[{"id":77}]})"));
    script(fetcher, kApi + QStringLiteral("grids/game/77"),
           ok(R"({"success":true,"data":[{"url":"https://cdn2.steamgriddb.com/grid/z.png","mime":"image/png"}]})"));
    script(fetcher, kCdn, ok(png(), "image/png"));
    auto scraper = withKey(tmp, fetcher);
    QVERIFY(scraper);
    QVERIFY(scraper->hasKey());
    QVERIFY(scraper->maskedKey().endsWith(QLatin1String("cdef")));
    QSignalSpy ready(scraper.get(), &ArtworkScraper::artworkReady);
    scraper->request(QStringLiteral("rom:snes:1"), QStringLiteral("Zelda"), QString());
    QTRY_COMPARE(ready.count(), 1);
    const QString path = ready.first().at(1).toString();
    QVERIFY(path.startsWith(tmp.path() + QStringLiteral("/artwork/")));
    QVERIFY(path.endsWith(QLatin1String(".png")));
    QVERIFY(QFileInfo(path).isFile());
    QCOMPARE(scraper->cachedPath(QStringLiteral("rom:snes:1")), path);
    QCOMPARE(fetcher->requests.size(), 3);
    QCOMPARE(fetcher->requests.at(0).bearer, kKey);   // API: the key
    QCOMPARE(fetcher->requests.at(1).bearer, kKey);
    QVERIFY(fetcher->requests.at(2).bearer.isEmpty());   // CDN: never the key
    // Cached: no new request.
    scraper->request(QStringLiteral("rom:snes:1"), QStringLiteral("Zelda"), QString());
    QTest::qWait(50);
    QCOMPARE(fetcher->requests.size(), 3);
}

void TestScraper::steamUsesAppId()
{
    QTemporaryDir tmp;
    auto *fetcher = new FakeFetcher;
    script(fetcher, kApi + QStringLiteral("grids/steam/620"),
           ok(R"({"success":true,"data":[{"url":"https://cdn2.steamgriddb.com/grid/p.png","mime":"image/png"}]})"));
    script(fetcher, kCdn, ok("not an image", "image/png"));   // invalid: dropped
    auto scraper = withKey(tmp, fetcher);
    QSignalSpy ready(scraper.get(), &ArtworkScraper::artworkReady);
    scraper->request(QStringLiteral("steam:620"), QStringLiteral("Portal 2"), QStringLiteral("620"));
    QTRY_COMPARE(fetcher->requests.size(), 2);
    QTRY_VERIFY(scraper->queued() == 0);
    QTest::qWait(50);
    QCOMPARE(ready.count(), 0);
    QVERIFY(scraper->cachedPath(QStringLiteral("steam:620")).isEmpty());
    QVERIFY(QDir(tmp.path() + QStringLiteral("/artwork")).entryList({ QStringLiteral("*.png") }).isEmpty());
}

void TestScraper::rejectedKeyStops()
{
    QTemporaryDir tmp;
    auto *fetcher = new FakeFetcher;
    fetcher->fallback.insert(kApi, status(401));
    auto scraper = withKey(tmp, fetcher);
    scraper->request(QStringLiteral("rom:a:1"), QStringLiteral("A"), QString());
    scraper->request(QStringLiteral("rom:a:2"), QStringLiteral("B"), QString());
    QTRY_COMPARE(fetcher->requests.size(), 1);
    QTest::qWait(100);
    QCOMPARE(fetcher->requests.size(), 1);   // stopped
    QVERIFY(scraper->status().contains(QLatin1String("rejected")));
    // A new key resumes.
    fetcher->fallback.clear();
    script(fetcher, kApi + QStringLiteral("search/"), ok(R"({"success":true,"data":[]})"));
    QVERIFY(scraper->setApiKey(QStringLiteral("ffffffffffffffffffffffffffffffff")));
    scraper->waitForKeyIo();
    QTRY_VERIFY(fetcher->requests.size() >= 2);
}

void TestScraper::backoffThenRetry()
{
    QTemporaryDir tmp;
    auto *fetcher = new FakeFetcher;
    script(fetcher, kApi + QStringLiteral("search/"), status(429, 0));
    script(fetcher, kApi + QStringLiteral("search/"), status(503));
    HttpResponse offline;
    offline.error = HttpResponse::Error::Network;
    script(fetcher, kApi + QStringLiteral("search/"), offline);
    script(fetcher, kApi + QStringLiteral("search/"), ok(R"({"success":true,"data":[{"id":5}]})"));
    script(fetcher, kApi + QStringLiteral("grids/game/5"),
           ok(R"({"success":true,"data":[{"url":"https://cdn2.steamgriddb.com/grid/b.png","mime":"image/png"}]})"));
    script(fetcher, kCdn, ok(png(), "image/png"));
    auto scraper = withKey(tmp, fetcher);
    QSignalSpy ready(scraper.get(), &ArtworkScraper::artworkReady);
    QElapsedTimer timer;
    timer.start();
    scraper->request(QStringLiteral("rom:x:1"), QStringLiteral("X"), QString());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
    // 50 ms (Retry-After floor) + 100 ms + 200 ms of backoff at least.
    QVERIFY2(timer.elapsed() >= 300, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(fetcher->requests.size(), 6);
}

void TestScraper::notFoundIsRemembered()
{
    QTemporaryDir tmp;
    auto *fetcher = new FakeFetcher;
    script(fetcher, kApi + QStringLiteral("search/"), ok(R"({"success":true,"data":[]})"));
    {
        auto scraper = withKey(tmp, fetcher);
        scraper->request(QStringLiteral("rom:x:1"), QStringLiteral("Obscure"), QString());
        QTRY_COMPARE(fetcher->requests.size(), 1);
        QTRY_COMPARE(scraper->queued(), 0);
        QTest::qWait(30);
    }   // destructor writes the index
    auto *second = new FakeFetcher;
    auto scraper = withKey(tmp, second);
    scraper->request(QStringLiteral("rom:x:1"), QStringLiteral("Obscure"), QString());
    QTest::qWait(50);
    QVERIFY(second->requests.isEmpty());
}

void TestScraper::diskBudget()
{
    QTemporaryDir tmp;
    auto *fetcher = new FakeFetcher;
    ArtworkScraper::Config c = config(tmp);
    const QByteArray image = png(300, 450);
    c.diskBudget = image.size() * 2 + 10;
    {
        ApiKeyStore store(c.configDir, false);
        QVERIFY(store.write(kKey));
    }
    ArtworkScraper scraper(c, fetcher);
    scraper.waitForKeyIo();
    QSignalSpy ready(&scraper, &ArtworkScraper::artworkReady);
    for (int i = 0; i < 3; ++i) {
        script(fetcher, kApi + QStringLiteral("grids/steam/"),
               ok(R"({"success":true,"data":[{"url":"https://cdn2.steamgriddb.com/grid/i.png","mime":"image/png"}]})"));
        script(fetcher, kCdn, ok(image, "image/png"));
        scraper.request(QStringLiteral("steam:%1").arg(100 + i), QString(), QString::number(100 + i));
        QTRY_COMPARE(ready.count(), i + 1);
        QTest::qWait(5);   // distinct "used" times
    }
    // An orphan file of ours is removed; foreign files are left alone.
    QFile orphan(tmp.path() + QStringLiteral("/artwork/") + QString(40, QLatin1Char('a')) + QStringLiteral(".png"));
    QVERIFY(orphan.open(QIODevice::WriteOnly));
    orphan.close();
    QFile foreign(tmp.path() + QStringLiteral("/artwork/README"));
    QVERIFY(foreign.open(QIODevice::WriteOnly));
    foreign.close();
    QVERIFY(scraper.trimDiskCache() >= 2);   // the orphan + the oldest image
    QVERIFY(scraper.cachedPath(QStringLiteral("steam:100")).isEmpty());
    QVERIFY(!scraper.cachedPath(QStringLiteral("steam:102")).isEmpty());
    QVERIFY(!QFileInfo::exists(orphan.fileName()));
    QVERIFY(QFileInfo::exists(foreign.fileName()));
}

// M4/L2: a reply that arrives after the key was cleared is ignored (no
// file, no signal), and the key file is gone (cleared on the key worker,
// after any pending write).
void TestScraper::clearDuringRequestIgnoresLateReply()
{
    QTemporaryDir tmp;
    auto *fetcher = new FakeFetcher;
    script(fetcher, kApi + QStringLiteral("grids/steam/620"),
           ok(R"({"success":true,"data":[{"url":"https://cdn2.steamgriddb.com/grid/p.png","mime":"image/png"}]})"));
    fetcher->hold = true;
    auto scraper = withKey(tmp, fetcher);
    QSignalSpy ready(scraper.get(), &ArtworkScraper::artworkReady);
    scraper->request(QStringLiteral("steam:620"), QString(), QStringLiteral("620"));
    QTRY_COMPARE(fetcher->held.size(), 1);
    // Stale temporaries of an interrupted write are cleaned up too.
    QFile stale(tmp.path() + QStringLiteral("/config/secrets/.steamgriddb.key.tmp-dead"));
    QVERIFY(stale.open(QIODevice::WriteOnly));
    stale.close();
    scraper->clearApiKey();
    scraper->waitForKeyIo();
    QVERIFY(!scraper->hasKey());
    QVERIFY(!QFileInfo::exists(tmp.path() + QStringLiteral("/config/secrets/steamgriddb.key")));
    QVERIFY(!QFileInfo::exists(stale.fileName()));
    fetcher->hold = false;
    fetcher->release();   // the late grids reply
    QTest::qWait(50);
    QCOMPARE(fetcher->requests.size(), 1);   // no image request followed
    QCOMPARE(ready.count(), 0);
    // A new key works normally afterwards.
    script(fetcher, kApi + QStringLiteral("grids/steam/621"),
           ok(R"({"success":true,"data":[{"url":"https://cdn2.steamgriddb.com/grid/q.png","mime":"image/png"}]})"));
    script(fetcher, kCdn, ok(png(), "image/png"));
    QVERIFY(scraper->setApiKey(kKey));
    scraper->waitForKeyIo();
    QVERIFY(scraper->hasKey());
    scraper->request(QStringLiteral("steam:621"), QString(), QStringLiteral("621"));
    QTRY_COMPARE(ready.count(), 1);
}

QTEST_MAIN(TestScraper)
#include "tst_scraper.moc"
