#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

// Crash counter for the MightyOS session (PLAN D29: "fall back to safe mode
// after repeated crashes").
//
// File: $XDG_STATE_HOME/mightyos/session.json, written with
// JsonFile::writePrivate (0600, never through a symlink), read with
// JsonFile::readRegularFile (no symlinks, capped):
//   { "version": 2, "boot": "<boot id>", "up": true,
//     "crashes": [<seconds since boot>, ...], "warnedGroups": ["sudo"],
//     "units": ["app-mightyos-...scope"] }
//
// Only what the restart loop saw counts (mightyos-session passes the last
// exit status as --previous-status=N):
//  - N >= 128: killed by a signal (SIGSEGV, SIGABRT, SIGKILL / OOM),
//  - 0 < N < 128 while "up" was still false: it failed during startup
//    (QML didn't load, ...). An exit code after startup is not a crash:
//    Qt's Wayland plugin _exit()s with 1 when the compositor goes away,
//    i.e. on every logout,
//  - the first start of a session (no --previous-status) never counts.
// Crash times are CLOCK_BOOTTIME seconds; a different boot id (a reboot)
// starts the list empty, so wall-clock jumps don't matter. Crashes older
// than kWindowSecs are dropped; with kSafeModeCrashes or more left, this
// start is in safe mode.
//
// An unusable file (not JSON, wrong version, too large) is moved aside to
// session.json.corrupt and the counter starts empty.
class SessionState
{
public:
    static constexpr int kVersion = 2;
    static constexpr int kSafeModeCrashes = 3;
    static constexpr qint64 kWindowSecs = 600;
    static constexpr int kMaxCrashEntries = 16;
    static constexpr int kMaxUnits = 8;
    static constexpr qint64 kMaxFileBytes = 16 * 1024;

    struct Start {
        bool previousCrashed = false;
        int recentCrashes = 0;
        bool safeMode = false;
    };

    // `stateHome`: $XDG_STATE_HOME (the file goes to <stateHome>/mightyos).
    explicit SessionState(const QString &stateHome);

    QString filePath() const;

    // The decision on an already-read state object (pure; updates `state`:
    // appends the crash, prunes old ones, sets "up": false and "boot").
    // `previousStatus` < 0: the first start of this session.
    static Start evaluate(QJsonObject &state, qint64 bootSecs, const QString &bootId, int previousStatus);
    static bool isCrash(int previousStatus, bool wasUp);

    // Reads, evaluates and writes back. A write failure is logged; the
    // decision still applies.
    Start recordStart(int previousStatus);
    Start recordStart(qint64 bootSecs, const QString &bootId, int previousStatus);
    // Startup finished (the window is up): later exit codes aren't crashes.
    bool markUp();

    // The scopes of the running launch (S7), stopped at the next session
    // start if MightyOS dies with it.
    QStringList launchUnits() const;
    bool setLaunchUnits(const QStringList &units);

    // Groups the session user was already warned about (one toast per new
    // set, not on every start).
    QStringList warnedGroups() const;
    bool setWarnedGroups(const QStringList &groups);

    // Groups that a kiosk session user should not be in (PLAN D8, D16):
    // root-equivalent (sudo, admin, wheel, root, disk, lxd, incus-admin,
    // docker, libvirt) or raw device access that bypasses the seat (video,
    // input, tty). Sorted, from `names`.
    static QStringList riskyGroups(const QStringList &names);
    // Worth knowing, not a warning: adm, systemd-journal (read all logs).
    static QStringList informationalGroups(const QStringList &names);
    // Names of this process's supplementary and primary groups.
    static QStringList currentGroupNames();

    // CLOCK_BOOTTIME in seconds; /proc/sys/kernel/random/boot_id.
    static qint64 bootSeconds();
    static QString bootId();

private:
    QJsonObject read() const;
    bool write(const QJsonObject &state) const;
    bool update(const char *key, const QJsonValue &value) const;

    QString m_stateHome;
};

// The session instance's lock and pid file (PLAN D28):
// $XDG_RUNTIME_DIR/mightyos/session.pid, 0600 in a 0700 dir we own, opened
// without following symlinks and flock()ed exclusively for the life of the
// process. Only the holder is "the session": it stops leftover app scopes
// (a second instance never does), and `mightyos --signal=...` (the labwc
// panic keys) signals only a pid whose file is still locked and that is
// still a process named mightyos of this user.
class SessionLock
{
public:
    SessionLock() = default;
    ~SessionLock();
    SessionLock(const SessionLock &) = delete;
    SessionLock &operator=(const SessionLock &) = delete;

    // False if another process holds it, or on any error (`*error`).
    bool acquire(const QString &runtimeDir, qint64 pid, QString *error = nullptr);
    bool isHeld() const { return m_fd >= 0; }

    static QString pidFilePath(const QString &runtimeDir);
    // Sends `signo` to the locked session instance. Returns "", or a
    // message for the user (no session, stale file, not MightyOS, ...).
    static QString signalSession(const QString &runtimeDir, int signo);

private:
    int m_fd = -1;
};
