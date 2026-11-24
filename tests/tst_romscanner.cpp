#include "librarycache.h"
#include "romscanner.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

// RomScanner: titles, the bounded walk (symlinks, special files, depth),
// multi-disc handling, incremental reuse; LibraryCache (SQLite).
class TestRomScanner : public QObject
{
    Q_OBJECT

private slots:
    void cleanTitles_data();
    void cleanTitles();
    void ids();
    void walkIsContained();
    void depthLimit();
    void multiDisc();
    void incremental();
    void verifiedPath();
    void cacheRoundTrip();
    void cacheRejectsBadRowsAndRecreates();
    void twentyThousandRoms();

private:
    static void touch(const QString &path, const QByteArray &data = "x")
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    }
    static QStringList titles(const QList<RomRecord> &records)
    {
        QStringList out;
        for (const RomRecord &r : records)
            out.append(r.title);
        out.sort();
        return out;
    }
};

void TestRomScanner::cleanTitles_data()
{
    QTest::addColumn<QString>("stem");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QString>("region");
    QTest::addColumn<QString>("tags");
    QTest::addColumn<int>("disc");
    QTest::newRow("no-intro") << "Super Mario World (USA)" << "Super Mario World" << "USA" << "" << 0;
    QTest::newRow("multi region + rev") << "Sonic the Hedgehog (USA, Europe) (Rev 1)" << "Sonic the Hedgehog"
                                        << "USA, Europe" << "Rev 1" << 0;
    QTest::newRow("article") << "Legend of Zelda, The - A Link to the Past (USA)"
                             << "The Legend of Zelda - A Link to the Past" << "USA" << "" << 0;
    QTest::newRow("languages and dump flags") << "Chrono Trigger (Japan) (En,Fr) [!] [b1]" << "Chrono Trigger"
                                              << "Japan" << "En,Fr, !, b1" << 0;
    QTest::newRow("disc") << "Final Fantasy VII (USA) (Disc 2)" << "Final Fantasy VII" << "USA" << "" << 2;
    QTest::newRow("underscores") << "metal_gear_solid" << "metal gear solid" << "" << "" << 0;
    QTest::newRow("only tags") << "(USA)" << "(USA)" << "USA" << "" << 0;
    QTest::newRow("bidi stripped") << QString::fromUtf8("Evil\xE2\x80\xAEgnirts (USA)") << "Evilgnirts" << "USA" << "" << 0;
    QTest::newRow("beta") << "Star Fox 2 (Beta) (1995-06-07)" << "Star Fox 2" << "" << "Beta, 1995-06-07" << 0;
}

void TestRomScanner::cleanTitles()
{
    QFETCH(QString, stem);
    QFETCH(QString, title);
    QFETCH(QString, region);
    QFETCH(QString, tags);
    QFETCH(int, disc);
    const CleanTitle t = RomScanner::cleanTitle(stem);
    QCOMPARE(t.title, title);
    QCOMPARE(t.region, region);
    QCOMPARE(t.tags, tags);
    QCOMPARE(t.disc, disc);
}

void TestRomScanner::ids()
{
    const QString id = RomScanner::romId(QStringLiteral("snes"), QStringLiteral("a/Game.sfc"));
    QVERIFY(id.startsWith(QLatin1String("rom:snes:")));
    QCOMPARE(id.size(), 9 + 40);
    QCOMPARE(id, RomScanner::romId(QStringLiteral("snes"), QStringLiteral("a/Game.sfc")));
    QVERIFY(id != RomScanner::romId(QStringLiteral("snes"), QStringLiteral("b/Game.sfc")));
}

