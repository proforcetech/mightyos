#include "library.h"
#include "tilelistmodel.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QTest>
#include <algorithm>

// The C++ model contract (PLAN M2 / M1 review) and the mock Library that
// feeds it.
class TestTileListModel : public QObject
{
    Q_OBJECT

private slots:
    void rolesAreStable();
    void indexOfKeyAfterReset();
    void indexOfKeyAfterInsertRemoveMove();
    void duplicateKeysFirstWins();
    void getAndKeyAt();
    void firstIndexForLetter();
    void sortKeyIsShared();
    void sameKeysUpdateInPlace();
    void qmlRows();
    void libraryKeepsMockIdsAndStructure();
    void libraryResolve();
    void libraryRejectsBadFiles();
    void diffSetItemsKeepsRows();
    void diffFallsBackToReset();
    void batchInsertRemove();
    void letterGroups();
    void largeDiffIsFast();
    void scatteredDiffResets();

private:
    static TileItem item(const QString &key, const QString &title, const QString &sortTitle = QString())
    {
        TileItem t;
        t.key = key;
        t.title = title;
        t.sortTitle = sortTitle;
        t.kind = QStringLiteral("game");
        return t;
    }
};

void TestTileListModel::rolesAreStable()
{
    TileListModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    const QList<QByteArray> names = roles.values();
    for (const char *name : { "key", "title", "subtitle", "icon", "artwork", "kind", "available",
                              "tint", "sortTitle", "badge" })
        QVERIFY2(names.contains(QByteArray(name)), name);
}

void TestTileListModel::indexOfKeyAfterReset()
{
    TileListModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QSignalSpy count(&model, &TileListModel::countChanged);
    QSignalSpy reset(&model, &TileListModel::itemsReset);
    model.setItems({ item(QStringLiteral("a"), QStringLiteral("A")), item(QStringLiteral("b"), QStringLiteral("B")) });
    QCOMPARE(model.count(), 2);
    QCOMPARE(count.count(), 1);
    QCOMPARE(reset.count(), 1);
    QCOMPARE(model.indexOfKey(QStringLiteral("b")), 1);
    model.setItems({ item(QStringLiteral("c"), QStringLiteral("C")), item(QStringLiteral("b"), QStringLiteral("B")),
                     item(QStringLiteral("a"), QStringLiteral("A")) });
    QCOMPARE(model.indexOfKey(QStringLiteral("a")), 2);
    QCOMPARE(model.indexOfKey(QStringLiteral("c")), 0);
    QCOMPARE(model.indexOfKey(QStringLiteral("zzz")), -1);
    QCOMPARE(model.indexOfKey(QString()), -1);
    model.clear();
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.indexOfKey(QStringLiteral("a")), -1);
}

void TestTileListModel::indexOfKeyAfterInsertRemoveMove()
{
    TileListModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    model.setItems({ item(QStringLiteral("a"), QStringLiteral("A")), item(QStringLiteral("c"), QStringLiteral("C")) });
    model.insertItem(1, item(QStringLiteral("b"), QStringLiteral("B")));
    QCOMPARE(model.indexOfKey(QStringLiteral("a")), 0);
    QCOMPARE(model.indexOfKey(QStringLiteral("b")), 1);
    QCOMPARE(model.indexOfKey(QStringLiteral("c")), 2);
    model.insertItem(0, item(QStringLiteral("z"), QStringLiteral("Z")));
    QCOMPARE(model.indexOfKey(QStringLiteral("c")), 3);
    QCOMPARE(model.indexOfKey(QStringLiteral("z")), 0);

    QVERIFY(model.removeAt(0));
    QCOMPARE(model.indexOfKey(QStringLiteral("z")), -1);
    QCOMPARE(model.indexOfKey(QStringLiteral("a")), 0);
    QVERIFY(!model.removeAt(7));

    QVERIFY(model.moveItem(0, 2));   // a b c -> b c a
    QCOMPARE(model.keyAt(0), QStringLiteral("b"));
    QCOMPARE(model.indexOfKey(QStringLiteral("a")), 2);
    QVERIFY(model.moveItem(2, 1));   // b c a -> b a c
    QCOMPARE(model.indexOfKey(QStringLiteral("a")), 1);
    QCOMPARE(model.indexOfKey(QStringLiteral("c")), 2);
    QVERIFY(!model.moveItem(0, 0));
    QVERIFY(!model.moveItem(0, 9));
}

