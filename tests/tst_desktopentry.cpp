#include "desktopentry.h"

#include <QTest>

// Desktop Entry Specification parsing, in particular Exec= -> argv without
// a shell (quoting, escaping, field codes, rejection of shell syntax).
using namespace Qt::StringLiterals;

class TestDesktopEntry : public QObject
{
    Q_OBJECT

private slots:
    void execValid_data();
    void execValid();
    void execRejected_data();
    void execRejected();
    void execErrorNeverContainsCommand();
    void parseBasics();
    void parseLocaleAndLists();
    void parseRejects_data();
    void parseRejects();
};

using DesktopEntry::ExecContext;

static const ExecContext kContext { QStringLiteral("My App"), QStringLiteral("my-icon"),
                                    QStringLiteral("/usr/share/applications/my.desktop") };

void TestDesktopEntry::execValid_data()
{
    // `raw` is the value exactly as written in the .desktop file.
    QTest::addColumn<QString>("raw");
    QTest::addColumn<QStringList>("argv");

    QTest::newRow("plain") << u"foo"_s << QStringList { u"foo"_s };
    QTest::newRow("args, extra spaces") << u"foo  bar   baz"_s << QStringList { u"foo"_s, u"bar"_s, u"baz"_s };
    QTest::newRow("quoted program with space") << u"\"/opt/My App/app\" --x"_s
                                                << QStringList { u"/opt/My App/app"_s, u"--x"_s };
    QTest::newRow("quoted part inside an argument") << u"foo --name=\"a b\""_s << QStringList { u"foo"_s, u"--name=a b"_s };
    QTest::newRow("empty quoted argument") << u"foo \"\""_s << QStringList { u"foo"_s, u""_s };
    // Inside quotes: \" \` \$ \\ escape. In the file each backslash is
    // itself written as \\ (string escapes are applied first).
    QTest::newRow("escaped quote") << u"foo \"a \\\\\"b\\\\\" c\""_s << QStringList { u"foo"_s, u"a \"b\" c"_s };
    QTest::newRow("escaped dollar and backtick") << u"foo \"\\\\$HOME \\\\`x\\\\`\""_s
                                                  << QStringList { u"foo"_s, u"$HOME `x`"_s };
    QTest::newRow("literal backslash") << u"foo \"a\\\\\\\\b\""_s << QStringList { u"foo"_s, u"a\\b"_s };
    QTest::newRow("reserved chars are fine when quoted") << "sh -c \"echo hi | tr a-z A-Z > /dev/null; true\""
                                                          << QStringList { u"sh"_s, u"-c"_s, u"echo hi | tr a-z A-Z > /dev/null; true"_s };
    QTest::newRow("\\s string escape separates") << u"foo a\\sb"_s << QStringList { u"foo"_s, u"a"_s, u"b"_s };
    QTest::newRow("env assignment as an argument") << u"env TERM=xterm byobu"_s
                                                    << QStringList { u"env"_s, u"TERM=xterm"_s, u"byobu"_s };

    // Field codes: files/URLs are never passed, so they disappear.
    QTest::newRow("%f") << u"foo %f"_s << QStringList { u"foo"_s };
    QTest::newRow("%F") << u"foo %F"_s << QStringList { u"foo"_s };
    QTest::newRow("%u") << u"foo %u --x"_s << QStringList { u"foo"_s, u"--x"_s };
    QTest::newRow("%U") << u"foo %U"_s << QStringList { u"foo"_s };
    QTest::newRow("%f inside an argument") << u"foo --file=%f"_s << QStringList { u"foo"_s, u"--file="_s };
    QTest::newRow("%i") << u"foo %i"_s << QStringList { u"foo"_s, u"--icon"_s, u"my-icon"_s };
    QTest::newRow("%c") << u"foo --title=%c"_s << QStringList { u"foo"_s, u"--title=My App"_s };
    QTest::newRow("%c alone") << u"foo %c"_s << QStringList { u"foo"_s, u"My App"_s };
    QTest::newRow("%k") << u"foo %k"_s << QStringList { u"foo"_s, u"/usr/share/applications/my.desktop"_s };
    QTest::newRow("%%") << u"foo 100%%"_s << QStringList { u"foo"_s, u"100%"_s };
    QTest::newRow("%% in quotes") << u"foo \"50%% off\""_s << QStringList { u"foo"_s, u"50% off"_s };
    QTest::newRow("deprecated %d %n %m dropped") << u"foo %d %n %m bar"_s << QStringList { u"foo"_s, u"bar"_s };
    QTest::newRow("flatpak export") << u"/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=kodi "
                                       "--file-forwarding tv.kodi.Kodi @@u %U @@"_s
                                    << QStringList { u"/usr/bin/flatpak"_s, u"run"_s, u"--branch=stable"_s, u"--arch=x86_64"_s,
                                                     u"--command=kodi"_s, u"--file-forwarding"_s, u"tv.kodi.Kodi"_s, u"@@u"_s, u"@@"_s };
}