void TestRomScanner::walkIsContained()
{
    QTemporaryDir tmp;
    const QString root = tmp.path() + QStringLiteral("/ROMs/snes");
    touch(root + QStringLiteral("/Alpha (USA).sfc"));
    touch(root + QStringLiteral("/sub/Beta (Europe).SMC"));      // extension case-insensitive
    touch(root + QStringLiteral("/readme.txt"));
    touch(root + QStringLiteral("/.hidden.sfc"));
    touch(root + QStringLiteral("/.git/Hidden dir.sfc"));
    touch(tmp.path() + QStringLiteral("/outside/Secret.sfc"));
    touch(tmp.path() + QStringLiteral("/outside/dir/Deep.sfc"));
    // Symlinks: inside the root (followed), outside (rejected), dangling.
    QVERIFY(QFile::link(root + QStringLiteral("/Alpha (USA).sfc"), root + QStringLiteral("/sub/Alias.sfc")));
    QVERIFY(QFile::link(tmp.path() + QStringLiteral("/outside/Secret.sfc"), root + QStringLiteral("/Escape.sfc")));
    QVERIFY(QFile::link(tmp.path() + QStringLiteral("/outside/dir"), root + QStringLiteral("/escapedir")));
    QVERIFY(QFile::link(root + QStringLiteral("/nowhere.sfc"), root + QStringLiteral("/Dangling.sfc")));
    QVERIFY(QFile::link(root, root + QStringLiteral("/sub/loop")));   // a cycle back to the root
    // A FIFO named like a ROM: never opened, skipped.
    QVERIFY(::mkfifo(QFile::encodeName(root + QStringLiteral("/Pipe.sfc")).constData(), 0600) == 0);

    RomScanStats stats;
    const QList<RomRecord> records = RomScanner::scan(
        { QStringLiteral("snes"), { QStringLiteral("sfc"), QStringLiteral("smc") }, { root } }, {}, &stats);
    QCOMPARE(titles(records), (QStringList { QStringLiteral("Alias"), QStringLiteral("Alpha"), QStringLiteral("Beta") }));
    QCOMPARE(stats.symlinksRejected, 3);
    QCOMPARE(stats.specialSkipped, 1);
    for (const RomRecord &r : records) {
        QCOMPARE(r.root, root);
        QVERIFY(!r.relPath.contains(QLatin1String("..")));
        QCOMPARE(r.id, RomScanner::romId(QStringLiteral("snes"), r.relPath));
    }
    // A missing root is not an error.
    QVERIFY(RomScanner::scan({ QStringLiteral("nes"), { QStringLiteral("nes") }, { tmp.path() + QStringLiteral("/none") } },
                             {}).isEmpty());
}

void TestRomScanner::depthLimit()
{
    QTemporaryDir tmp;
    QString dir = tmp.path();
    for (int i = 0; i <= RomScanner::kMaxDepth + 1; ++i) {
        touch(dir + QStringLiteral("/Level %1.nes").arg(i));
        dir += QStringLiteral("/d");
    }
    RomScanStats stats;
    const auto records = RomScanner::scan({ QStringLiteral("nes"), { QStringLiteral("nes") }, { tmp.path() } }, {}, &stats);
    QCOMPARE(records.size(), RomScanner::kMaxDepth + 1);
    QVERIFY(stats.truncated);
}

void TestRomScanner::multiDisc()
{
    QTemporaryDir tmp;
    const QString root = tmp.path();
    // .m3u playlist hides its discs.
    touch(root + QStringLiteral("/FF7 (Disc 1).chd"));
    touch(root + QStringLiteral("/FF7 (Disc 2).chd"));
    touch(root + QStringLiteral("/FF7 (Disc 3).chd"));
    touch(root + QStringLiteral("/Final Fantasy VII (USA).m3u"),
          "# playlist\nFF7 (Disc 1).chd\r\nFF7 (Disc 2).chd\nFF7 (Disc 3).chd\n../../etc/passwd\n/abs.chd\n");
    // .cue hides its tracks; same stem keeps the .cue.
    touch(root + QStringLiteral("/Crash (USA).cue"), "FILE \"Crash (USA) (Track 1).bin\" BINARY\nFILE \"Crash (USA) (Track 2).bin\" BINARY\n");
    touch(root + QStringLiteral("/Crash (USA) (Track 1).bin"));
    touch(root + QStringLiteral("/Crash (USA) (Track 2).bin"));
    touch(root + QStringLiteral("/Spyro (USA).cue"), "FILE Spyro.bin BINARY\n");
    touch(root + QStringLiteral("/Spyro (USA).chd"));
    // "(Disc N)" without a playlist: one game, disc 1, discs = 2.
    touch(root + QStringLiteral("/MGS (USA) (Disc 2).chd"));
    touch(root + QStringLiteral("/MGS (USA) (Disc 1).chd"));

    const auto records = RomScanner::scan(
        { QStringLiteral("ps1"), { QStringLiteral("cue"), QStringLiteral("chd"), QStringLiteral("m3u"), QStringLiteral("bin") },
          { root } }, {});
    QHash<QString, RomRecord> byTitle;
    for (const RomRecord &r : records)
        byTitle.insert(r.title, r);
    QCOMPARE(records.size(), 4);
    QCOMPARE(byTitle.value(QStringLiteral("Final Fantasy VII")).discs, 3);
    QVERIFY(byTitle.value(QStringLiteral("Final Fantasy VII")).relPath.endsWith(QLatin1String(".m3u")));
    QCOMPARE(byTitle.value(QStringLiteral("Crash")).relPath, QStringLiteral("Crash (USA).cue"));
    QCOMPARE(byTitle.value(QStringLiteral("Spyro")).relPath, QStringLiteral("Spyro (USA).cue"));
    QCOMPARE(byTitle.value(QStringLiteral("MGS")).discs, 2);
    QCOMPARE(byTitle.value(QStringLiteral("MGS")).relPath, QStringLiteral("MGS (USA) (Disc 1).chd"));
}