void TestTileListModel::duplicateKeysFirstWins()
{
    TileListModel model;
    model.setItems({ item(QStringLiteral("x"), QStringLiteral("First")), item(QStringLiteral("x"), QStringLiteral("Second")) });
    QCOMPARE(model.indexOfKey(QStringLiteral("x")), 0);
}

void TestTileListModel::getAndKeyAt()
{
    TileListModel model;
    TileItem t = item(QStringLiteral("rom:snes:1"), QStringLiteral("Game"));
    t.subtitle = QStringLiteral("1990");
    t.available = false;
    t.badge = QStringLiteral("Kids");
    model.setItems({ t });
    const QVariantMap row = model.get(0);
    QCOMPARE(row.value(QStringLiteral("key")).toString(), QStringLiteral("rom:snes:1"));
    QCOMPARE(row.value(QStringLiteral("title")).toString(), QStringLiteral("Game"));
    QCOMPARE(row.value(QStringLiteral("subtitle")).toString(), QStringLiteral("1990"));
    QCOMPARE(row.value(QStringLiteral("kind")).toString(), QStringLiteral("game"));
    QCOMPARE(row.value(QStringLiteral("available")).toBool(), false);
    QCOMPARE(row.value(QStringLiteral("badge")).toString(), QStringLiteral("Kids"));
    QCOMPARE(row.value(QStringLiteral("sortTitle")).toString(), QStringLiteral("Game"));   // falls back to title
    QVERIFY(model.get(1).isEmpty());
    QVERIFY(model.get(-1).isEmpty());
    QCOMPARE(model.keyAt(0), QStringLiteral("rom:snes:1"));
    QCOMPARE(model.keyAt(5), QString());
}

void TestTileListModel::firstIndexForLetter()
{
    TileListModel model;
    model.setItems({ item(QStringLiteral("1"), QStringLiteral("3D Racer")),
                     item(QStringLiteral("2"), QStringLiteral("Alpha")),
                     item(QStringLiteral("3"), QStringLiteral("The Beta"), QStringLiteral("Beta, The")),
                     item(QStringLiteral("4"), QStringLiteral("delta")),
                     item(QStringLiteral("5"), QStringLiteral("Echo")) });
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("A")), 1);
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("b")), 2);   // sortTitle wins, case-insensitive
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("C")), 3);   // none with C: the next one
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("D")), 3);
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("Z")), -1);
    QCOMPARE(model.firstIndexForLetter(QString()), -1);
    // After a reset and an insert the answer follows the rows.
    model.insertItem(1, item(QStringLiteral("6"), QStringLiteral("Aardvark")));
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("A")), 1);
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("B")), 3);
    model.setItems({ item(QStringLiteral("7"), QStringLiteral("Zed")) });
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("A")), 0);
}

void TestTileListModel::sortKeyIsShared()
{
    QCOMPARE(TileListModel::sortKey(QStringLiteral("_Alpha")), QStringLiteral("alpha"));
    QCOMPARE(TileListModel::sortKey(QStringLiteral("[Beta] X")), QStringLiteral("beta] x"));
    QCOMPARE(TileListModel::sortKey(QString::fromUtf8("\u00c9chec")), QStringLiteral("echec"));
    QCOMPARE(TileListModel::sortKey(QString::fromUtf8("\u00c5ngstr\u00f6m")), QStringLiteral("angstrom"));
    QCOMPARE(TileListModel::sortKey(QStringLiteral("...")), QStringLiteral("..."));   // nothing left: as is

    // Sort with sortLess, then the letter jump agrees with that order.
    QStringList titles { QStringLiteral("zeta"), QString::fromUtf8("\u00c9chec"), QStringLiteral("[Beta] X"),
                         QStringLiteral("_Alpha"), QString::fromUtf8("\u00c5ngstr\u00f6m"), QStringLiteral("3D Racer"),
                         QStringLiteral("delta") };
    std::sort(titles.begin(), titles.end(), TileListModel::sortLess);
    QCOMPARE(titles, (QStringList { QStringLiteral("3D Racer"), QStringLiteral("_Alpha"),
                                    QString::fromUtf8("\u00c5ngstr\u00f6m"), QStringLiteral("[Beta] X"),
                                    QStringLiteral("delta"), QString::fromUtf8("\u00c9chec"), QStringLiteral("zeta") }));
    TileListModel model;
    QList<TileItem> items;
    for (const QString &t : std::as_const(titles))
        items.append(item(t, t));
    model.setItems(items);
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("a")), 1);          // _Alpha
    QCOMPARE(model.firstIndexForLetter(QString::fromUtf8("\u00c5")), 1); // folded like the titles
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("B")), 3);          // [Beta] X
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("e")), 5);          // Échec
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("f")), 6);          // next after: zeta
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("1")), 0);
}

