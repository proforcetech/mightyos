#include "favorites.h"
#include "history.h"
#include "itemresolver.h"
#include "profileservice.h"

#include <QAbstractItemModelTester>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QRegularExpression>

namespace {

// Resolves whatever is in `items`; tests remove entries to simulate an
// uninstalled app or a deleted ROM.
using namespace Qt::StringLiterals;

class FakeResolver : public ItemResolver
{
public:
    QHash<QString, TileItem> items;
    bool pending = false;   // app keys "not scanned yet"

    void add(const QString &key, const QString &title, const QString &kind = QStringLiteral("game"))
    {
        TileItem t;
        t.key = key;
        t.title = title;
        t.kind = kind;
        items.insert(key, t);
    }
    bool isPending(const QString &key) const override
    {
        return pending && key.startsWith(QLatin1String("desktop:"));
    }
    std::optional<TileItem> resolve(const QString &key) const override
    {
        const auto found = items.constFind(key);
        return found == items.cend() ? std::nullopt : std::optional<TileItem>(*found);
    }
};

} // namespace

// Favorites and History: toggle, order, missing targets, per-profile
// isolation, files.
class TestFavorites : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void toggleAndOrder();
    void moveKeepsModelInSync();
    void missingTargetIsUnavailable();
    void perProfileIsolation();
    void fileFormatAndValidation();
    void invalidKeysAreRefused();
    void historyRecordsAndOrders();
    void historyIsPerProfileAndSkipsMissing();
    void historyCapsEntries();
    void unusableFilesAreKept();
    void pendingAppsBeforeFirstScan();

private:
    QJsonObject readJson(const QString &path) const;

    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<ProfileService> m_profiles;
    std::unique_ptr<FakeResolver> m_resolver;
};

void TestFavorites::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());
    m_profiles = std::make_unique<ProfileService>(m_tmp->path() + QStringLiteral("/config"),
                                                  m_tmp->path() + QStringLiteral("/state"));
    m_resolver = std::make_unique<FakeResolver>();
    m_resolver->add(QStringLiteral("rom:snes:1"), QStringLiteral("Alpha"));
    m_resolver->add(QStringLiteral("rom:snes:2"), QStringLiteral("Beta"));
    m_resolver->add(QStringLiteral("desktop:vim"), QStringLiteral("Vim"), QStringLiteral("app"));
    m_resolver->add(QStringLiteral("snes"), QStringLiteral("SNES"), QStringLiteral("system"));
}

void TestFavorites::cleanup()
{
    m_resolver.reset();
    m_profiles.reset();
}

