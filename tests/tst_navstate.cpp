#include "envguard.h"
#include "navstate.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

class TestNavState : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void defaultDirectoryFollowsXdg();
    void roundTrip();
    void missingFileIsEmpty();
    void corruptFilesAreIgnored_data();
    void corruptFilesAreIgnored();
    void malformedEntriesTruncatePath();
    void pathLengthIsCapped();
    void profilePaths();
    void saveLaterDebouncesAndFlushes();
    void discardDropsPendingSave();

private:
    static QVariantMap entry(const QString &screen, const QVariantMap &params, int row, int index,
                             const QString &key = QString());
    void writeRaw(const QByteArray &data);

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<NavState> m_state;
};

QVariantMap TestNavState::entry(const QString &screen, const QVariantMap &params, int row, int index,
                                const QString &key)
{
    return {
        { QStringLiteral("screen"), screen }, { QStringLiteral("params"), params },
        { QStringLiteral("row"), row }, { QStringLiteral("index"), index },
        { QStringLiteral("key"), key }, { QStringLiteral("extra"), QVariantMap() },
    };
}

void TestNavState::writeRaw(const QByteArray &data)
{
    QFile file(m_state->filePath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(data);
}

void TestNavState::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_state = std::make_unique<NavState>();
    m_state->setDirectory(m_dir->path());
}

void TestNavState::defaultDirectoryFollowsXdg()
{
    {
        const EnvGuard xdg("XDG_STATE_HOME", m_dir->path().toUtf8());
        QCOMPARE(NavState::defaultDirectory(), m_dir->path() + QStringLiteral("/mightyos"));
        NavState fresh;
        QCOMPARE(fresh.filePath(), m_dir->path() + QStringLiteral("/mightyos/navstate.json"));
    }
    {
        const EnvGuard home("HOME", m_dir->path().toUtf8());
        const EnvGuard xdg("XDG_STATE_HOME", QByteArray());
        QCOMPARE(NavState::defaultDirectory(), m_dir->path() + QStringLiteral("/.local/state/mightyos"));
    }
}

void TestNavState::roundTrip()
{
    const QVariantList path {
        entry(QStringLiteral("home"), {}, 1, 1, QStringLiteral("snes")),
        entry(QStringLiteral("systems"), { { QStringLiteral("category"), QStringLiteral("emulators") } }, 0, 1,
              QStringLiteral("snes")),
        entry(QStringLiteral("games"), { { QStringLiteral("system"), QStringLiteral("snes") } }, 0, 23,
              QStringLiteral("rom:snes:0123abcd")),
    };
    QVERIFY(m_state->save(path));
    QVERIFY(QFile::exists(m_state->filePath()));
    QCOMPARE(m_state->load(), path);

    // On disk: versioned object.
    QFile file(m_state->filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), NavState::kVersion);
    QCOMPARE(root.value(QStringLiteral("path")).toArray().size(), 3);
}

void TestNavState::missingFileIsEmpty()
{
    QVERIFY(m_state->load().isEmpty());
    NavState nowhere;
    nowhere.setDirectory(m_dir->path() + QStringLiteral("/does/not/exist"));
    QVERIFY(nowhere.load().isEmpty());
}

void TestNavState::corruptFilesAreIgnored_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("garbage") << QByteArray("\x00\xff not json");
    QTest::newRow("truncated") << QByteArray(R"({"version":1,"path":[{"screen":"home")");
    QTest::newRow("array-root") << QByteArray(R"([{"screen":"home"}])");
    QTest::newRow("no-version") << QByteArray(R"({"path":[{"screen":"home"}]})");
    QTest::newRow("future-version") << QByteArray(R"({"version":99,"path":[{"screen":"home"}]})");
    QTest::newRow("path-not-array") << QByteArray(R"({"version":1,"path":{"screen":"home"}})");
    QByteArray huge = R"({"version":1,"path":[{"screen":"home","key":")";
    huge += QByteArray(NavState::kMaxFileBytes, 'x');
    huge += R"("}]})";
    QTest::newRow("oversized") << huge;
}

void TestNavState::corruptFilesAreIgnored()
{
    QFETCH(QByteArray, data);
    writeRaw(data);
    QVERIFY(m_state->load().isEmpty());
}

