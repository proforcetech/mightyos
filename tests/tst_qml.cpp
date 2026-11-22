// Qt Quick Test runner for the QML tests (tests/qml/tst_*.qml).
// The MightyOS.Core and MightyOS.UI modules are static libraries, linked
// in and imported here exactly as in the app.
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQml/qqmlextensionplugin.h>
#include <QtQuickTest/quicktest.h>

Q_IMPORT_QML_PLUGIN(MightyOS_CorePlugin)
Q_IMPORT_QML_PLUGIN(MightyOS_UIPlugin)

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
    }
};

QUICK_TEST_MAIN_WITH_SETUP(mightyos_qml, Setup)

#include "tst_qml.moc"