void TestTileListModel::sameKeysUpdateInPlace()
{
    TileListModel model;
    model.setItems({ item(QStringLiteral("a"), QStringLiteral("A")), item(QStringLiteral("b"), QStringLiteral("B")) });
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.setItems({ item(QStringLiteral("a"), QStringLiteral("A")), item(QStringLiteral("b"), QStringLiteral("B2")) });
    QCOMPARE(reset.count(), 0);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(model.get(1).value(QStringLiteral("title")).toString(), QStringLiteral("B2"));
}

void TestTileListModel::qmlRows()
{
    TileListModel model;
    model.setRows({ QVariantMap { { QStringLiteral("key"), QStringLiteral("k1") }, { QStringLiteral("title"), QStringLiteral("One") } } });
    model.appendRow({ { QStringLiteral("key"), QStringLiteral("k2") }, { QStringLiteral("title"), QStringLiteral("Two") },
                      { QStringLiteral("available"), false } });
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.indexOfKey(QStringLiteral("k2")), 1);
    QCOMPARE(model.get(0).value(QStringLiteral("available")).toBool(), true);
    QCOMPARE(model.get(1).value(QStringLiteral("available")).toBool(), false);
}

void TestTileListModel::libraryKeepsMockIdsAndStructure()
{
    Library library(nullptr);
    TileListModel *emulators = library.systems(QStringLiteral("emulators"));
    TileListModel *games = library.systems(QStringLiteral("games"));
    QCOMPARE(emulators->count(), 9);
    QCOMPARE(emulators->keyAt(0), QStringLiteral("nes"));
    QCOMPARE(emulators->keyAt(8), QStringLiteral("arcade"));
    QCOMPARE(games->count(), 1);                         // Games -> [Steam] (D12)
    QCOMPARE(games->keyAt(0), QStringLiteral("steam"));
    QCOMPARE(library.systems(QStringLiteral("emulators")), emulators);   // cached, same object

    TileListModel *snes = library.games(QStringLiteral("snes"));
    QVERIFY(snes);
    QCOMPARE(snes->count(), 40);
    // Same FNV-based ids as the M1 mock (navstate keys keep working).
    QCOMPARE(snes->keyAt(0), QStringLiteral("rom:snes:5d6eb12d"));
    QCOMPARE(snes->get(0).value(QStringLiteral("title")).toString(), QStringLiteral("Aero Blaster Zero"));
    QCOMPARE(library.games(QStringLiteral("steam"))->count(), 16);
    QCOMPARE(library.games(QStringLiteral("steam"))->keyAt(0), QStringLiteral("steam:200000"));
    QVERIFY(!library.games(QStringLiteral("uninstalled-console")));

    QCOMPARE(library.adjacentSystem(QStringLiteral("snes"), 1), QStringLiteral("n64"));
    QCOMPARE(library.adjacentSystem(QStringLiteral("nes"), -1), QString());
    QCOMPARE(library.adjacentSystem(QStringLiteral("steam"), 1), QString());
    QCOMPARE(library.system(QStringLiteral("ps1")).value(QStringLiteral("fullName")).toString(), QStringLiteral("PlayStation"));
}