void TestNavState::malformedEntriesTruncatePath()
{
    // Everything after the first bad entry depends on it, so it all goes.
    const QByteArray data = R"({"version":1,"path":[
        {"screen":"home","row":1,"index":2,"key":"snes","extra":{"category":"emulators"}},
        {"screen":"systems","params":{"category":"emulators"},"row":0,"index":1},
        {"screen":"games","params":"snes","index":3},
        {"screen":"games","params":{"system":"snes"},"index":3}
    ]})";
    writeRaw(data);
    QVariantList path = m_state->load();
    QCOMPARE(path.size(), 2);
    QCOMPARE(path.at(0).toMap().value(QStringLiteral("extra")).toMap().value(QStringLiteral("category")).toString(),
             QStringLiteral("emulators"));
    // Missing fields get defaults.
    QCOMPARE(path.at(1).toMap().value(QStringLiteral("key")).toString(), QString());

    const QList<QByteArray> bad {
        R"({"screen":"../etc"})",
        R"({"screen":"Home"})",
        R"({"screen":""})",
        R"({"screen":5})",
        R"({"screen":"games","index":-1})",
        R"({"screen":"games","index":1.5})",
        R"({"screen":"games","row":"0"})",
        R"({"screen":"games","key":7})",
        R"({"screen":"games","extra":[1]})",
        R"("home")",
    };
    for (const QByteArray &entryJson : bad) {
        writeRaw(R"({"version":1,"path":[{"screen":"home"},)" + entryJson + "]}");
        path = m_state->load();
        QVERIFY2(path.size() == 1, entryJson.constData());
    }
}

void TestNavState::pathLengthIsCapped()
{
    QVariantList path;
    for (int i = 0; i < NavState::kMaxEntries + 5; ++i)
        path.append(entry(QStringLiteral("games"), {}, 0, i));
    QVERIFY(m_state->save(path));
    QCOMPARE(m_state->load().size(), NavState::kMaxEntries);
}

void TestNavState::profilePaths()
{
    QCOMPARE(m_state->filePath(QStringLiteral("kid_1")),
             m_dir->path() + QStringLiteral("/profiles/kid_1/navstate.json"));
    for (const QString &bad : { QStringLiteral("../x"), QStringLiteral("a/b"), QStringLiteral("."),
                                QStringLiteral(".."), QStringLiteral("a b"), QString(65, QLatin1Char('a')) }) {
        QVERIFY2(!NavState::isValidProfileId(bad), qPrintable(bad));
        QVERIFY(m_state->filePath(bad).isEmpty());
        QVERIFY(!m_state->save({ entry(QStringLiteral("home"), {}, 0, 0) }, bad));
        QVERIFY(m_state->load(bad).isEmpty());
    }

    // Profiles are independent of each other and of the default file.
    QVERIFY(m_state->save({ entry(QStringLiteral("home"), {}, 0, 1) }));
    QVERIFY(m_state->save({ entry(QStringLiteral("home"), {}, 0, 4) }, QStringLiteral("kid_1")));
    QCOMPARE(m_state->load().first().toMap().value(QStringLiteral("index")).toInt(), 1);
    QCOMPARE(m_state->load(QStringLiteral("kid_1")).first().toMap().value(QStringLiteral("index")).toInt(), 4);
    QVERIFY(m_state->load(QStringLiteral("other")).isEmpty());
}

void TestNavState::saveLaterDebouncesAndFlushes()
{
    m_state->saveLater({ entry(QStringLiteral("home"), {}, 0, 1) });
    m_state->saveLater({ entry(QStringLiteral("home"), {}, 0, 2) });
    QVERIFY(!QFile::exists(m_state->filePath()));     // nothing written yet

    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(m_state->filePath()), 5 * NavState::kSaveDelayMs);
    QCOMPARE(m_state->load().first().toMap().value(QStringLiteral("index")).toInt(), 2);

    // flush() writes immediately; a later flush is a no-op.
    m_state->saveLater({ entry(QStringLiteral("home"), {}, 0, 3) });
    m_state->flush();
    QCOMPARE(m_state->load().first().toMap().value(QStringLiteral("index")).toInt(), 3);

    // A pending save for one profile is written before another's is queued.
    m_state->saveLater({ entry(QStringLiteral("home"), {}, 0, 5) }, QStringLiteral("a"));
    m_state->saveLater({ entry(QStringLiteral("home"), {}, 0, 6) }, QStringLiteral("b"));
    QCOMPARE(m_state->load(QStringLiteral("a")).first().toMap().value(QStringLiteral("index")).toInt(), 5);

    // Destruction flushes too.
    m_state.reset();
    NavState reader;
    reader.setDirectory(m_dir->path());
    QCOMPARE(reader.load(QStringLiteral("b")).first().toMap().value(QStringLiteral("index")).toInt(), 6);
}

void TestNavState::discardDropsPendingSave()
{
    const QVariantList path { entry(QStringLiteral("home"), {}, 0, 2, QStringLiteral("games")) };
    m_state->saveLater(path, QStringLiteral("gone"));
    m_state->discard(QStringLiteral("other"));      // someone else's: no effect
    m_state->discard(QStringLiteral("gone"));
    m_state->flush();
    QVERIFY(!QFileInfo::exists(m_dir->path() + QStringLiteral("/profiles/gone")));
    m_state->saveLater(path, QStringLiteral("kept"));
    m_state->discard(QStringLiteral("gone"));
    m_state->flush();
    QCOMPARE(m_state->load(QStringLiteral("kept")).size(), 1);
}

QTEST_GUILESS_MAIN(TestNavState)
#include "tst_navstate.moc"
