#include "envguard.h"
#include "navstate.h"
#include "profileservice.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

// ProfileService: first run, create/rename/delete, last-profile
// protection, the startup rule, id validation, atomic writes.
using namespace Qt::StringLiterals;

class TestProfiles : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void firstRunCreatesDefault();
    void defaultDirsFollowXdg();
    void migratesLegacyNavState();
    void createAndValidate();
    void renameAndAppearance();
    void deleteRemovesDataAndProtectsLast();
    void deleteActiveSwitches();
    void startupRule();
    void selectOrdersByLastUse();
    void invalidIdsInFileAreDropped();
    void corruptFileIsMovedAside();
    void atomicWriteFailureLeavesStateUnchanged();
    void suggestedNamesAreUnused();
    void profileDirsRejectInvalidIds();
    void updateIsOneAtomicWrite();
    void deleteDoesNotFollowSymlinks();
    void deleteRefusesSymlinkedTree();

private:
    ProfileService *make();
    QJsonObject readFile() const;

    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<ProfileService> m_service;
    QString m_config, m_state;
};

void TestProfiles::init()
{
    m_service.reset();
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());
    m_config = m_tmp->path() + QStringLiteral("/config/mightyos");
    m_state = m_tmp->path() + QStringLiteral("/state/mightyos");
}

ProfileService *TestProfiles::make()
{
    m_service = std::make_unique<ProfileService>(m_config, m_state);
    return m_service.get();
}

QJsonObject TestProfiles::readFile() const
{
    QFile file(m_config + QStringLiteral("/profiles.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void TestProfiles::firstRunCreatesDefault()
{
    ProfileService *s = make();
    QCOMPARE(s->count(), 1);
    QCOMPARE(s->activeId(), ProfileService::kDefaultId);
    QVERIFY(!s->pickerAtStartup());
    const QJsonObject root = readFile();
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 1);
    const QJsonObject p = root.value(QStringLiteral("profiles")).toArray().at(0).toObject();
    QCOMPARE(p.value(QStringLiteral("id")).toString(), QStringLiteral("default"));
    QCOMPARE(p.value(QStringLiteral("kind")).toString(), QStringLiteral("standard"));
    QVERIFY(ProfileService::avatars().contains(p.value(QStringLiteral("avatar")).toString()));
    QVERIFY(ProfileService::colors().contains(p.value(QStringLiteral("color")).toString()));
    QVERIFY(QDateTime::fromString(p.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs).isValid());
    QVERIFY(QDateTime::fromString(p.value(QStringLiteral("lastUsed")).toString(), Qt::ISODateWithMs).isValid());
    // A second start reads it back instead of creating another one.
    make();
    QCOMPARE(m_service->count(), 1);
}

void TestProfiles::defaultDirsFollowXdg()
{
    const EnvGuard config("XDG_CONFIG_HOME", (m_tmp->path() + QStringLiteral("/xc")).toUtf8());
    const EnvGuard state("XDG_STATE_HOME", (m_tmp->path() + QStringLiteral("/xs")).toUtf8());
    ProfileService s;
    QCOMPARE(s.filePath(), m_tmp->path() + QStringLiteral("/xc/mightyos/profiles.json"));
    QCOMPARE(s.profileStateDir(QStringLiteral("default")), m_tmp->path() + QStringLiteral("/xs/mightyos/profiles/default"));
    QCOMPARE(s.profileConfigDir(QStringLiteral("default")), m_tmp->path() + QStringLiteral("/xc/mightyos/profiles/default"));
    QVERIFY(QFile::exists(s.filePath()));
}

void TestProfiles::migratesLegacyNavState()
{
    QVERIFY(QDir().mkpath(m_state));
    QFile legacy(m_state + QStringLiteral("/navstate.json"));
    QVERIFY(legacy.open(QIODevice::WriteOnly));
    legacy.write(R"json({"version":1,"path":[{"screen":"home","row":1,"index":2,"key":"n64",
                 "extra":{"category":"emulators"}},{"screen":"games","params":{"system":"n64"},"index":7}]})json");
    legacy.close();
    make();
    QVERIFY(!QFile::exists(m_state + QStringLiteral("/navstate.json")));
    QVERIFY(QFile::exists(m_state + QStringLiteral("/profiles/default/navstate.json")));
    // And NavState reads it back as the default profile's path.
    NavState navState;
    navState.setDirectory(m_state);
    const QVariantList path = navState.load(QStringLiteral("default"));
    QCOMPARE(path.size(), 2);
    QCOMPARE(path.at(0).toMap().value(QStringLiteral("key")).toString(), QStringLiteral("n64"));
    QCOMPARE(path.at(1).toMap().value(QStringLiteral("index")).toInt(), 7);
}

