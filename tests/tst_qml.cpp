// Qt Quick Test runner for the QML tests (tests/qml/tst_*.qml).
// The MightyOS.Core and MightyOS.UI modules are static libraries, linked
// in and imported here exactly as in the app.
#include "artworkprovider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQml/qqmlextensionplugin.h>
#include <QtQuickTest/quicktest.h>

Q_IMPORT_QML_PLUGIN(MightyOS_CorePlugin)
Q_IMPORT_QML_PLUGIN(MightyOS_UIPlugin)

// Lets QML tests drop .desktop files and manifests where AppRegistry
// looks. Confined to the test XDG_DATA_HOME: relative, clean paths only.
class TestFiles : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    Q_INVOKABLE bool write(const QString &relativePath, const QString &content)
    {
        const QString base = qEnvironmentVariable("XDG_DATA_HOME");
        if (base.isEmpty() || relativePath.isEmpty() || QDir::isAbsolutePath(relativePath)
            || QDir::cleanPath(relativePath) != relativePath || relativePath.startsWith(QLatin1String("..")))
            return false;
        const QString path = base + QLatin1Char('/') + relativePath;
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        return file.write(content.toUtf8()) >= 0;
    }
    // `area` "config" | "state" | "data" | "cache": does <XDG dir>/<relativePath> exist?
    Q_INVOKABLE bool exists(const QString &area, const QString &relativePath) const
    {
        static const QHash<QString, const char *> vars {
            { QStringLiteral("config"), "XDG_CONFIG_HOME" }, { QStringLiteral("state"), "XDG_STATE_HOME" },
            { QStringLiteral("data"), "XDG_DATA_HOME" }, { QStringLiteral("cache"), "XDG_CACHE_HOME" },
        };
        const QString base = vars.contains(area) ? qEnvironmentVariable(vars.value(area)) : QString();
        if (base.isEmpty() || QDir::isAbsolutePath(relativePath) || QDir::cleanPath(relativePath) != relativePath
            || relativePath.startsWith(QLatin1String("..")))
            return false;
        return QFileInfo::exists(base + QLatin1Char('/') + relativePath);
    }
    Q_INVOKABLE bool remove(const QString &relativePath)
    {
        const QString base = qEnvironmentVariable("XDG_DATA_HOME");
        if (base.isEmpty() || QDir::isAbsolutePath(relativePath) || QDir::cleanPath(relativePath) != relativePath
            || relativePath.startsWith(QLatin1String("..")))
            return false;
        return QFile::remove(base + QLatin1Char('/') + relativePath);
    }
};

class Setup : public QObject
{
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        // tst_main.qml launches the real test app (offscreen) through the
        // Launcher. Only test code reads this.
        engine->rootContext()->setContextProperty(QStringLiteral("mightyosTestApp"),
                                                  QStringLiteral(MIGHTYOS_TESTAPP_PATH));
        engine->rootContext()->setContextProperty(QStringLiteral("testFiles"), new TestFiles(engine));
        ArtworkProvider::install(engine);
    }
};

QUICK_TEST_MAIN_WITH_SETUP(mightyos_qml, Setup)

#include "tst_qml.moc"
