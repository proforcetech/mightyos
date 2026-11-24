#pragma once

#include "launcher.h"

#include <QElapsedTimer>
#include <QStringList>
#include <QTimer>

// Steam game exit detection (PLAN D20, risk 3: heuristic). Polls every
// `pollMs`; the game counts as running only when two sources agree:
//   registry: a registry.vdf says so (HKCU/Software/Valve/Steam
//             RunningAppID == appid, or apps/<appid>/Running == "1"), and
//   live:     a process of ours has SteamAppId=<appid> / SteamGameId=<appid>
//             in its environment, or the Steam client of a steam.pid file
//             is alive (ours, /proc/<pid>/comm starts with "steam").
// A registry that already said "running" when the launch started (left
// over from a crash) doesn't count: the registry has to change after
// start() before the game can be "seen".
// Waiting: until running is first true (appStarted), or `startTimeoutMs`
// passed (appFinished with seen() false: Steam needed a login, an update,
// showed an error, ...). Running: appFinished once `confirmPolls`
// consecutive polls say "not running". Guide (held) ends a tracked launch
// at any time (Launcher::stopTracking()).
//
// Known limits: Steam writes registry.vdf lazily (seconds of lag); a game
// that re-launches through a launcher keeps SteamAppId, but one that
// detaches completely can end early; while the Steam client stays alive
// and its registry still says running, a crashed game is noticed only when
// Steam updates the registry.
class SteamRunTracker : public LaunchTracker
{
    Q_OBJECT

public:
    struct Options {
        int pollMs = 2000;
        int startTimeoutMs = 120000;
        int confirmPolls = 2;
        QString procRoot = QStringLiteral("/proc");
        QStringList pidFiles;   // steam.pid files (SteamLibrary::steamPidFiles())
    };

    SteamRunTracker(const QString &appid, const QStringList &registryFiles, QObject *parent = nullptr);
    SteamRunTracker(const QString &appid, const QStringList &registryFiles, const Options &options,
                    QObject *parent = nullptr);

    void start() override;
    bool seen() const override { return m_seen; }

    // One poll; public for tests.
    bool isRunningNow() const;

private:
    void poll();

    QString m_appid;
    QStringList m_registryFiles;
    Options m_options;
    QTimer m_timer;
    QElapsedTimer m_elapsed;
    bool registryRunning() const;
    bool liveProcess() const;
    QByteArray registrySnapshot() const;

    QByteArray m_baseline;
    bool m_seen = false;
    bool m_done = false;
    int m_misses = 0;
};