void TestDesktopEntry::execValid()
{
    QFETCH(QString, raw);
    QFETCH(QStringList, argv);
    const DesktopEntry::ExecResult result = DesktopEntry::parseExec(raw, kContext);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.argv, argv);
}

void TestDesktopEntry::execRejected_data()
{
    QTest::addColumn<QString>("raw");

    // Would need a shell to mean what it says.
    QTest::newRow("pipe") << u"foo | bar"_s;
    QTest::newRow("redirect") << u"foo > /tmp/x"_s;
    QTest::newRow("input redirect") << u"foo < /etc/passwd"_s;
    QTest::newRow("variable") << u"foo $HOME"_s;
    QTest::newRow("command substitution") << u"foo `id`"_s;
    QTest::newRow("semicolon") << "foo; rm -rf ~";
    QTest::newRow("ampersand") << u"foo && bar"_s;
    QTest::newRow("glob") << u"foo *.txt"_s;
    QTest::newRow("question mark") << u"foo ?"_s;
    QTest::newRow("tilde") << u"foo ~/file"_s;
    QTest::newRow("single quotes") << u"foo 'a b'"_s;
    QTest::newRow("hash") << u"foo #comment"_s;
    QTest::newRow("parentheses") << u"foo (bar)"_s;
    QTest::newRow("unquoted backslash") << u"foo a\\\\b"_s;
    QTest::newRow("tab via escape") << u"foo\\tbar"_s;
    QTest::newRow("unescaped $ in quotes") << u"foo \"$HOME\""_s;
    QTest::newRow("unescaped ` in quotes") << u"foo \"`id`\""_s;
    QTest::newRow("env assignment as program") << u"FOO=1 foo"_s;

    // Malformed.
    QTest::newRow("empty") << u""_s;
    QTest::newRow("only spaces") << u"   "_s;
    QTest::newRow("unterminated quote") << u"foo \"bar"_s;
    QTest::newRow("invalid escape in quotes") << u"foo \"\\\\q\""_s;
    QTest::newRow("dangling %") << u"foo %"_s;
    QTest::newRow("unknown field code") << u"foo %z"_s;
    QTest::newRow("two file codes") << u"foo %f %u"_s;
    QTest::newRow("%F not alone") << u"foo --files=%F"_s;
    QTest::newRow("%U in quotes") << u"foo \"%U\""_s;
    QTest::newRow("%f in quotes") << u"foo \"%f\""_s;
    QTest::newRow("%c in quotes") << u"foo \"--title=%c\""_s;
    QTest::newRow("%i in quotes") << u"foo \"%i\""_s;
    QTest::newRow("tab in quotes") << u"foo \"a\\tb\""_s;
    QTest::newRow("newline in quotes") << u"foo \"a\\nb\""_s;
    QTest::newRow("NUL in quotes") << (u"foo \"a"_s + QChar(0) + u"b\""_s);
    QTest::newRow("NUL unquoted") << (u"foo a"_s + QChar(0) + u"b"_s);
    QTest::newRow("escape char unquoted") << (u"foo a"_s + QChar(0x1b) + u"b"_s);
    QTest::newRow("%i not alone") << u"foo x%i"_s;
    QTest::newRow("only a field code") << u"%f"_s;
    QTest::newRow("relative program path") << u"bin/foo"_s;
    QTest::newRow("dot-relative program") << u"./foo"_s;
    QTest::newRow("too long") << QString(DesktopEntry::kMaxExecLength + 1, QLatin1Char('a'));
}

