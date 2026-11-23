// Global settings (settings.json): defaults, rumble and tvRemote round trip,
// unknown keys kept, parse errors moved aside, newer versions read but never
// written, unreadable files left alone, failed writes.
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

class TestSettings : public QObject
{
    Q_OBJECT

private slots:
    void defaults();
    void rumbleToggleRoundTrip();
    void tvRemoteRoundTrip();
    void unknownKeysAreKept();
    void parseErrorIsMovedAside();
    void newerVersionIsReadOnly();
    void unreadableFileIsLeftAlone();
    void failedWriteKeepsValue();
};

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(data);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QJsonObject readJson(const QString &path)
{
    return QJsonDocument::fromJson(readFile(path)).object();
}

} // namespace

void TestSettings::defaults()
{
    QTemporaryDir dir;
    Settings settings(dir.path());
    QVERIFY(settings.rumble());                     // default on
    QCOMPARE(settings.tvRemote(), QStringLiteral("navigation"));
    QVERIFY(settings.isWritable());
    QVERIFY(!QFile::exists(settings.filePath()));   // nothing written yet
}

void TestSettings::rumbleToggleRoundTrip()
{
    QTemporaryDir dir;
    {
        Settings settings(dir.path());
        QSignalSpy changed(&settings, &Settings::rumbleChanged);
        settings.setRumble(false);
        QCOMPARE(changed.count(), 1);
        settings.setRumble(false);
        QCOMPARE(changed.count(), 1);
        const QJsonObject json = readJson(settings.filePath());
        QCOMPARE(json.value(QStringLiteral("version")).toInt(), Settings::kVersion);
        QCOMPARE(json.value(QStringLiteral("rumble")).toBool(true), false);
    }
    Settings again(dir.path());
    QVERIFY(!again.rumble());
    again.setRumble(true);
    Settings third(dir.path());
    QVERIFY(third.rumble());
}

void TestSettings::tvRemoteRoundTrip()
{
    QTemporaryDir dir;
    {
        Settings settings(dir.path());
        QSignalSpy changed(&settings, &Settings::tvRemoteChanged);
        settings.setTvRemote(QStringLiteral("full"));
        settings.setTvRemote(QStringLiteral("everything"));   // invalid: ignored
        QCOMPARE(settings.tvRemote(), QStringLiteral("full"));
        QCOMPARE(changed.count(), 1);
        settings.setTvRemote(QStringLiteral("off"));
    }
    Settings again(dir.path());
    QCOMPARE(again.tvRemote(), QStringLiteral("off"));
    writeFile(again.filePath(), R"({"version":1,"tvRemote":"admin"})");
    again.reload();
    QCOMPARE(again.tvRemote(), QStringLiteral("navigation"));   // unknown value: default
}

void TestSettings::unknownKeysAreKept()
{
    QTemporaryDir dir;
    writeFile(dir.filePath(QStringLiteral("settings.json")),
              R"({"version":1,"rumble":true,"futureKey":{"a":1}})");
    Settings settings(dir.path());
    settings.setRumble(false);
    const QJsonObject json = readJson(settings.filePath());
    QCOMPARE(json.value(QStringLiteral("futureKey")).toObject().value(QStringLiteral("a")).toInt(), 1);
    QCOMPARE(json.value(QStringLiteral("rumble")).toBool(true), false);
    // A wrongly typed value means the default.
    writeFile(settings.filePath(), R"({"version":1,"rumble":"no"})");
    settings.reload();
    QVERIFY(settings.rumble());
}

void TestSettings::parseErrorIsMovedAside()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    for (const QByteArray &bad : { QByteArray("{ not json"), QByteArray("[1,2]"), QByteArray(R"({"rumble":false})") }) {
        writeFile(path, bad);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("kept as \".corrupt\"")));
        Settings settings(dir.path());
        QVERIFY(settings.rumble());
        QVERIFY(settings.isWritable());
        QCOMPARE(readFile(path + QStringLiteral(".corrupt")), bad);
        QVERIFY(!QFile::exists(path));
    }
}

void TestSettings::newerVersionIsReadOnly()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QByteArray newer = R"({"version":2,"rumble":false,"tvRemote":"off","newThing":true})";
    writeFile(path, newer);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("newer MightyOS")));
    Settings settings(dir.path());
    // Known keys are read ...
    QVERIFY(!settings.rumble());
    QCOMPARE(settings.tvRemote(), QStringLiteral("off"));
    QVERIFY(!settings.isWritable());
    // ... the file is neither moved nor rewritten.
    QSignalSpy failed(&settings, &Settings::writeFailed);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("read-only")));
    settings.setRumble(true);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!settings.rumble());
    QCOMPARE(readFile(path), newer);
    QVERIFY(!QFile::exists(path + QStringLiteral(".unsupported")));
    QVERIFY(!QFile::exists(path + QStringLiteral(".corrupt")));
}

void TestSettings::unreadableFileIsLeftAlone()
{
    if (::geteuid() == 0)
        QSKIP("root can read anything");
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    writeFile(path, R"({"version":1,"rumble":false})");
    QCOMPARE(::chmod(QFile::encodeName(path).constData(), 0000), 0);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("cannot read settings.json")));
    Settings settings(dir.path());
    QVERIFY(settings.rumble());                  // defaults
    QVERIFY(!settings.isWritable());             // and not overwritten
    QVERIFY(QFile::exists(path));
    QVERIFY(!QFile::exists(path + QStringLiteral(".corrupt")));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("read-only")));
    settings.setRumble(false);
    ::chmod(QFile::encodeName(path).constData(), 0600);
    QCOMPARE(readJson(path).value(QStringLiteral("rumble")).toBool(true), false);   // untouched
}

void TestSettings::failedWriteKeepsValue()
{
    if (::geteuid() == 0)
        QSKIP("root can write anywhere");
    QTemporaryDir dir;
    const QString config = dir.filePath(QStringLiteral("config"));
    QVERIFY(QDir().mkdir(config));
    QCOMPARE(::chmod(QFile::encodeName(config).constData(), 0500), 0);   // no writes
    Settings settings(config);
    QVERIFY(settings.isWritable());   // nothing wrong with reading
    QTest::ignoreMessage(QtWarningMsg, "could not write settings.json");
    QSignalSpy failed(&settings, &Settings::writeFailed);
    QSignalSpy changed(&settings, &Settings::rumbleChanged);
    settings.setRumble(false);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(changed.count(), 0);
    QVERIFY(settings.rumble());
    ::chmod(QFile::encodeName(config).constData(), 0700);
}

QTEST_GUILESS_MAIN(TestSettings)
#include "tst_settings.moc"
