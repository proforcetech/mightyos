#include "library.h"
#include "tilelistmodel.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
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
    Library library;
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
    Library library;
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
    Library library;
    QVERIFY(!library.loadFile(QStringLiteral("/nonexistent/library.json")));
    QCOMPARE(library.systems(QStringLiteral("emulators"))->count(), 9);   // old content kept
}

QTEST_GUILESS_MAIN(TestTileListModel)
#include "tst_tilelistmodel.moc"
