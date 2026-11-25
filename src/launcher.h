#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

// Decides when a launched app has *really* ended, for launchers whose
// process exits long before the game does (`steam steam://rungameid/...`
// hands the request to the running Steam client and exits at once) or
// stays alive after it (a Steam client started by that call). Launcher
// owns it for the duration of one launch.
class LaunchTracker : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    // Called once, after the launched process has started.
    virtual void start() = 0;
    // True once the app was seen running.
    virtual bool seen() const = 0;

signals:
    void appStarted();
    // The app ended, or never showed up in time (seen() tells which).
    void appFinished();
};

// Launcher starts exactly one external application at a time and reports its
// lifecycle to QML.
//
// Launch hygiene (PLAN.md section 4):
//  - Program + argument list via QProcess. Never a shell, never a command
//    string that gets split or interpreted.
//  - The child gets a minimal allowlisted environment; LD_* is always removed.
//  - Relative program names are resolved against the sanitised PATH only.
//
// Emulator argv come from validated systems.d templates (placeholders are
// whole elements, ROM paths are absolute so they can't pose as options;
// templates add "--" where the emulator supports it).
// Deferred: systemd-run --user --scope per app (M6), Guide overlay /
// force-close (M6).
class Launcher : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged FINAL)
    Q_PROPERTY(QString currentId READ currentId NOTIFY currentIdChanged FINAL)
    // A tracked launch whose app hasn't shown up yet (Steam starting,
    // updating, logging in).
    Q_PROPERTY(bool waiting READ isWaiting NOTIFY waitingChanged FINAL)
    // The running launch has a tracker (Steam): holding Guide calls
    // stopTracking() to give the screen back, waiting or not.
    Q_PROPERTY(bool tracked READ isTracked NOTIFY runningChanged FINAL)

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
    // Same, with a tracker (taken over, deleted after the launch) that
    // decides when the app has ended:
    //  - the process exiting with 0 while the tracker waits is a hand-over,
    //    not the end; a non-zero exit before the app was seen is a failure
    //    and ends the launch like an untracked one;
    //  - appFinished() ends the launch with exited(id, 0); a process still
    //    running then (e.g. the Steam client it started) is left running
    //    in the background (terminated with MightyOS, like any child).
    bool launch(const QString &id, const QStringList &argv, LaunchTracker *tracker);
    bool isWaiting() const { return m_waiting; }
    bool isTracked() const { return m_running && m_tracker; }
    // Ends a tracked launch now, whether its app was seen or not
    // (exited(id, 0)); the launched process is left running in the
    // background. No-op for untracked launches.
    Q_INVOKABLE void stopTracking();
    // Starts a short helper (e.g. `steam -shutdown`) with the same hygiene
    // as launch() (argv only, clean env, own session), not tracked and not
    // affecting `running`. False if the program isn't found.
    bool runAuxiliary(const QStringList &argv);
    // Closes the running app on request (Guide held on a kiosk web app or a
    // Moonlight stream, which have no exit of their own a gamepad reaches):
    // SIGTERM to its process group (flatpak run / bwrap / the browser),
    // SIGKILL to the same group kTerminateGraceMs later if anything in it
    // is still alive (a Launcher-owned timer keyed by the pgid, so it also
    // catches members that outlive the leader). The end is reported as
    // exited(id, 0). Untracked launches only: false for a tracked (Steam)
    // launch, whose process may be the Steam client (use stopTracking()),
    // and when nothing runs. A second call while closing returns true and
    // does nothing.
    Q_INVOKABLE bool terminate();
    static constexpr int kTerminateGraceMs = 3000;

    // argv of the bundled test program, the launch target for (mock)
    // games until the real library arrives in M4. Apps launch with the
    // argv AppRegistry parsed. Empty if the test program is missing.
    Q_INVOKABLE QStringList testAppCommand() const;

    // Exposed for unit tests.
    static QProcessEnvironment buildChildEnvironment(const QProcessEnvironment &source);
    static QString resolveProgram(const QString &program, const QProcessEnvironment &env);

signals:
    void runningChanged();
    void currentIdChanged();
    void waitingChanged();
    void launched(const QString &id);
    // exitCode is -1 if the process crashed or was killed by a signal.
    void exited(const QString &id, int exitCode);
    void failed(const QString &id, const QString &error);

private:
    void setRunning(bool running);
    void setCurrentId(const QString &id);
    void finish();
    void setWaiting(bool waiting);
    void endTracked();

    QPointer<QProcess> m_process;
    QPointer<LaunchTracker> m_tracker;
    QList<QPointer<QProcess>> m_background;
    bool m_waiting = false;
    QString m_currentId;
    bool m_running = false;
    bool m_terminating = false;
};