void TestRomScanner::incremental()
{
    QTemporaryDir tmp;
    touch(tmp.path() + QStringLiteral("/A.nes"));
    touch(tmp.path() + QStringLiteral("/B.nes"));
    const RomScanSpec spec { QStringLiteral("nes"), { QStringLiteral("nes") }, { tmp.path() } };
    const auto first = RomScanner::scan(spec, {});
    QHash<QString, RomRecord> previous;
    for (RomRecord r : first) {
        r.title += QStringLiteral(" (cached)");   // proves reuse
        previous.insert(r.id, r);
    }
    QTest::qWait(20);
    touch(tmp.path() + QStringLiteral("/B.nes"), "changed");
    RomScanStats stats;
    const auto second = RomScanner::scan(spec, previous, &stats);
    QCOMPARE(stats.reused, 1);
    QCOMPARE(titles(second), (QStringList { QStringLiteral("A (cached)"), QStringLiteral("B") }));
}

void TestRomScanner::verifiedPath()
{
    QTemporaryDir tmp;
    touch(tmp.path() + QStringLiteral("/root/Game.nes"));
    touch(tmp.path() + QStringLiteral("/outside.nes"));
    const QString root = tmp.path() + QStringLiteral("/root");
    QVERIFY(QFile::link(tmp.path() + QStringLiteral("/outside.nes"), root + QStringLiteral("/Link.nes")));
    QCOMPARE(RomScanner::verifiedPath(root, QStringLiteral("Game.nes")), root + QStringLiteral("/Game.nes"));
    QVERIFY(RomScanner::verifiedPath(root, QStringLiteral("../outside.nes")).isEmpty());
    QVERIFY(RomScanner::verifiedPath(root, QStringLiteral("Link.nes")).isEmpty());
    QVERIFY(RomScanner::verifiedPath(root, QStringLiteral("/etc/passwd")).isEmpty());
    QVERIFY(RomScanner::verifiedPath(root, QStringLiteral("Missing.nes")).isEmpty());
    QVERIFY(RomScanner::verifiedPath(QStringLiteral("relative"), QStringLiteral("Game.nes")).isEmpty());
}

void TestRomScanner::cacheRoundTrip()
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        QSKIP("Qt SQLite driver missing (libqt6sql6-sqlite)");
    QTemporaryDir tmp;
    const QString path = tmp.path() + QStringLiteral("/cache/library.sqlite");
    RomRecord a { RomScanner::romId(QStringLiteral("snes"), QStringLiteral("A.sfc")), QStringLiteral("snes"),
                  QStringLiteral("/r"), QStringLiteral("A.sfc"), QStringLiteral("A"), QStringLiteral("USA"),
                  QString(), 1, 10, 100 };
    RomRecord b = a;
    b.id = RomScanner::romId(QStringLiteral("snes"), QStringLiteral("B.sfc"));
    b.relPath = QStringLiteral("B.sfc");
    b.title = QStringLiteral("B");
    RomRecord n = a;
    n.system = QStringLiteral("nes");
    n.id = RomScanner::romId(QStringLiteral("nes"), QStringLiteral("A.sfc"));
    {
        LibraryCache cache(path);
        QVERIFY(cache.open());
        int written = 0, deleted = 0;
        QVERIFY(cache.sync({ a, b, n }, { QStringLiteral("snes"), QStringLiteral("nes") }, {}, &written, &deleted));
        QCOMPARE(written, 3);
    }
    QFileInfo info(path);
    QCOMPARE(info.permissions() & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther),
             QFile::Permissions());
    LibraryCache cache(path);
    QVERIFY(cache.open());
    const auto loaded = cache.load();
    QCOMPARE(loaded.size(), 3);
    QCOMPARE(loaded.value(a.id), a);
    // Diff: b removed, a unchanged (no write), a new one; nes untouched.
    RomRecord c = a;
    c.id = RomScanner::romId(QStringLiteral("snes"), QStringLiteral("C.sfc"));
    c.relPath = QStringLiteral("C.sfc");
    int written = 0, deleted = 0;
    QVERIFY(cache.sync({ a, c }, { QStringLiteral("snes") }, loaded, &written, &deleted));
    QCOMPARE(written, 1);
    QCOMPARE(deleted, 1);
    const auto again = cache.load();
    QCOMPARE(again.size(), 3);
    QVERIFY(again.contains(n.id));
    QVERIFY(!again.contains(b.id));
}

