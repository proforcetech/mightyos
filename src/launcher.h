#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <sys/types.h>

// systemd user-manager tools for per-app scopes (PLAN section 4: "put each
// app in its own systemd-run --user --scope"). Usable only with both
// binaries at their fixed system paths (root-owned, not group/other
// writable; never from PATH) and a running user manager
// ($XDG_RUNTIME_DIR/systemd/private is a socket we own). Tests inject
// fakes with Launcher::setScopeTools().
struct ScopeTools {
    QString systemdRun;
    QString systemctl;

    bool isUsable() const { return !systemdRun.isEmpty() && !systemctl.isEmpty(); }
    static ScopeTools detect();
};

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
//
// Scopes (M6): with a systemd user manager (ScopeTools) every launch runs as
//   systemd-run --user --scope --quiet --collect --unit=<unit>
//               --property=TimeoutStopSec=3 -- <program> <args...>
// systemd-run creates the scope with its own pid and execs the program, so
// the QProcess pid is the app and exit codes are the app's. <unit> is
// scopeUnitName(): app-mightyos-<id, sanitised>-<random>.scope. Without a
// user manager (WSL, containers) the launch is the plain process-group path.
// `flatpak run` moves itself into its own app-flatpak-<appid>-<n>.scope
// (checked on the test PC), so terminate() targets the scope the app's
// process is in *now* (/proc/<pid>/cgroup), accepted only if it is this
// launch's unit or app-flatpak-<the launched app id>-<n>.scope; anything
// else falls back to the process group as well. If systemd-run itself fails
// (the process never shows up in its scope and exits non-zero), the launch
// is retried once without a scope and scopes stay off for the run.
// PDEATHSIG (SIGTERM when MightyOS dies) stays: a crashed MightyOS doesn't
// leave an untracked game running; in session mode the next start also
// stops leftover app-mightyos-*.scope units (stopStaleScopes()).
// Every child gets MightyOS's original RLIMIT_CORE back
// (ProcessHardening::restoreChildLimits()).
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
    // In scope mode: `systemctl --user kill --signal=SIGTERM <unit>` (every
    // process in the scope), and SIGKILL the same way after the grace time
    // if the scope's cgroup is still populated. See the class comment.
    Q_INVOKABLE bool terminate();
    static constexpr int kTerminateGraceMs = 3000;

    // Scope support (detected in the constructor; tests replace it).
    void setScopeTools(const ScopeTools &tools) { m_scopeTools = tools; }
    ScopeTools scopeTools() const { return m_scopeTools; }
    bool usesScopes() const { return m_scopeTools.isUsable(); }
    // The unit of the running launch ("" without scopes or when idle).
    QString currentScopeUnit() const { return m_running ? m_scopeUnit : QString(); }
    // The scopes of the running untracked launch: its own, plus the
    // app-flatpak-<id>-<n>.scope `flatpak run` moved to (probed for 3 s
    // after the start). main() keeps them in session.json.
    QStringList activeUnits() const { return m_activeUnits; }
    // Session start (only by the lock holder): `systemctl --user stop`
    // app-mightyos-*.scope and the `recorded` units of the last run that
    // look like ours (isRestartableUnit()). Tracked (Steam) launches use
    // app-mightyos.tracked-*, which this never matches. False without scopes.
    bool stopStaleScopes(const QStringList &recorded = {});
    static bool isRestartableUnit(const QString &unit);

    // "app-mightyos-<id>-<8 hex>.scope" ("app-mightyos.tracked-..." for
    // tracked launches): the id with everything but [A-Za-z0-9._] replaced
    // by '_', at most 64 characters.
    static QString scopeUnitName(const QString &id, quint32 random, bool tracked = false);
    // systemd-run arguments (after the program) for one launch.
    static QStringList scopeArguments(const QString &unit, const QString &program, const QStringList &args);
    // The app id of a `flatpak run [--opt=value...] <id> ...` argv, else "".
    static QString flatpakAppIdOf(const QStringList &argv);
    // The unit a process is in now, from the content of /proc/<pid>/cgroup
    // ("0::/user.slice/.../app.slice/<unit>"), if it is `ownUnit` or
    // app-flatpak-<flatpakId>-<digits>.scope (that exact app); ""
    // otherwise. `cgroupPath` gets the cgroup v2 path ("/user.slice/...").
    static QString acceptedScopeUnit(const QByteArray &procCgroup, const QString &ownUnit,
                                     const QString &flatpakId = QString(), QString *cgroupPath = nullptr);
    // Tests: stands in for /proc/<pid>/cgroup (gets the pid and the
    // launch's own unit). An empty function restores the real one.
    static void setCgroupReaderForTests(std::function<QByteArray(qint64, const QString &)> reader);

    // argv of the bundled test program, the launch target for (mock)
    // games until the real library arrives in M4. Apps launch with the
    // argv AppRegistry parsed. Empty if the test program is missing.
    Q_INVOKABLE QStringList testAppCommand() const;

    // Exposed for unit tests.
    static QProcessEnvironment buildChildEnvironment(const QProcessEnvironment &source);
    static QString resolveProgram(const QString &program, const QProcessEnvironment &env);

signals:
    void runningChanged();
    void activeUnitsChanged();
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
    // Starts `systemctl --user kill --signal=<sig> -- <unit>`; with `wait`
    // blocks up to 1 s (destructor). If systemctl fails (or can't start),
    // `fallbackPgid` (if > 0) gets the signal instead. False if no scopes.
    bool killScope(const QString &unit, int sig, bool wait, pid_t fallbackPgid);
    bool startProcess(bool scoped);
    void probeScope();
    void setActiveUnits(const QStringList &units);
    QProcess *startHelper(const QString &program, const QStringList &args);

    QPointer<QProcess> m_process;
    QPointer<LaunchTracker> m_tracker;
    QList<QPointer<QProcess>> m_background;
    QList<QPointer<QProcess>> m_helpers;   // systemctl calls
    bool m_waiting = false;
    QString m_currentId;
    bool m_running = false;
    bool m_terminating = false;
    ScopeTools m_scopeTools;
    QString m_scopeUnit;
    QString m_flatpakId;
    QString m_program;
    QStringList m_arguments;
    QStringList m_activeUnits;
    QTimer m_scopeProbe;
    QElapsedTimer m_probeUntil;
    bool m_scopeConfirmed = false;
    bool m_launchedEmitted = false;
};
