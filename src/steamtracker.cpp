#include "steamtracker.h"

#include "jsonfile.h"
#include "steamlibrary.h"

SteamRunTracker::SteamRunTracker(const QString &appid, const QStringList &registryFiles, QObject *parent)
    : SteamRunTracker(appid, registryFiles, Options(), parent)
{
}

SteamRunTracker::SteamRunTracker(const QString &appid, const QStringList &registryFiles, const Options &options,
                                 QObject *parent)
    : LaunchTracker(parent)
    , m_appid(appid)
    , m_registryFiles(registryFiles)
    , m_options(options)
{
    m_timer.setInterval(qMax(50, m_options.pollMs));
    connect(&m_timer, &QTimer::timeout, this, &SteamRunTracker::poll);
}

void SteamRunTracker::start()
{
    if (m_timer.isActive() || m_done)
        return;
    m_baseline = registrySnapshot();
    m_elapsed.start();
    m_timer.start();
}

QByteArray SteamRunTracker::registrySnapshot() const
{
    QByteArray all;
    for (const QString &file : m_registryFiles) {
        const auto data = JsonFile::readRegularFile(file, SteamLibrary::kMaxVdfBytes, true);
        all += data.value_or(QByteArray()) + '\0';
    }
    return all;
}

bool SteamRunTracker::registryRunning() const
{
    for (const QString &file : m_registryFiles) {
        const auto data = JsonFile::readRegularFile(file, SteamLibrary::kMaxVdfBytes, true);
        if (data && SteamLibrary::registrySaysRunning(*data, m_appid))
            return true;
    }
    return false;
}

bool SteamRunTracker::liveProcess() const
{
    if (m_options.procRoot.isEmpty())
        return false;
    if (SteamLibrary::steamClientAlive(m_options.procRoot, m_options.pidFiles))
        return true;
    return SteamLibrary::processRunning(m_options.procRoot, m_appid);
}

bool SteamRunTracker::isRunningNow() const
{
    // Cheap check first; the /proc scan only when the registry says yes.
    return registryRunning() && liveProcess();
}

void SteamRunTracker::poll()
{
    if (m_done)
        return;
    const bool running = isRunningNow();
    if (!m_seen) {
        // A stale "running" from before the launch doesn't count.
        if (running && registrySnapshot() != m_baseline) {
            m_seen = true;
            emit appStarted();
        } else if (m_elapsed.elapsed() >= m_options.startTimeoutMs) {
            m_done = true;
            m_timer.stop();
            emit appFinished();
        }
        return;
    }
    m_misses = running ? 0 : m_misses + 1;
    if (m_misses >= qMax(1, m_options.confirmPolls)) {
        m_done = true;
        m_timer.stop();
        emit appFinished();
    }
}