QJsonObject TestFavorites::readJson(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void TestFavorites::toggleAndOrder()
{
    Favorites favorites(m_profiles.get(), m_resolver.get());
    QAbstractItemModelTester tester(favorites.model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    QSignalSpy changed(&favorites, &Favorites::changed);
    QCOMPARE(favorites.count(), 0);

    QVERIFY(favorites.toggle(QStringLiteral("rom:snes:2")));    // a game
    QVERIFY(favorites.toggle(QStringLiteral("desktop:vim")));   // an app
    QVERIFY(favorites.toggle(QStringLiteral("snes")));          // a system
    QCOMPARE(favorites.ids(), (QStringList { u"rom:snes:2"_s, u"desktop:vim"_s, u"snes"_s }));
    QCOMPARE(changed.count(), 3);
    QVERIFY(favorites.isFavorite(QStringLiteral("desktop:vim")));
    QCOMPARE(favorites.model()->count(), 3);
    QCOMPARE(favorites.model()->keyAt(1), QStringLiteral("desktop:vim"));
    QCOMPARE(favorites.model()->get(1).value(QStringLiteral("kind")).toString(), QStringLiteral("app"));

    QVERIFY(!favorites.toggle(QStringLiteral("desktop:vim")));  // off again
    QVERIFY(!favorites.isFavorite(QStringLiteral("desktop:vim")));
    QCOMPARE(favorites.ids(), (QStringList { u"rom:snes:2"_s, u"snes"_s }));
    QCOMPARE(favorites.model()->indexOfKey(QStringLiteral("snes")), 1);

    // Saved order survives a restart.
    Favorites reloaded(m_profiles.get(), m_resolver.get());
    QCOMPARE(reloaded.ids(), (QStringList { u"rom:snes:2"_s, u"snes"_s }));
}

void TestFavorites::moveKeepsModelInSync()
{
    Favorites favorites(m_profiles.get(), m_resolver.get());
    QAbstractItemModelTester tester(favorites.model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    for (const char *key : { "rom:snes:1", "rom:snes:2", "desktop:vim" })
        QVERIFY(favorites.add(QString::fromLatin1(key)));
    QSignalSpy moved(favorites.model(), &QAbstractItemModel::rowsMoved);
    QSignalSpy reset(favorites.model(), &QAbstractItemModel::modelReset);

    QVERIFY(favorites.move(QStringLiteral("desktop:vim"), -1));
    QCOMPARE(favorites.ids(), (QStringList { u"rom:snes:1"_s, u"desktop:vim"_s, u"rom:snes:2"_s }));
    QVERIFY(favorites.move(QStringLiteral("desktop:vim"), -1));
    QCOMPARE(favorites.ids(), (QStringList { u"desktop:vim"_s, u"rom:snes:1"_s, u"rom:snes:2"_s }));
    QVERIFY(!favorites.move(QStringLiteral("desktop:vim"), -1));   // first already
    QVERIFY(!favorites.move(QStringLiteral("rom:snes:2"), 1));     // last already
    QVERIFY(!favorites.move(QStringLiteral("nope"), 1));
    QVERIFY(favorites.move(QStringLiteral("rom:snes:1"), 1));
    QCOMPARE(favorites.ids(), (QStringList { u"desktop:vim"_s, u"rom:snes:2"_s, u"rom:snes:1"_s }));

    QCOMPARE(moved.count(), 3);
    QCOMPARE(reset.count(), 0);                                    // moves, not resets
    for (int i = 0; i < favorites.count(); ++i)
        QCOMPARE(favorites.model()->keyAt(i), favorites.ids().at(i));
    Favorites reloaded(m_profiles.get(), m_resolver.get());
    QCOMPARE(reloaded.ids(), favorites.ids());
}

void TestFavorites::missingTargetIsUnavailable()
{
    {
        Favorites favorites(m_profiles.get(), m_resolver.get());
        QVERIFY(favorites.add(QStringLiteral("desktop:vim")));
        QVERIFY(favorites.add(QStringLiteral("rom:snes:1")));
    }
    // Vim gets uninstalled.
    m_resolver->items.remove(QStringLiteral("desktop:vim"));
    Favorites favorites(m_profiles.get(), m_resolver.get());
    QCOMPARE(favorites.count(), 2);                                 // still listed...
    const QVariantMap row = favorites.model()->get(0);
    QCOMPARE(row.value(QStringLiteral("key")).toString(), QStringLiteral("desktop:vim"));
    QCOMPARE(row.value(QStringLiteral("available")).toBool(), false);   // ...greyed
    QCOMPARE(row.value(QStringLiteral("title")).toString(), QStringLiteral("Vim"));   // label snapshot
    QCOMPARE(row.value(QStringLiteral("kind")).toString(), QStringLiteral("app"));
    QCOMPARE(favorites.model()->get(1).value(QStringLiteral("available")).toBool(), true);

    // It comes back (re-scan): the tile turns available in place.
    m_resolver->add(QStringLiteral("desktop:vim"), QStringLiteral("Vim"), QStringLiteral("app"));
    QSignalSpy reset(favorites.model(), &QAbstractItemModel::modelReset);
    emit m_resolver->changed();
    QCOMPARE(reset.count(), 0);
    QCOMPARE(favorites.model()->get(0).value(QStringLiteral("available")).toBool(), true);

    // Removing an unavailable favorite works.
    m_resolver->items.remove(QStringLiteral("desktop:vim"));
    emit m_resolver->changed();
    QVERIFY(favorites.remove(QStringLiteral("desktop:vim")));
    QCOMPARE(favorites.ids(), QStringList { u"rom:snes:1"_s });
}

void TestFavorites::perProfileIsolation()
{
    Favorites favorites(m_profiles.get(), m_resolver.get());
    QVERIFY(favorites.add(QStringLiteral("rom:snes:1")));
    const QString other = m_profiles->create(QStringLiteral("Comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    QSignalSpy changed(&favorites, &Favorites::changed);
    QVERIFY(m_profiles->select(other));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(favorites.profileId(), other);
    QCOMPARE(favorites.count(), 0);
    QVERIFY(!favorites.isFavorite(QStringLiteral("rom:snes:1")));
    QVERIFY(favorites.add(QStringLiteral("desktop:vim")));
    QVERIFY(favorites.filePath().contains(QStringLiteral("/profiles/") + other + QStringLiteral("/favorites.json")));

    QVERIFY(m_profiles->select(ProfileService::kDefaultId));
    QCOMPARE(favorites.ids(), QStringList { u"rom:snes:1"_s });
    QVERIFY(m_profiles->select(other));
    QCOMPARE(favorites.ids(), QStringList { u"desktop:vim"_s });

    // Deleting the profile deletes its favorites.
    const QString otherFile = favorites.filePath();
    QVERIFY(m_profiles->remove(other));
    QVERIFY(!QFile::exists(otherFile));
    QCOMPARE(favorites.ids(), QStringList { u"rom:snes:1"_s });       // back on the default profile
}

void TestFavorites::fileFormatAndValidation()
{
    Favorites favorites(m_profiles.get(), m_resolver.get());
    QVERIFY(favorites.add(QStringLiteral("rom:snes:1")));
    const QJsonObject root = readJson(favorites.filePath());
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(root.value(QStringLiteral("items")).toArray().at(0).toString(), QStringLiteral("rom:snes:1"));
    QCOMPARE(root.value(QStringLiteral("labels")).toObject().value(QStringLiteral("rom:snes:1")).toString(),
             QStringLiteral("Alpha"));

    // Hand-edited file: junk entries are dropped, duplicates collapsed.
    QFile file(favorites.filePath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(R"json({"version":1,"items":["rom:snes:2",42,"has space","","rom:snes:2","snes"]})json");
    file.close();
    Favorites reloaded(m_profiles.get(), m_resolver.get());
    QCOMPARE(reloaded.ids(), (QStringList { u"rom:snes:2"_s, u"snes"_s }));

    // Wrong version or garbage: empty, with a warning, and never a crash.
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("{\"version\":7,\"items\":[\"snes\"]}");
    file.close();
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("ignoring unusable favorites")));
    Favorites wrongVersion(m_profiles.get(), m_resolver.get());
    QCOMPARE(wrongVersion.count(), 0);
    QVERIFY(QFile::exists(favorites.filePath() + QStringLiteral(".unsupported")));
}

void TestFavorites::invalidKeysAreRefused()
{
    Favorites favorites(m_profiles.get(), m_resolver.get());
    QVERIFY(!favorites.add(QString()));
    QVERIFY(!favorites.add(QStringLiteral("has space")));
    QVERIFY(!favorites.add(QString(513, QLatin1Char('k'))));
    QVERIFY(!favorites.toggle(QStringLiteral("line\nbreak")));
    QCOMPARE(favorites.count(), 0);
    QVERIFY(favorites.add(QStringLiteral("rom:snes:1")));
    QVERIFY(!favorites.add(QStringLiteral("rom:snes:1")));          // already there
}

void TestFavorites::historyRecordsAndOrders()
{
    History history(m_profiles.get(), m_resolver.get());
    QSignalSpy recorded(&history, &History::recorded);
    QCOMPARE(history.recent()->count(), 0);
    QVERIFY(history.record(QStringLiteral("rom:snes:1")));
    QVERIFY(history.record(QStringLiteral("desktop:vim")));
    QVERIFY(history.record(QStringLiteral("rom:snes:1")));
    QCOMPARE(recorded.count(), 3);
    QCOMPARE(history.launchCount(QStringLiteral("rom:snes:1")), 2);
    QCOMPARE(history.launchCount(QStringLiteral("desktop:vim")), 1);
    QCOMPARE(history.launchCount(QStringLiteral("snes")), 0);
    QVERIFY(history.lastPlayed(QStringLiteral("rom:snes:1")) > history.lastPlayed(QStringLiteral("desktop:vim")));
    // Newest first.
    QCOMPARE(history.recent()->keyAt(0), QStringLiteral("rom:snes:1"));
    QCOMPARE(history.recent()->keyAt(1), QStringLiteral("desktop:vim"));

    History reloaded(m_profiles.get(), m_resolver.get());
    QCOMPARE(reloaded.launchCount(QStringLiteral("rom:snes:1")), 2);
    QCOMPARE(reloaded.recent()->keyAt(0), QStringLiteral("rom:snes:1"));
    const QJsonObject root = readJson(history.filePath());
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(root.value(QStringLiteral("items")).toObject().value(QStringLiteral("rom:snes:1"))
                 .toObject().value(QStringLiteral("count")).toInt(), 2);
    QVERIFY(history.filePath().startsWith(m_tmp->path() + QStringLiteral("/state/profiles/default/")));
    QVERIFY(!history.record(QStringLiteral("bad key")));
}

void TestFavorites::historyIsPerProfileAndSkipsMissing()
{
    History history(m_profiles.get(), m_resolver.get());
    QVERIFY(history.record(QStringLiteral("desktop:vim")));
    QVERIFY(history.record(QStringLiteral("rom:snes:2")));
    m_resolver->items.remove(QStringLiteral("rom:snes:2"));
    emit m_resolver->changed();
    QCOMPARE(history.recent()->count(), 1);                         // gone targets drop out
    QCOMPARE(history.recent()->keyAt(0), QStringLiteral("desktop:vim"));

    const QString other = m_profiles->create(QStringLiteral("Comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    QVERIFY(m_profiles->select(other));
    QCOMPARE(history.recent()->count(), 0);
    QCOMPARE(history.launchCount(QStringLiteral("desktop:vim")), 0);
    QVERIFY(m_profiles->select(ProfileService::kDefaultId));
    QCOMPARE(history.launchCount(QStringLiteral("desktop:vim")), 1);
}

void TestFavorites::historyCapsEntries()
{
    History history(m_profiles.get(), m_resolver.get());
    for (int i = 0; i < History::kMaxEntries + 5; ++i)
        history.record(QStringLiteral("rom:x:%1").arg(i));
    QCOMPARE(history.launchCount(QStringLiteral("rom:x:0")), 0);    // oldest dropped
    QCOMPARE(history.launchCount(QStringLiteral("rom:x:%1").arg(History::kMaxEntries + 4)), 1);
    QVERIFY(history.recent()->count() <= History::kRecentCount);
}

void TestFavorites::unusableFilesAreKept()
{
    QString favPath, histPath;
    {
        Favorites favorites(m_profiles.get(), m_resolver.get());
        History history(m_profiles.get(), m_resolver.get());
        QVERIFY(favorites.add(QStringLiteral("snes")));
        QVERIFY(history.record(QStringLiteral("snes")));
        favPath = favorites.filePath();
        histPath = history.filePath();
    }
    auto overwrite = [](const QString &path, const QByteArray &data) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    };
    // A file from a newer MightyOS: never overwritten by our next write.
    const QByteArray newer = R"json({"version":2,"items":{"future":"format"}})json";
    overwrite(favPath, newer);
    overwrite(histPath, "{ truncated");
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("ignoring unusable favorites")));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("ignoring unusable history")));
    Favorites favorites(m_profiles.get(), m_resolver.get());
    History history(m_profiles.get(), m_resolver.get());
    QCOMPARE(favorites.count(), 0);
    QVERIFY(favorites.add(QStringLiteral("rom:snes:1")));            // writes a fresh file...
    QVERIFY(history.record(QStringLiteral("rom:snes:1")));
    QFile kept(favPath + QStringLiteral(".unsupported"));            // ...the newer one is kept
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), newer);
    QVERIFY(QFile::exists(histPath + QStringLiteral(".corrupt")));
}