void TestTileListModel::libraryResolve()
{
    Library library(nullptr);
    const auto game = library.resolve(QStringLiteral("rom:snes:5d6eb12d"));
    QVERIFY(game);
    QCOMPARE(game->kind, QStringLiteral("game"));
    QCOMPARE(game->subtitle, QStringLiteral("SNES"));   // favorites show the system
    const auto system = library.resolve(QStringLiteral("n64"));
    QVERIFY(system);
    QCOMPARE(system->kind, QStringLiteral("system"));
    QVERIFY(!library.resolve(QStringLiteral("rom:snes:deadbeef")));
    QVERIFY(!library.resolve(QStringLiteral("desktop:vim")));
}

void TestTileListModel::libraryRejectsBadFiles()
{
    Library library(nullptr);
    QVERIFY(!library.loadFile(QStringLiteral("/nonexistent/library.json")));
    QCOMPARE(library.systems(QStringLiteral("emulators"))->count(), 9);   // old content kept
}

// M4: setItems() is a diff when the keys only gained or lost rows.
void TestTileListModel::diffSetItemsKeepsRows()
{
    TileListModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QList<TileItem> items;
    for (int i = 0; i < 10; ++i)
        items.append(item(QStringLiteral("k%1").arg(i), QStringLiteral("T%1").arg(i)));
    model.setItems(items);
    QCOMPARE(model.resetCount(), 1);

    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy count(&model, &TileListModel::countChanged);
    QList<TileItem> next = items;
    next.removeAt(7);
    next.removeAt(2);
    next.insert(0, item(QStringLiteral("new0"), QStringLiteral("A")));
    next.insert(5, item(QStringLiteral("new5a"), QStringLiteral("B")));
    next.insert(6, item(QStringLiteral("new5b"), QStringLiteral("C")));
    next.append(item(QStringLiteral("last"), QStringLiteral("Z")));
    next[3].subtitle = QStringLiteral("changed");
    model.setItems(next);
    QCOMPARE(model.resetCount(), 1);            // no reset
    QCOMPARE(model.items(), next);
    QCOMPARE(removed.count(), 2);               // two runs of one
    QCOMPARE(inserted.count(), 3);              // [new0], [new5a, new5b], [last]
    QCOMPARE(changed.count(), 1);
    QCOMPARE(count.count(), 1);                 // once, at the end
    for (int i = 0; i < next.size(); ++i)
        QCOMPARE(model.indexOfKey(next.at(i).key), i);
    QCOMPARE(model.indexOfKey(QStringLiteral("k2")), -1);
    QCOMPARE(model.firstIndexForLetter(QStringLiteral("Z")), next.size() - 1);
}

void TestTileListModel::diffFallsBackToReset()
{
    TileListModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    model.setItems({ item(QStringLiteral("a"), QStringLiteral("A")), item(QStringLiteral("b"), QStringLiteral("B")),
                     item(QStringLiteral("c"), QStringLiteral("C")) });
    const int base = model.resetCount();
    // Survivors reordered: reset.
    model.setItems({ item(QStringLiteral("c"), QStringLiteral("C")), item(QStringLiteral("a"), QStringLiteral("A")) });
    QCOMPARE(model.resetCount(), base + 1);
    // Duplicate keys in the new list: reset, first row wins.
    model.setItems({ item(QStringLiteral("c"), QStringLiteral("C")), item(QStringLiteral("x"), QStringLiteral("1")),
                     item(QStringLiteral("x"), QStringLiteral("2")) });
    QCOMPARE(model.resetCount(), base + 2);
    QCOMPARE(model.indexOfKey(QStringLiteral("x")), 1);
    // Back to unique keys: a diff again, hash correct.
    model.setItems({ item(QStringLiteral("c"), QStringLiteral("C")), item(QStringLiteral("x"), QStringLiteral("1")),
                     item(QStringLiteral("y"), QStringLiteral("Y")) });
    QCOMPARE(model.indexOfKey(QStringLiteral("y")), 2);
    QCOMPARE(model.indexOfKey(QStringLiteral("x")), 1);
}