void TestRomScanner::cacheRejectsBadRowsAndRecreates()
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        QSKIP("Qt SQLite driver missing (libqt6sql6-sqlite)");
    QTemporaryDir tmp;
    const QString path = tmp.path() + QStringLiteral("/library.sqlite");
    // Garbage file: recreated.
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(4096, 'z'));
    }
    {
        LibraryCache cache(path);
        QVERIFY(cache.open());
        QVERIFY(cache.load().isEmpty());
        // A row whose id doesn't match its path, and a "../" path.
        RomRecord bad { QStringLiteral("rom:snes:deadbeef"), QStringLiteral("snes"), QStringLiteral("/r"),
                        QStringLiteral("A.sfc"), QStringLiteral("A"), {}, {}, 1, 1, 1 };
        RomRecord escape { RomScanner::romId(QStringLiteral("snes"), QStringLiteral("../x")), QStringLiteral("snes"),
                           QStringLiteral("/r"), QStringLiteral("../x"), QStringLiteral("X"), {}, {}, 1, 1, 1 };
        QVERIFY(cache.sync({ bad, escape }, { QStringLiteral("snes") }, {}));
        QVERIFY(cache.load().isEmpty());
    }
    // A symlink in place of the database is replaced, not followed.
    QFile::remove(path);
    const QString target = tmp.path() + QStringLiteral("/victim");
    {
        QFile f(target);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("precious");
    }
    QVERIFY(QFile::link(target, path));
    LibraryCache cache(path);
    QVERIFY(cache.open());
    QVERIFY(!QFileInfo(path).isSymLink());
    QFile f(target);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("precious"));
}

void TestRomScanner::twentyThousandRoms()
{
    QTemporaryDir tmp;
    for (int d = 0; d < 20; ++d) {
        const QString dir = tmp.path() + QStringLiteral("/set%1").arg(d);
        QDir().mkpath(dir);
        for (int i = 0; i < 1000; ++i) {
            QFile f(dir + QStringLiteral("/Game %1-%2 (USA).sfc").arg(d).arg(i));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
    }
    QElapsedTimer timer;
    timer.start();
    const RomScanSpec spec { QStringLiteral("snes"), { QStringLiteral("sfc") }, { tmp.path() } };
    const auto records = RomScanner::scan(spec, {});
    const qint64 cold = timer.restart();
    QCOMPARE(records.size(), 20000);
    if (QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        LibraryCache cache(tmp.path() + QStringLiteral("/c.sqlite"));
        QVERIFY(cache.open());
        QVERIFY(cache.sync(records, { QStringLiteral("snes") }, {}));
        const qint64 write = timer.restart();
        const auto loaded = cache.load();
        QCOMPARE(loaded.size(), 20000);
        const qint64 load = timer.restart();
        const auto again = RomScanner::scan(spec, loaded);
        QCOMPARE(again.size(), 20000);
        int written = -1;
        QVERIFY(cache.sync(again, { QStringLiteral("snes") }, loaded, &written));
        QCOMPARE(written, 0);   // nothing changed: nothing written
        qInfo("20k ROMs: scan %lld ms, cache write %lld ms, load %lld ms, rescan+diff %lld ms",
              cold, write, load, timer.elapsed());
    }
    QVERIFY2(cold < 10000, qPrintable(QString::number(cold)));
}

QTEST_GUILESS_MAIN(TestRomScanner)
#include "tst_romscanner.moc"