void TestFavorites::pendingAppsBeforeFirstScan()
{
    {
        Favorites favorites(m_profiles.get(), m_resolver.get());
        QVERIFY(favorites.add(QStringLiteral("desktop:vim")));
    }
    // Next start, before the app scan: Vim doesn't resolve *yet*.
    m_resolver->items.remove(QStringLiteral("desktop:vim"));
    m_resolver->pending = true;
    Favorites favorites(m_profiles.get(), m_resolver.get());
    QVariantMap row = favorites.model()->get(0);
    QCOMPARE(row.value(QStringLiteral("available")).toBool(), true);   // not "Unavailable"
    QCOMPARE(row.value(QStringLiteral("title")).toString(), QStringLiteral("Vim"));
    QVERIFY(!row.value(QStringLiteral("subtitle")).toString().isEmpty());
    // The scan finds it: same row, now resolved, in place.
    QSignalSpy changed(favorites.model(), &QAbstractItemModel::dataChanged);
    m_resolver->pending = false;
    m_resolver->add(QStringLiteral("desktop:vim"), QStringLiteral("Vim"), QStringLiteral("app"));
    emit m_resolver->changed();
    QCOMPARE(changed.count(), 1);
    QCOMPARE(favorites.model()->get(0).value(QStringLiteral("subtitle")).toString(), QString());
    // Or doesn't: only then it is unavailable.
    m_resolver->items.remove(QStringLiteral("desktop:vim"));
    emit m_resolver->changed();
    QCOMPARE(favorites.model()->get(0).value(QStringLiteral("available")).toBool(), false);
}

QTEST_GUILESS_MAIN(TestFavorites)
#include "tst_favorites.moc"