void TestProfiles::createAndValidate()
{
    ProfileService *s = make();
    QSignalSpy changed(s, &ProfileService::profilesChanged);
    const QString id = s->create(QStringLiteral("  Comet  "), QStringLiteral("cat"), QStringLiteral("#ff6b8b"),
                                 QStringLiteral("kids"));
    QVERIFY(QRegularExpression(QStringLiteral("^p[0-9a-f]{10}$")).match(id).hasMatch());
    QCOMPARE(changed.count(), 1);
    QCOMPARE(s->count(), 2);
    const QVariantMap p = s->profile(id);
    QCOMPARE(p.value(QStringLiteral("name")).toString(), QStringLiteral("Comet"));   // trimmed
    QCOMPARE(p.value(QStringLiteral("kind")).toString(), QStringLiteral("kids"));
    QCOMPARE(s->activeId(), ProfileService::kDefaultId);   // creating doesn't switch

    // Invalid input is refused and nothing is written.
    QVERIFY(s->create(QStringLiteral(""), QStringLiteral("cat"), QStringLiteral("#ff6b8b")).isEmpty());
    QVERIFY(s->create(QStringLiteral("comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b")).isEmpty());  // taken
    QVERIFY(s->create(QString(ProfileService::kMaxNameLength + 1, QLatin1Char('x')), QStringLiteral("cat"),
                      QStringLiteral("#ff6b8b")).isEmpty());
    QVERIFY(s->create(QStringLiteral("Bad\x01Name"), QStringLiteral("cat"), QStringLiteral("#ff6b8b")).isEmpty());
    QVERIFY(s->create(QStringLiteral("Nova"), QStringLiteral("../../etc"), QStringLiteral("#ff6b8b")).isEmpty());
    QVERIFY(s->create(QStringLiteral("Nova"), QStringLiteral("cat"), QStringLiteral("red")).isEmpty());
    QVERIFY(s->create(QStringLiteral("Nova"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"), QStringLiteral("admin")).isEmpty());
    QCOMPARE(s->count(), 2);
    QCOMPARE(changed.count(), 1);

    // The limit.
    for (int i = s->count(); i < ProfileService::kMaxProfiles; ++i)
        QVERIFY(!s->create(QStringLiteral("P%1").arg(i), QStringLiteral("cat"), QStringLiteral("#ff6b8b")).isEmpty());
    QVERIFY(s->create(QStringLiteral("One too many"), QStringLiteral("cat"), QStringLiteral("#ff6b8b")).isEmpty());

    // Persisted.
    make();
    QCOMPARE(m_service->count(), ProfileService::kMaxProfiles);
    QCOMPARE(m_service->profile(id).value(QStringLiteral("name")).toString(), QStringLiteral("Comet"));
}

void TestProfiles::renameAndAppearance()
{
    ProfileService *s = make();
    const QString id = s->create(QStringLiteral("Comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    QVERIFY(s->rename(id, QStringLiteral("Pixel")));
    QVERIFY(!s->rename(id, QStringLiteral("player 1")));        // taken by the default profile
    QVERIFY(s->rename(id, QStringLiteral("PIXEL")));            // its own name, other case
    QVERIFY(!s->rename(QStringLiteral("nope"), QStringLiteral("X")));
    QVERIFY(!s->rename(id, QString()));
    QVERIFY(s->setAppearance(id, QStringLiteral("ghost"), QStringLiteral("#7bd88f")));
    QVERIFY(!s->setAppearance(id, QStringLiteral("dragon"), QStringLiteral("#7bd88f")));
    QVERIFY(s->setKind(id, QStringLiteral("kids")));
    QVERIFY(!s->setKind(id, QStringLiteral("root")));

    QSignalSpy active(s, &ProfileService::activeChanged);
    QVERIFY(s->rename(ProfileService::kDefaultId, QStringLiteral("Aaron")));
    QCOMPARE(active.count(), 1);                                // active profile's data changed

    make();
    const QVariantMap p = m_service->profile(id);
    QCOMPARE(p.value(QStringLiteral("name")).toString(), QStringLiteral("PIXEL"));
    QCOMPARE(p.value(QStringLiteral("avatar")).toString(), QStringLiteral("ghost"));
    QCOMPARE(p.value(QStringLiteral("color")).toString(), QStringLiteral("#7bd88f"));
    QCOMPARE(p.value(QStringLiteral("kind")).toString(), QStringLiteral("kids"));
}

void TestProfiles::deleteRemovesDataAndProtectsLast()
{
    ProfileService *s = make();
    const QString id = s->create(QStringLiteral("Comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    QVERIFY(QDir().mkpath(s->profileConfigDir(id)));
    QVERIFY(QDir().mkpath(s->profileStateDir(id)));
    QFile marker(s->profileConfigDir(id) + QStringLiteral("/favorites.json"));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.close();
    // A sibling outside the profile's dirs must survive.
    QVERIFY(QDir().mkpath(m_config + QStringLiteral("/profiles/keep-me")));

    QSignalSpy removed(s, &ProfileService::profileRemoved);
    QVERIFY(s->remove(id));
    QCOMPARE(removed.count(), 1);
    QCOMPARE(s->count(), 1);
    QVERIFY(!QFileInfo::exists(s->profileConfigDir(id)));
    QVERIFY(!QFileInfo::exists(s->profileStateDir(id)));
    QVERIFY(QFileInfo::exists(m_config + QStringLiteral("/profiles/keep-me")));

    // The last profile can't be deleted.
    QVERIFY(!s->remove(ProfileService::kDefaultId));
    QCOMPARE(s->count(), 1);
    QVERIFY(!s->remove(QStringLiteral("../config")));
    QVERIFY(!s->remove(QString()));
    make();
    QCOMPARE(m_service->count(), 1);
}

void TestProfiles::deleteActiveSwitches()
{
    ProfileService *s = make();
    const QString a = s->create(QStringLiteral("A"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    const QString b = s->create(QStringLiteral("B"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    QVERIFY(s->select(a));
    QVERIFY(s->select(b));
    QSignalSpy active(s, &ProfileService::activeChanged);
    QVERIFY(s->remove(b));
    QCOMPARE(active.count(), 1);
    QCOMPARE(s->activeId(), a);                                 // most recently used remaining
}

void TestProfiles::startupRule()
{
    ProfileService *s = make();
    QVERIFY(!s->pickerAtStartup());                             // 1 profile: straight to Home
    const QString id = s->create(QStringLiteral("Comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    QVERIFY(s->pickerAtStartup());                              // 2+: the picker
    QCOMPARE(make()->activeId(), ProfileService::kDefaultId);   // last used preselected
    QVERIFY(m_service->select(id));
    QCOMPARE(make()->activeId(), id);
    QVERIFY(m_service->pickerAtStartup());
    QVERIFY(m_service->remove(ProfileService::kDefaultId));
    QVERIFY(!make()->pickerAtStartup());
}

void TestProfiles::selectOrdersByLastUse()
{
    ProfileService *s = make();
    const QString a = s->create(QStringLiteral("A"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    const QString b = s->create(QStringLiteral("B"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    // Several selections within the same millisecond still order strictly.
    for (int i = 0; i < 5; ++i) {
        QVERIFY(s->select(a));
        QVERIFY(s->select(b));
    }
    QCOMPARE(make()->activeId(), b);
    QVERIFY(m_service->select(a));
    QCOMPARE(make()->activeId(), a);
    QVERIFY(!m_service->select(QStringLiteral("missing")));
    QCOMPARE(m_service->activeId(), a);
}

void TestProfiles::invalidIdsInFileAreDropped()
{
    QVERIFY(QDir().mkpath(m_config));
    QFile file(m_config + QStringLiteral("/profiles.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"json({"version":1,"profiles":[
        {"id":"../../etc","name":"Evil","avatar":"cat","color":"#ff6b8b","kind":"standard"},
        {"id":"a/b","name":"Slash"},
        {"id":"","name":"Empty"},
        {"id":"good_one","name":"Good","avatar":"dragon","color":"red","kind":"admin"},
        {"id":"good_one","name":"Duplicate"}
    ]})json");
    file.close();
    for (int i = 0; i < 4; ++i)
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("invalid or duplicate id")));
    ProfileService *s = make();
    QCOMPARE(s->count(), 1);
    QCOMPARE(s->activeId(), QStringLiteral("good_one"));
    const QVariantMap p = s->profile(QStringLiteral("good_one"));
    QCOMPARE(p.value(QStringLiteral("name")).toString(), QStringLiteral("Good"));
    QVERIFY(ProfileService::avatars().contains(p.value(QStringLiteral("avatar")).toString()));   // repaired
    QCOMPARE(p.value(QStringLiteral("kind")).toString(), QStringLiteral("standard"));
}

void TestProfiles::corruptFileIsMovedAside()
{
    QVERIFY(QDir().mkpath(m_config));
    QFile file(m_config + QStringLiteral("/profiles.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ not json");
    file.close();
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("profiles.json unusable")));
    ProfileService *s = make();
    QCOMPARE(s->count(), 1);
    QCOMPARE(s->activeId(), ProfileService::kDefaultId);
    QVERIFY(QFile::exists(m_config + QStringLiteral("/profiles.json.corrupt")));
    QCOMPARE(readFile().value(QStringLiteral("version")).toInt(), 1);
}

void TestProfiles::atomicWriteFailureLeavesStateUnchanged()
{
    ProfileService *s = make();
    const QByteArray before = [this] {
        QFile f(m_config + QStringLiteral("/profiles.json"));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    QVERIFY(!before.isEmpty());
    // Make the directory read-only: QSaveFile can't create its temp file,
    // so the commit fails and the old file is untouched.
    QFile::setPermissions(m_config, QFile::ReadOwner | QFile::ExeOwner);
    if (QFileInfo(m_config).isWritable())
        QSKIP("running with privileges that ignore directory permissions");
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("cannot write profiles.json")));
    QVERIFY(s->create(QStringLiteral("Comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b")).isEmpty());
    QCOMPARE(s->count(), 1);
    QFile::setPermissions(m_config, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile f(m_config + QStringLiteral("/profiles.json"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), before);
    // No temp files left behind.
    QCOMPARE(QDir(m_config).entryList(QDir::Files), QStringList { u"profiles.json"_s });
}

void TestProfiles::suggestedNamesAreUnused()
{
    ProfileService *s = make();
    const QStringList first = s->suggestedNames(4);
    QCOMPARE(first.size(), 4);
    QVERIFY(!s->create(first.at(0), QStringLiteral("cat"), QStringLiteral("#ff6b8b")).isEmpty());
    const QStringList next = s->suggestedNames(30);
    QCOMPARE(next.size(), 30);
    QVERIFY(!next.contains(first.at(0)));
    QVERIFY(!next.contains(QStringLiteral("Player 1")));
    for (const QString &name : next)
        QVERIFY(ProfileService::isValidName(name));
}

void TestProfiles::profileDirsRejectInvalidIds()
{
    ProfileService *s = make();
    for (const char *bad : { "", "..", "../x", "../config", "a/b", "a b", ".hidden", "x\ny" })
        QVERIFY2(s->profileConfigDir(QString::fromLatin1(bad)).isEmpty(), bad);
    QVERIFY(s->profileStateDir(QString(65, QLatin1Char('a'))).isEmpty());
    QVERIFY(!s->profileStateDir(QString(64, QLatin1Char('a'))).isEmpty());
}

void TestProfiles::updateIsOneAtomicWrite()
{
    ProfileService *s = make();
    const QString id = s->create(QStringLiteral("Comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    QSignalSpy changed(s, &ProfileService::profilesChanged);
    QVERIFY(s->update(id, QStringLiteral("Nova"), QStringLiteral("ghost"), QStringLiteral("#7bd88f"),
                      QStringLiteral("kids")));
    QCOMPARE(changed.count(), 1);                                   // one commit
    const QVariantMap p = s->profile(id);
    QCOMPARE(p.value(QStringLiteral("name")).toString(), QStringLiteral("Nova"));
    QCOMPARE(p.value(QStringLiteral("kind")).toString(), QStringLiteral("kids"));
    // One invalid field: nothing at all changes.
    QVERIFY(!s->update(id, QStringLiteral("Pixel"), QStringLiteral("ghost"), QStringLiteral("#7bd88f"),
                       QStringLiteral("root")));
    QVERIFY(!s->update(id, QStringLiteral("Player 1"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"),
                       QStringLiteral("standard")));                 // name taken
    QCOMPARE(changed.count(), 1);
    QCOMPARE(s->profile(id).value(QStringLiteral("name")).toString(), QStringLiteral("Nova"));
    QCOMPARE(make()->profile(id).value(QStringLiteral("avatar")).toString(), QStringLiteral("ghost"));
}

void TestProfiles::deleteDoesNotFollowSymlinks()
{
    ProfileService *s = make();
    const QString id = s->create(QStringLiteral("Comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    // The profile's config dir is a symlink to somewhere else.
    const QString outside = m_tmp->path() + QStringLiteral("/outside");
    QVERIFY(QDir().mkpath(outside));
    QFile keep(outside + QStringLiteral("/precious.txt"));
    QVERIFY(keep.open(QIODevice::WriteOnly));
    keep.close();
    QVERIFY(QDir().mkpath(m_config + QStringLiteral("/profiles")));
    QVERIFY(QFile::link(outside, s->profileConfigDir(id)));
    QVERIFY(s->remove(id));
    QVERIFY(!QFileInfo(s->profileConfigDir(id)).isSymLink());       // the link is gone...
    QVERIFY(QFile::exists(outside + QStringLiteral("/precious.txt")));   // ...its target is not
}

void TestProfiles::deleteRefusesSymlinkedTree()
{
    ProfileService *s = make();
    const QString id = s->create(QStringLiteral("Comet"), QStringLiteral("cat"), QStringLiteral("#ff6b8b"));
    // state/mightyos/profiles -> elsewhere: removing <profiles>/<id> would
    // delete outside our tree. Refuse the whole removal.
    const QString outside = m_tmp->path() + QStringLiteral("/elsewhere");
    QVERIFY(QDir().mkpath(outside + QStringLiteral("/") + id));
    QVERIFY(QDir().mkpath(m_state));
    QVERIFY(QFile::link(outside, m_state + QStringLiteral("/profiles")));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("is a symlink")));
    QVERIFY(!s->remove(id));
    QCOMPARE(s->count(), 2);
    QVERIFY(QFileInfo::exists(outside + QStringLiteral("/") + id));
}

QTEST_GUILESS_MAIN(TestProfiles)
#include "tst_profiles.moc"
