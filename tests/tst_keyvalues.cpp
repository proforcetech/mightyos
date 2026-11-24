#include "keyvalues.h"

#include <QTest>

// Valve KeyValues text parser (libraryfolders.vdf, appmanifest_*.acf):
// untrusted input, so malformed files and every limit are tested.
class TestKeyValues : public QObject
{
    Q_OBJECT

private slots:
    void steamManifest();
    void libraryFolders();
    void escapesCommentsConditionals();
    void bareTokensAndBom();
    void duplicatesAndCase();
    void malformed_data();
    void malformed();
    void limits();
    void deepInputIsNotRecursive();
};

void TestKeyValues::steamManifest()
{
    const QByteArray acf = R"("AppState"
{
	"appid"		"620"
	"Universe"		"1"
	"name"		"Portal 2"
	"StateFlags"		"4"
	"installdir"		"Portal 2"
	"InstalledDepots"
	{
		"621"
		{
			"manifest"		"123"
		}
	}
}
)";
    QString error;
    const auto doc = KeyValues::parse(acf, {}, &error);
    QVERIFY2(doc, qPrintable(error));
    const KeyValues::Node *state = doc->child(u"AppState");
    QVERIFY(state && state->isBlock);
    QCOMPARE(state->valueOf(u"appid"), QStringLiteral("620"));
    QCOMPARE(state->valueOf(u"name"), QStringLiteral("Portal 2"));
    QCOMPARE(state->valueOf(u"missing", QStringLiteral("x")), QStringLiteral("x"));
    const KeyValues::Node *depot = doc->path({ u"AppState", u"InstalledDepots", u"621" });
    QVERIFY(depot);
    QCOMPARE(depot->valueOf(u"manifest"), QStringLiteral("123"));
    QVERIFY(!doc->path({ u"AppState", u"name" }));   // a leaf is not a block
}

void TestKeyValues::libraryFolders()
{
    const QByteArray vdf = R"("libraryfolders"
{
	"contentstatsid"		"-123"
	"0"
	{
		"path"		"/home/u/.local/share/Steam"
		"apps" { "620" "1234" }
	}
	"1"
	{
		"path"		"D:\\Games\\Steam Library"
	}
})";
    const auto doc = KeyValues::parse(vdf);
    QVERIFY(doc);
    const KeyValues::Node *folders = doc->child(u"libraryfolders");
    QVERIFY(folders);
    QCOMPARE(folders->children.size(), 3);
    QCOMPARE(folders->children.at(1).valueOf(u"path"), QStringLiteral("/home/u/.local/share/Steam"));
    QCOMPARE(folders->children.at(2).valueOf(u"path"), QStringLiteral("D:\\Games\\Steam Library"));
}

void TestKeyValues::escapesCommentsConditionals()
{
    const QByteArray text = "// header comment\n"
                            "\"root\" { \"a\" \"line\\nnext \\\"q\\\" \\\\ \\x\" // trailing\n"
                            "  \"win\" \"1\" [$WIN32]\n"
                            "  \"blk\" [$LINUX] { \"k\" \"v\" } [$X360]\n"
                            "}";
    QString error;
    const auto doc = KeyValues::parse(text, {}, &error);
    QVERIFY2(doc, qPrintable(error));
    const KeyValues::Node *root = doc->child(u"root");
    QVERIFY(root);
    QCOMPARE(root->valueOf(u"a"), QStringLiteral("line\nnext \"q\" \\ \\x"));
    QCOMPARE(root->valueOf(u"win"), QStringLiteral("1"));
    QVERIFY(root->child(u"blk") && root->child(u"blk")->isBlock);
    QCOMPARE(root->children.size(), 3);
}

void TestKeyValues::bareTokensAndBom()
{
    const auto doc = KeyValues::parse("\xEF\xBB\xBFroot { key value other \"x y\" }");
    QVERIFY(doc);
    QCOMPARE(doc->child(u"root")->valueOf(u"key"), QStringLiteral("value"));
    QCOMPARE(doc->child(u"root")->valueOf(u"other"), QStringLiteral("x y"));
    // UTF-8 names survive.
    const auto utf = KeyValues::parse("\"n\" \"Ōkami\"");
    QVERIFY(utf);
    QCOMPARE(utf->valueOf(u"n"), QStringLiteral("Ōkami"));
}

void TestKeyValues::duplicatesAndCase()
{
    const auto doc = KeyValues::parse("\"R\" { \"Key\" \"1\" \"key\" \"2\" }");
    QVERIFY(doc);
    QCOMPARE(doc->child(u"r")->valueOf(u"KEY"), QStringLiteral("1"));   // first, case-insensitive
    QCOMPARE(doc->child(u"r")->children.size(), 2);
}

void TestKeyValues::malformed_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::newRow("unterminated string") << QByteArray("\"a\" \"b");
    QTest::newRow("unbalanced close") << QByteArray("\"a\" \"b\" }");
    QTest::newRow("missing close") << QByteArray("\"a\" { \"b\" \"c\"");
    QTest::newRow("brace without key") << QByteArray("{ \"a\" \"b\" }");
    QTest::newRow("key without value") << QByteArray("\"a\"");
    QTest::newRow("key then close") << QByteArray("\"r\" { \"a\" }");
    QTest::newRow("nul in string") << QByteArray("\"a\" \"b\0c\"", 9);
    QTest::newRow("nul in token") << QByteArray("a b\0c", 5);
}

void TestKeyValues::malformed()
{
    QFETCH(QByteArray, text);
    QString error;
    QVERIFY(!KeyValues::parse(text, {}, &error));
    QVERIFY(!error.isEmpty());
}

void TestKeyValues::limits()
{
    KeyValues::Limits limits;
    limits.maxBytes = 64;
    QVERIFY(!KeyValues::parse(QByteArray(65, ' '), limits));
    QVERIFY(KeyValues::parse(QByteArray(64, ' '), limits));

    limits = {};
    limits.maxTokenLength = 8;
    QVERIFY(!KeyValues::parse("\"a\" \"123456789\"", limits));
    QVERIFY(!KeyValues::parse("a 123456789", limits));
    QVERIFY(KeyValues::parse("\"a\" \"12345678\"", limits));

    limits = {};
    limits.maxNodes = 3;
    QVERIFY(KeyValues::parse("a 1 b 2 c 3", limits));
    QVERIFY(!KeyValues::parse("a 1 b 2 c 3 d 4", limits));

    limits = {};
    limits.maxDepth = 2;
    QVERIFY(KeyValues::parse("a { b { c 1 } }", limits));
    QVERIFY(!KeyValues::parse("a { b { c { d 1 } } }", limits));
}

void TestKeyValues::deepInputIsNotRecursive()
{
    // 100k nested blocks: rejected by the depth limit, without recursion.
    QByteArray deep;
    for (int i = 0; i < 100000; ++i)
        deep += "k{";
    QString error;
    QVERIFY(!KeyValues::parse(deep, {}, &error));
    QVERIFY(error.contains(QLatin1String("deep")));
}

QTEST_GUILESS_MAIN(TestKeyValues)
#include "tst_keyvalues.moc"