void TestDesktopEntry::execRejected()
{
    QFETCH(QString, raw);
    const DesktopEntry::ExecResult result = DesktopEntry::parseExec(raw, kContext);
    QVERIFY2(!result.ok, qPrintable(result.argv.join(QLatin1Char('|'))));
    QVERIFY(result.argv.isEmpty());
    QVERIFY(!result.error.isEmpty());
}

void TestDesktopEntry::execErrorNeverContainsCommand()
{
    // Errors are logged; they must not leak the command line.
    const DesktopEntry::ExecResult result = DesktopEntry::parseExec(QStringLiteral("secret-tool --token=abc | x"), kContext);
    QVERIFY(!result.ok);
    QVERIFY(!result.error.contains(QStringLiteral("secret")));
    QVERIFY(!result.error.contains(QStringLiteral("abc")));
}

void TestDesktopEntry::parseBasics()
{
    const QByteArray data =
        "# comment\n"
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name = Spaced Name\n"
        "Exec=foo %U\r\n"
        "Comment=Line\\none\n"
        "NoDisplay=true\n"
        "Name=Duplicate ignored\n"
        "\n"
        "[Desktop Action new]\n"
        "Exec=evil | thing\n";
    const auto entry = DesktopEntry::parse(data);
    QVERIFY(entry);
    QCOMPARE(entry->string(QStringLiteral("Type")), QStringLiteral("Application"));
    QCOMPARE(entry->string(QStringLiteral("Name")), QStringLiteral("Spaced Name"));
    QCOMPARE(entry->values.value(QStringLiteral("Exec")), QStringLiteral("foo %U"));   // \r stripped, action ignored
    QCOMPARE(entry->string(QStringLiteral("Comment")), QStringLiteral("Line\none"));
    QVERIFY(entry->boolean(QStringLiteral("NoDisplay")));
    QVERIFY(!entry->boolean(QStringLiteral("Hidden")));
}

void TestDesktopEntry::parseLocaleAndLists()
{
    const QByteArray data =
        "[Desktop Entry]\n"
        "Name=Plain\n"
        "Name[de]=Deutsch\n"
        "Name[de_AT]=Oesterreich\n"
        "Name[sr@latin]=Latinica\n"
        "Categories=AudioVideo;Video;;Player\\;X;\n";
    const auto entry = DesktopEntry::parse(data);
    QVERIFY(entry);
    QCOMPARE(entry->localeString(QStringLiteral("Name"), QStringLiteral("de_AT.UTF-8")), QStringLiteral("Oesterreich"));
    QCOMPARE(entry->localeString(QStringLiteral("Name"), QStringLiteral("de_DE")), QStringLiteral("Deutsch"));
    QCOMPARE(entry->localeString(QStringLiteral("Name"), QStringLiteral("sr_RS@latin")), QStringLiteral("Latinica"));
    QCOMPARE(entry->localeString(QStringLiteral("Name"), QStringLiteral("fr_FR")), QStringLiteral("Plain"));
    QCOMPARE(entry->localeString(QStringLiteral("Name"), QStringLiteral("C")), QStringLiteral("Plain"));
    QCOMPARE(entry->list(QStringLiteral("Categories")),
             (QStringList { u"AudioVideo"_s, u"Video"_s, u"Player;X"_s }));
}

void TestDesktopEntry::parseRejects_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::newRow("no group") << QByteArray("Name=x\n");
    QTest::newRow("wrong first group") << QByteArray("[Other]\nName=x\n[Desktop Entry]\nName=y\n");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("malformed line") << QByteArray("[Desktop Entry]\nthis is not a key\n");
    QTest::newRow("bad key") << QByteArray("[Desktop Entry]\nNa me=x\n");
    QTest::newRow("invalid utf-8") << QByteArray("[Desktop Entry]\nName=\xff\xfe\n");
    QTest::newRow("too large") << (QByteArray("[Desktop Entry]\n") + QByteArray(DesktopEntry::kMaxFileBytes, '#'));
    QTest::newRow("duplicate group") << QByteArray("[Desktop Entry]\nName=a\n[Desktop Entry]\nName=b\n");
}

void TestDesktopEntry::parseRejects()
{
    QFETCH(QByteArray, data);
    QString error;
    QVERIFY(!DesktopEntry::parse(data, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(TestDesktopEntry)
#include "tst_desktopentry.moc"
