#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

// Launcher starts exactly one external application at a time and reports its
// lifecycle to QML.
//
// Launch hygiene (PLAN.md section 4):
//  - Program + argument list via QProcess. Never a shell, never a command
//    string that gets split or interpreted.
//  - The child gets a minimal allowlisted environment; LD_* is always removed.
//  - Relative program names are resolved against the sanitised PATH only.
//
// Deferred: systemd-run --user --scope per app (M6), "--" separators for
// specific emulators (M4), Guide overlay / force-close (M6).
class Launcher : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged FINAL)
    Q_PROPERTY(QString currentId READ currentId NOTIFY currentIdChanged FINAL)

public:
    explicit Launcher(QObject *parent = nullptr);
    ~Launcher() override;

    bool isRunning() const { return m_running; }
    QString currentId() const { return m_currentId; }

    // argv[0] is the program (absolute path or a name looked up in the
    // sanitised PATH); argv[1..] are passed verbatim as separate arguments.
    // Returns true if the process start was initiated. Every failure path
    // also emits failed(), so QML only needs to listen to the signal.
    Q_INVOKABLE bool launch(const QString &id, const QStringList &argv);

    // M0 only: argv of a harmless GUI program to use as a launch target.
    // Empty if nothing suitable was found. Replaced by AppRegistry in M2.
    Q_INVOKABLE QStringList testAppCommand() const;

    // Exposed for unit tests.
    static QProcessEnvironment buildChildEnvironment(const QProcessEnvironment &source);
    static QString resolveProgram(const QString &program, const QProcessEnvironment &env);

signals:
    void runningChanged();
    void currentIdChanged();
    void launched(const QString &id);
    // exitCode is -1 if the process crashed or was killed by a signal.
    void exited(const QString &id, int exitCode);
    void failed(const QString &id, const QString &error);

private:
    void setRunning(bool running);
    void setCurrentId(const QString &id);
    void finish();

    QPointer<QProcess> m_process;
    QString m_currentId;
    bool m_running = false;
};