void TestTileListModel::batchInsertRemove()
{
    TileListModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    model.insertItems(0, { item(QStringLiteral("a"), QStringLiteral("A")), item(QStringLiteral("d"), QStringLiteral("D")) });
    model.insertItems(1, { item(QStringLiteral("b"), QStringLiteral("B")), item(QStringLiteral("c"), QStringLiteral("C")) });
    QCOMPARE(inserted.count(), 2);
    QCOMPARE(model.keyAt(2), QStringLiteral("c"));
    QCOMPARE(model.indexOfKey(QStringLiteral("d")), 3);
    QCOMPARE(model.letterAt(3), QStringLiteral("D"));
    QVERIFY(model.removeItems(1, 2));
    QCOMPARE(removed.count(), 1);
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.indexOfKey(QStringLiteral("d")), 1);
    QCOMPARE(model.indexOfKey(QStringLiteral("b")), -1);
    QVERIFY(!model.removeItems(1, 5));
    QVERIFY(!model.removeItems(-1, 1));
}

void TestTileListModel::letterGroups()
{
    TileListModel model;
    model.setItems({ item(QStringLiteral("1"), QStringLiteral("1942")), item(QStringLiteral("2"), QStringLiteral("Alpha")),
                     item(QStringLiteral("3"), QStringLiteral("Axe")), item(QStringLiteral("4"), QStringLiteral("Bomb")),
                     item(QStringLiteral("5"), QStringLiteral("Échec")), item(QStringLiteral("6"), QStringLiteral("Echo")) });
    QCOMPARE(model.letterAt(0), QStringLiteral("#"));
    QCOMPARE(model.letterAt(4), QStringLiteral("E"));   // accents folded
    QCOMPARE(model.letterAt(9), QString());
    QCOMPARE(model.letterStep(0, 1), 1);
    QCOMPARE(model.letterStep(1, 1), 3);
    QCOMPARE(model.letterStep(2, 1), 3);
    QCOMPARE(model.letterStep(4, 1), -1);   // E is the last group
    QCOMPARE(model.letterStep(2, -1), 1);   // start of the current group first
    QCOMPARE(model.letterStep(1, -1), 0);   // then the previous group
    QCOMPARE(model.letterStep(5, -1), 4);
    QCOMPARE(model.letterStep(0, -1), -1);
}

void TestTileListModel::largeDiffIsFast()
{
    TileListModel model;
    QList<TileItem> items;
    for (int i = 0; i < 20000; ++i)
        items.append(item(QStringLiteral("rom:x:%1").arg(i, 6, 10, QLatin1Char('0')),
                          QStringLiteral("Game %1").arg(i, 6, 10, QLatin1Char('0'))));
    model.setItems(items);
    QList<TileItem> next = items;
    next.removeAt(100);
    next.insert(5000, item(QStringLiteral("rom:x:new"), QStringLiteral("Game 005000a")));
    QElapsedTimer timer;
    timer.start();
    model.setItems(next);
    QCOMPARE(model.resetCount(), 1);
    QCOMPARE(model.items(), next);
    QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
}

// H3 (M4 review): 10k scattered edits on 20k rows took 2.3 s as a diff.
// Above kMaxDiffRuns runs it is one reset.
void TestTileListModel::scatteredDiffResets()
{
    TileListModel model;
    QList<TileItem> items;
    for (int i = 0; i < 20000; ++i)
        items.append(item(QStringLiteral("k%1").arg(i, 6, 10, QLatin1Char('0')), QStringLiteral("T%1").arg(i)));
    model.setItems(items);
    const int base = model.resetCount();
    QList<TileItem> scattered;
    for (int i = 0; i < items.size(); ++i) {
        if (i % 2 == 0)
            scattered.append(items.at(i));   // every other row removed: 10k runs
    }
    QElapsedTimer timer;
    timer.start();
    model.setItems(scattered);
    QVERIFY2(timer.elapsed() < 300, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(model.resetCount(), base + 1);
    QCOMPARE(model.items(), scattered);
    QCOMPARE(model.indexOfKey(scattered.last().key), scattered.size() - 1);
    // A handful of runs is still a diff, and the hash is right afterwards.
    QList<TileItem> few = scattered;
    for (int i = 0; i < TileListModel::kMaxDiffRuns / 2; ++i)
        few.removeAt(i * 300);
    model.setItems(few);
    QCOMPARE(model.resetCount(), base + 1);
    for (int i = 0; i < few.size(); i += 997)
        QCOMPARE(model.indexOfKey(few.at(i).key), i);
    QCOMPARE(model.indexOfKey(scattered.at(0).key), -1);
}

QTEST_GUILESS_MAIN(TestTileListModel)
#include "tst_tilelistmodel.moc"
