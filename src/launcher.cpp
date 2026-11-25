#include "launcher.h"

#include "processhardening.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <functional>

#include <csignal>
#include <memory>
#include <cstring>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcLauncher, "mightyos.launcher")

namespace {

// Variables copied verbatim when present.
constexpr const char *kAllowedNames[] = {
    "PATH", "HOME", "USER", "LOGNAME", "TZ",
    "LANG", "LANGUAGE",
    "WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY",
    "DBUS_SESSION_BUS_ADDRESS",
};

// Variables copied when their name starts with one of these prefixes.
constexpr const char *kAllowedPrefixes[] = {
    "XDG_", "LC_", "PULSE_", "PIPEWIRE_",
};

// Never passed on, even if a future allowlist entry would match.
constexpr const char *kDeniedPrefixes[] = {
    "LD_",
};

constexpr const char *kFallbackPath = "/usr/local/bin:/usr/bin:/bin";

bool startsWithAny(const QString &name, const auto &prefixes)
{
    for (const char *prefix : prefixes) {
        if (name.startsWith(QLatin1StringView(prefix)))
            return true;
    }
    return false;
}

bool isAllowed(const QString &name)
{
    if (startsWithAny(name, kDeniedPrefixes))
        return false;
    for (const char *allowed : kAllowedNames) {
        if (name == QLatin1StringView(allowed))
            return true;
    }
    return startsWithAny(name, kAllowedPrefixes);
}

// Keep only absolute, existing-or-not directories; drop "", "." and other
// relative entries so a launch can never pick up a binary from the CWD.
QString sanitisePath(const QString &path)
{
    QStringList kept;
    const QStringList parts = path.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (QDir::isAbsolutePath(part) && !kept.contains(part))
            kept.append(part);
    }
    return kept.isEmpty() ? QString::fromLatin1(kFallbackPath) : kept.join(QLatin1Char(':'));
}

// Each child runs in its own session (pgid == pid), so signalling the
// negative pid reaches wrappers' children too (flatpak run, steam, ...).
void signalGroup(QProcess *process, int sig)
{
    const qint64 pid = process->processId();
    if (pid > 0)
        ::kill(-static_cast<pid_t>(pid), sig);
}

// A system binary we may run without PATH: regular, executable, owned by
// root, not writable by group or others (also its directory).
bool trustedSystemBinary(const QString &path)
{
    struct stat st {};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0 || !S_ISREG(st.st_mode)
        || st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)) || !(st.st_mode & S_IXUSR))
        return false;
    struct stat dir {};
    const QString parent = QFileInfo(path).absolutePath();
    return ::stat(QFile::encodeName(parent).constData(), &dir) == 0 && dir.st_uid == 0
           && !(dir.st_mode & (S_IWGRP | S_IWOTH));
}

QString firstTrusted(std::initializer_list<const char *> candidates)
{
    for (const char *candidate : candidates) {
        const QString path = QString::fromLatin1(candidate);
        if (trustedSystemBinary(path))
            return path;
    }
    return {};
}

// The child side of every launch: runs between fork() and execve(), so
// async-signal-safe calls only.
void childSetup() noexcept
{
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    ProcessHardening::restoreChildLimits();
}

// procfs / cgroupfs files report st_size 0, which JsonFile's size check
// rejects; these are kernel files under fixed roots, read capped.
QByteArray readKernelFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
        return {};
    return file.read(4096);
}

// "populated 1" in <cgroup>/cgroup.events: something still runs there.
bool cgroupPopulated(const QString &cgroupPath)
{
    if (cgroupPath.isEmpty())
        return false;
    return readKernelFile(QStringLiteral("/sys/fs/cgroup") + cgroupPath + QStringLiteral("/cgroup.events"))
        .contains("populated 1");
}


// Tests replace /proc/<pid>/cgroup (a fake systemd-run creates no scope).
std::function<QByteArray(qint64, const QString &)> &cgroupReader()
{
    static std::function<QByteArray(qint64, const QString &)> reader;
    return reader;
}

QByteArray readProcCgroup(qint64 pid, const QString &ownUnit = QString())
{
    if (pid <= 0)
        return {};
    if (cgroupReader())
        return cgroupReader()(pid, ownUnit);
    return readKernelFile(QStringLiteral("/proc/%1/cgroup").arg(pid));
}

// SIGKILL to everything in a cgroup (cgroup v2 cgroup.kill), in-process.
bool cgroupKill(const QString &cgroupPath)
{
    if (cgroupPath.isEmpty())
        return false;
    QFile file(QStringLiteral("/sys/fs/cgroup") + cgroupPath + QStringLiteral("/cgroup.kill"));
    return file.open(QIODevice::WriteOnly | QIODevice::Unbuffered) && file.write("1") == 1;
}

constexpr int kScopeProbeIntervalMs = 100;
constexpr int kScopeProbeMs = 3000;

} // namespace

ScopeTools ScopeTools::detect()
{
    ScopeTools tools;
    // A user manager: its private socket, ours.
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty() || !QDir::isAbsolutePath(runtime))
        return tools;
    struct stat st {};
    const QByteArray socket = QFile::encodeName(runtime + QStringLiteral("/systemd/private"));
    if (::lstat(socket.constData(), &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != ::getuid())
        return tools;
    // ...that answers: a socket file left behind by a manager that is gone
    // (seen on WSL) refuses the connection, and systemd-run would fail
    // every launch.
    sockaddr_un address {};
    if (size_t(socket.size()) >= sizeof address.sun_path)
        return tools;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket.constData(), size_t(socket.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return tools;
    const bool answers = ::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0;
    ::close(fd);
    if (!answers)
        return tools;
    tools.systemdRun = firstTrusted({ "/usr/bin/systemd-run", "/bin/systemd-run" });
    tools.systemctl = firstTrusted({ "/usr/bin/systemctl", "/bin/systemctl" });
    if (!tools.isUsable())
        tools = {};
    return tools;
}

Launcher::Launcher(QObject *parent)
    : QObject(parent)
    , m_scopeTools(ScopeTools::detect())
{
    qCInfo(lcLauncher) << (m_scopeTools.isUsable() ? "apps run in systemd user scopes"
                                                   : "no systemd user manager: apps run in their own process group");
    m_scopeProbe.setInterval(kScopeProbeIntervalMs);
    connect(&m_scopeProbe, &QTimer::timeout, this, &Launcher::probeScope);
}

void Launcher::setCgroupReaderForTests(std::function<QByteArray(qint64, const QString &)> reader)
{
    cgroupReader() = std::move(reader);
}

QString Launcher::scopeUnitName(const QString &id, quint32 random, bool tracked)
{
    QString name;
    for (const QChar c : id) {
        const bool keep = (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                          || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('.') || c == QLatin1Char('_');
        name.append(keep ? c : QLatin1Char('_'));
        if (name.size() >= 64)
            break;
    }
    if (name.isEmpty())
        name = QStringLiteral("app");
    return QStringLiteral("%1-%2-%3.scope")
        .arg(tracked ? QStringLiteral("app-mightyos.tracked") : QStringLiteral("app-mightyos"), name)
        .arg(random, 8, 16, QLatin1Char('0'));
}

QStringList Launcher::scopeArguments(const QString &unit, const QString &program, const QStringList &args)
{
    return QStringList { QStringLiteral("--user"), QStringLiteral("--scope"), QStringLiteral("--quiet"),
                         QStringLiteral("--collect"), QStringLiteral("--unit=") + unit,
                         QStringLiteral("--description=MightyOS launch"),
                         QStringLiteral("--property=TimeoutStopSec=3"), QStringLiteral("--"), program }
           + args;
}

QString Launcher::flatpakAppIdOf(const QStringList &argv)
{
    if (argv.size() < 3 || QFileInfo(argv.at(0)).fileName() != QLatin1String("flatpak")
        || argv.at(1) != QLatin1String("run"))
        return {};
    static const QRegularExpression appId(QStringLiteral("^[A-Za-z_][A-Za-z0-9_-]*(\\.[A-Za-z_][A-Za-z0-9_-]*){2,}$"));
    for (int i = 2; i < argv.size(); ++i) {
        const QString &arg = argv.at(i);
        if (arg == QLatin1String("--"))
            return i + 1 < argv.size() && appId.match(argv.at(i + 1)).hasMatch() ? argv.at(i + 1) : QString();
        if (arg.startsWith(QLatin1Char('-')))
            continue;   // our launches always use --option=value
        return appId.match(arg).hasMatch() ? arg : QString();
    }
    return {};
}

QString Launcher::acceptedScopeUnit(const QByteArray &procCgroup, const QString &ownUnit, const QString &flatpakId,
                                    QString *cgroupPath)
{
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,20}$"));
    static const QRegularExpression cleanPath(QStringLiteral("^(/[A-Za-z0-9@:._\\\\-]+)+$"));
    for (const QByteArray &line : procCgroup.split('\n')) {
        if (!line.startsWith("0::"))
            continue;   // cgroup v2 only
        const QString path = QString::fromUtf8(line.mid(3)).trimmed();
        if (!cleanPath.match(path).hasMatch() || path.contains(QLatin1String("/../")) || path.endsWith(QLatin1String("/..")))
            return {};
        const QString unit = path.section(QLatin1Char('/'), -1);
        bool accepted = !ownUnit.isEmpty() && unit == ownUnit;
        // `flatpak run <id>` moves itself to app-flatpak-<id>-<instance>.scope:
        // only the app this launch started.
        const QString prefix = QStringLiteral("app-flatpak-") + flatpakId + QLatin1Char('-');
        if (!accepted && !flatpakId.isEmpty() && unit.startsWith(prefix) && unit.endsWith(QLatin1String(".scope")))
            accepted = digits.match(unit.mid(prefix.size(), unit.size() - prefix.size() - 6)).hasMatch();
        if (accepted && cgroupPath)
            *cgroupPath = path;
        return accepted ? unit : QString();
    }
    return {};
}

bool Launcher::isRestartableUnit(const QString &unit)
{
    static const QRegularExpression ours(QStringLiteral("^app-mightyos-[A-Za-z0-9._]{1,64}-[0-9a-f]{8}\\.scope$"));
    static const QRegularExpression flatpak(QStringLiteral("^app-flatpak-[A-Za-z0-9._-]{1,255}-[0-9]{1,20}\\.scope$"));
    return ours.match(unit).hasMatch() || flatpak.match(unit).hasMatch();
}

QProcess *Launcher::startHelper(const QString &program, const QStringList &args)
{
    const QProcessEnvironment env = buildChildEnvironment(QProcessEnvironment::systemEnvironment());
    auto *process = new QProcess(this);
    process->setProgram(program);
    process->setArguments(args);
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(env.value(QStringLiteral("HOME"), QDir::homePath()));
    process->setStandardInputFile(QProcess::nullDevice());
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::ResetSignalHandlers
                                      | QProcess::UnixProcessFlag::CloseFileDescriptors
                                      | QProcess::UnixProcessFlag::CreateNewSession);
    process->setChildProcessModifier([] { ProcessHardening::restoreChildLimits(); });
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    m_helpers.removeAll(nullptr);
    m_helpers.append(process);
    process->start();
    return process;
}

bool Launcher::killScope(const QString &unit, int sig, bool wait, pid_t fallbackPgid)
{
    if (!m_scopeTools.isUsable() || unit.isEmpty())
        return false;
    const QString name = sig == SIGKILL ? QStringLiteral("SIGKILL") : QStringLiteral("SIGTERM");
    QPointer<QProcess> process = startHelper(m_scopeTools.systemctl,
        { QStringLiteral("--user"), QStringLiteral("kill"), QStringLiteral("--signal=") + name,
          QStringLiteral("--"), unit });
    if (!process)
        return false;
    // systemctl failing (unit gone, manager unreachable): the process group.
    const auto onDone = [unit, sig, fallbackPgid](int code, QProcess::ExitStatus status) {
        if ((status != QProcess::NormalExit || code != 0) && fallbackPgid > 0) {
            qCWarning(lcLauncher).noquote() << "systemctl kill" << unit << "failed; signalling the process group";
            ::kill(-fallbackPgid, sig);
        }
    };
    if (wait) {
        if (process->waitForFinished(1000))
            onDone(process->exitCode(), process->exitStatus());
        else if (fallbackPgid > 0)
            ::kill(-fallbackPgid, sig);
        return true;
    }
    connect(process, &QProcess::finished, this, onDone);
    connect(process, &QProcess::errorOccurred, this, [unit, sig, fallbackPgid](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && fallbackPgid > 0)
            ::kill(-fallbackPgid, sig);
    });
    return true;
}

bool Launcher::stopStaleScopes(const QStringList &recorded)
{
    if (!m_scopeTools.isUsable())
        return false;
    // app-mightyos.tracked-* (Steam launches: the client may still be in
    // use) never match this pattern.
    QStringList units { QStringLiteral("app-mightyos-*.scope") };
    for (const QString &unit : recorded) {
        if (isRestartableUnit(unit) && !units.contains(unit))
            units.append(unit);
    }
    qCInfo(lcLauncher).noquote() << "stopping leftover scopes:" << units.join(QLatin1Char(' '));
    return startHelper(m_scopeTools.systemctl, QStringList { QStringLiteral("--user"), QStringLiteral("stop"),
                                                             QStringLiteral("--no-block"), QStringLiteral("--") }
                                                   + units) != nullptr;
}

Launcher::~Launcher()
{
    // A pending `systemctl kill` (an escalation) finishes first.
    for (const QPointer<QProcess> &helper : std::as_const(m_helpers)) {
        if (helper && helper->state() != QProcess::NotRunning)
            helper->waitForFinished(1000);
    }
    for (const QPointer<QProcess> &process : std::as_const(m_background)) {
        if (process && process->state() != QProcess::NotRunning) {
            process->disconnect(this);
            signalGroup(process, SIGTERM);
            if (!process->waitForFinished(1500)) {
                signalGroup(process, SIGKILL);
                process->waitForFinished(500);
            }
        }
    }
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // Leaving the launcher (a clean quit, SIGTERM from logind) takes
        // the running app down with it: its whole scope, and the group.
        m_process->disconnect(this);
        const pid_t pgid = pid_t(m_process->processId());
        QString path;
        QString unit = acceptedScopeUnit(readProcCgroup(pgid, m_scopeUnit), m_scopeUnit, m_flatpakId, &path);
        if (unit.isEmpty())
            unit = m_scopeUnit;
        qCInfo(lcLauncher).noquote() << "quitting: closing" << m_currentId << unit;
        killScope(unit, SIGTERM, true, pgid);
        signalGroup(m_process, SIGTERM);
        if (!m_process->waitForFinished(1500)) {
            killScope(unit, SIGKILL, true, pgid);
            cgroupKill(path);
            signalGroup(m_process, SIGKILL);
            m_process->waitForFinished(500);
        }
    }
}

QProcessEnvironment Launcher::buildChildEnvironment(const QProcessEnvironment &source)
{
    QProcessEnvironment env;
    const QStringList keys = source.keys();
    for (const QString &key : keys) {
        if (isAllowed(key))
            env.insert(key, source.value(key));
    }
    env.insert(QStringLiteral("PATH"), sanitisePath(env.value(QStringLiteral("PATH"))));
    return env;
}

QString Launcher::resolveProgram(const QString &program, const QProcessEnvironment &env)
{
    if (program.isEmpty())
        return {};

    if (program.contains(QLatin1Char('/'))) {
        // Only absolute paths; "./foo" or "bin/foo" would depend on the CWD.
        if (!QDir::isAbsolutePath(program))
            return {};
        const QFileInfo info(program);
        return (info.isFile() && info.isExecutable()) ? info.absoluteFilePath() : QString();
    }

    const QStringList dirs = env.value(QStringLiteral("PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    return QStandardPaths::findExecutable(program, dirs);
}

bool Launcher::launch(const QString &id, const QStringList &argv)
{
    return launch(id, argv, nullptr);
}

bool Launcher::launch(const QString &id, const QStringList &argv, LaunchTracker *tracker)
{
    // Owned from here on, whatever happens.
    std::unique_ptr<LaunchTracker> owned(tracker);
    if (id.isEmpty()) {
        emit failed(id, tr("No application id given."));
        return false;
    }
    if (m_running) {
        emit failed(id, tr("Another application is already running."));
        return false;
    }
    if (argv.isEmpty()) {
        emit failed(id, tr("No program configured."));
        return false;
    }

    const QProcessEnvironment env = buildChildEnvironment(QProcessEnvironment::systemEnvironment());
    const QString program = resolveProgram(argv.first(), env);
    if (program.isEmpty()) {
        // Ids only in logs, never argv (PLAN section 4); the program name
        // is shown to the user in the toast, not logged.
        qCWarning(lcLauncher) << "launch" << id << "- program not found";
        emit failed(id, tr("Program not found: %1").arg(argv.first()));
        return false;
    }

    if (owned) {
        m_tracker = owned.release();
        m_tracker->setParent(this);
        connect(m_tracker, &LaunchTracker::appStarted, this, [this] {
            qCInfo(lcLauncher) << "tracked app" << m_currentId << "is running";
            setWaiting(false);
        });
        connect(m_tracker, &LaunchTracker::appFinished, this, &Launcher::endTracked);
        setWaiting(true);
    }
    m_program = program;
    m_arguments = argv.mid(1);
    m_flatpakId = flatpakAppIdOf(QStringList { program } + m_arguments);
    setCurrentId(id);
    setRunning(true);
    qCInfo(lcLauncher) << "launching" << id;
    return startProcess(m_scopeTools.isUsable());
}

bool Launcher::startProcess(bool scoped)
{
    const QProcessEnvironment env = buildChildEnvironment(QProcessEnvironment::systemEnvironment());
    auto *process = new QProcess(this);
    QString unit;
    if (scoped) {
        unit = scopeUnitName(m_currentId, QRandomGenerator::system()->generate(), !m_tracker.isNull());
        process->setProgram(m_scopeTools.systemdRun);
        process->setArguments(scopeArguments(unit, m_program, m_arguments));
    } else {
        process->setProgram(m_program);
        process->setArguments(m_arguments);
    }
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(env.value(QStringLiteral("HOME"), QDir::homePath()));
    process->setStandardInputFile(QProcess::nullDevice());
    // Child output goes to our stdout/stderr (the session log). A scope
    // inherits it as well (only services get the journal).
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    // Clean slate for the child: default signal handlers, no inherited fds,
    // its own session/process group.
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::ResetSignalHandlers
                                      | QProcess::UnixProcessFlag::CloseFileDescriptors
                                      | QProcess::UnixProcessFlag::CreateNewSession);
    // If MightyOS dies abnormally (crash, SIGKILL) the child gets SIGTERM
    // instead of being orphaned (survives systemd-run's exec of the app).
    process->setChildProcessModifier(childSetup);

    connect(process, &QProcess::started, this, [this, process] {
        if (process != m_process)
            return;
        if (m_scopeUnit.isEmpty())
            qCInfo(lcLauncher).noquote() << "started" << m_currentId << "pid" << process->processId();
        else
            qCInfo(lcLauncher).noquote() << "started" << m_currentId << "pid" << process->processId() << "in" << m_scopeUnit;
        if (!m_scopeUnit.isEmpty()) {
            m_probeUntil.start();
            m_scopeProbe.start();
            probeScope();
        }
        if (!m_launchedEmitted) {
            m_launchedEmitted = true;
            emit launched(m_currentId);
            if (m_tracker)
                m_tracker->start();
        }
    });

    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process != m_process)
            return;
        // Crashes and read/write errors are followed by finished(); only a
        // failed start ends the launch here.
        if (error != QProcess::FailedToStart)
            return;
        const QString id = m_currentId;
        const QString message = process->errorString();
        qCWarning(lcLauncher) << "failed to start" << id;
        finish();
        emit failed(id, message);
    });

    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus status) {
        if (process != m_process)
            return;
        const QString id = m_currentId;
        if (!m_scopeUnit.isEmpty() && !m_scopeConfirmed && !m_terminating && !m_tracker
            && (status != QProcess::NormalExit || exitCode != 0)) {
            // Never seen in its scope: systemd-run itself failed (the user
            // manager went away, D-Bus trouble). The app never ran: once
            // more without a scope, and no scopes for the rest of the run.
            qCWarning(lcLauncher).noquote() << "systemd-run failed for" << m_scopeUnit
                                            << "- launching without a scope from now on";
            m_scopeTools = {};
            m_scopeProbe.stop();
            m_scopeUnit.clear();
            setActiveUnits({});
            process->disconnect(this);
            process->deleteLater();
            m_process = nullptr;
            startProcess(false);
            return;
        }
        // Closed on request: a SIGTERM/SIGKILL ending is the expected one.
        const int code = m_terminating ? 0 : (status == QProcess::CrashExit) ? -1 : exitCode;
        if (m_tracker && (code == 0 || m_tracker->seen())) {
            // Handed over (steam://...): the tracker decides from here.
            qCInfo(lcLauncher) << "launcher process of" << id << "exited, tracking the app";
            m_process->disconnect(this);
            m_process->deleteLater();
            m_process = nullptr;
            return;
        }
        qCInfo(lcLauncher) << "exited" << id << "code" << code;
        finish();
        emit exited(id, code);
    });

    m_process = process;
    m_scopeUnit = unit;
    m_scopeConfirmed = false;
    if (!unit.isEmpty() && !m_tracker)
        setActiveUnits({ unit });
    process->start();

    // start() may fail synchronously; errorOccurred has then already run
    // finish() and emitted failed().
    return m_process == process;
}

// While a scoped launch starts: is the process in its scope yet (then
// systemd-run worked), and did `flatpak run` move it to its own scope (then
// that one is recorded too, for the next session start)?
void Launcher::probeScope()
{
    if (!m_process || m_scopeUnit.isEmpty() || m_process->state() != QProcess::Running) {
        m_scopeProbe.stop();
        return;
    }
    const QString unit = acceptedScopeUnit(readProcCgroup(m_process->processId(), m_scopeUnit), m_scopeUnit,
                                           m_flatpakId);
    if (!unit.isEmpty()) {
        m_scopeConfirmed = true;
        if (!m_tracker && unit != m_scopeUnit && !m_activeUnits.contains(unit))
            setActiveUnits(m_activeUnits + QStringList { unit });
    }
    if (m_probeUntil.elapsed() >= kScopeProbeMs)
        m_scopeProbe.stop();
}

bool Launcher::terminate()
{
    // Tracked launches (Steam): the process may be the Steam client, which
    // must never be signalled; Guide uses stopTracking() for those.
    if (!m_running || m_tracker || !m_process || m_process->state() == QProcess::NotRunning)
        return false;
    if (m_terminating)
        return true;   // already closing: one SIGTERM, one escalation
    const qint64 pid = m_process->processId();
    if (pid <= 0)
        return false;
    qCInfo(lcLauncher) << "closing" << m_currentId << "on request";
    m_terminating = true;
    // Our children run in their own session: pgid == pid.
    const pid_t pgid = static_cast<pid_t>(pid);

    // Scope mode: the unit the app is in now (its own, or the one
    // `flatpak run <id>` moved it to). Every process in it gets the signal.
    QString cgroupPath;
    const QString verified = m_scopeUnit.isEmpty()
        ? QString() : acceptedScopeUnit(readProcCgroup(pid, m_scopeUnit), m_scopeUnit, m_flatpakId, &cgroupPath);
    const QString unit = verified.isEmpty() ? m_scopeUnit : verified;
    // If systemctl fails, killScope() signals the process group instead.
    const bool scoped = !unit.isEmpty() && killScope(unit, SIGTERM, false, pgid);
    // Not verifiably in a scope of ours (no scopes, or the app's process
    // is somewhere unexpected): the process group as well.
    const bool byGroup = !scoped || verified.isEmpty();
    if (byGroup)
        ::kill(-pgid, SIGTERM);
    if (scoped && byGroup)
        qCInfo(lcLauncher).noquote() << "SIGTERM to" << unit << "and its process group";
    else if (scoped)
        qCInfo(lcLauncher).noquote() << "SIGTERM to" << unit;

    // Escalation, owned by the Launcher and independent of later launches.
    // It looks again: where the process is now, whether its cgroup is still
    // populated, whether the group still exists.
    const QPointer<QProcess> process = m_process;
    QTimer::singleShot(kTerminateGraceMs, this, [this, process, pid, pgid, unit, cgroupPath] {
        QString path = cgroupPath;
        QString target = unit;
        if (process && process->state() != QProcess::NotRunning) {
            QString nowPath;
            const QString now = acceptedScopeUnit(readProcCgroup(pid, unit), unit, m_flatpakId, &nowPath);
            if (!now.isEmpty()) {
                target = now;
                path = nowPath;
            }
        }
        const bool processLeft = process && process->state() != QProcess::NotRunning;
        const bool groupLeft = ::kill(-pgid, 0) == 0;
        const bool scopeLeft = cgroupPopulated(path);
        if (!processLeft && !groupLeft && !scopeLeft)
            return;
        qCInfo(lcLauncher).noquote() << (target.isEmpty() ? QStringLiteral("process group %1").arg(pgid) : target)
                                     << "still running after SIGTERM, sending SIGKILL";
        if (!target.isEmpty())
            killScope(target, SIGKILL, false, pgid);
        if (scopeLeft)
            cgroupKill(path);
        ::kill(-pgid, SIGKILL);
        // Still there a second later (SIGKILL can't be ignored, but a
        // process can sit in uninterruptible sleep): let Close be tried again.
        QTimer::singleShot(1000, this, [this, process] {
            if (process && process == m_process && process->state() != QProcess::NotRunning) {
                qCWarning(lcLauncher) << m_currentId << "survived SIGKILL; Close can be retried";
                m_terminating = false;
            }
        });
    });
    return true;
}

QStringList Launcher::testAppCommand() const
{
    // Only our own bundled test program: predictable, no shell, and it exits
    // when its window closes (unlike single-instance editors that hand off
    // over D-Bus and exit immediately).
    const QProcessEnvironment env = buildChildEnvironment(QProcessEnvironment::systemEnvironment());
    const QString bundled = QCoreApplication::applicationDirPath() + QStringLiteral("/mightyos-testapp");
    if (!resolveProgram(bundled, env).isEmpty())
        return { bundled };
    return {};
}

void Launcher::endTracked()
{
    if (!m_tracker)
        return;
    const QString id = m_currentId;
    qCInfo(lcLauncher) << "tracked app" << id << (m_tracker->seen() ? "ended" : "did not show up");
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // E.g. the Steam client this launch started: keep it running.
        QProcess *process = m_process;
        process->disconnect(this);
        m_process = nullptr;
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
        m_background.removeAll(nullptr);
        m_background.append(process);
    }
    finish();
    emit exited(id, 0);
}

void Launcher::stopTracking()
{
    if (m_tracker && m_running)
        endTracked();
}

bool Launcher::runAuxiliary(const QStringList &argv)
{
    if (argv.isEmpty())
        return false;
    const QProcessEnvironment env = buildChildEnvironment(QProcessEnvironment::systemEnvironment());
    const QString program = resolveProgram(argv.first(), env);
    if (program.isEmpty())
        return false;
    auto *process = new QProcess(this);
    process->setProgram(program);
    process->setArguments(argv.mid(1));
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(env.value(QStringLiteral("HOME"), QDir::homePath()));
    process->setStandardInputFile(QProcess::nullDevice());
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::ResetSignalHandlers
                                      | QProcess::UnixProcessFlag::CloseFileDescriptors
                                      | QProcess::UnixProcessFlag::CreateNewSession);
    process->setChildProcessModifier([] { ProcessHardening::restoreChildLimits(); });
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    process->start();
    return true;
}

void Launcher::setActiveUnits(const QStringList &units)
{
    if (m_activeUnits == units)
        return;
    m_activeUnits = units;
    emit activeUnitsChanged();
}

void Launcher::finish()
{
    m_terminating = false;
    m_scopeUnit.clear();
    m_flatpakId.clear();
    m_scopeConfirmed = false;
    m_launchedEmitted = false;
    m_scopeProbe.stop();
    setActiveUnits({});
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (m_tracker) {
        m_tracker->disconnect(this);
        m_tracker->deleteLater();
        m_tracker = nullptr;
    }
    setWaiting(false);
    setRunning(false);
    setCurrentId(QString());
}

void Launcher::setWaiting(bool waiting)
{
    if (m_waiting == waiting)
        return;
    m_waiting = waiting;
    emit waitingChanged();
}

void Launcher::setRunning(bool running)
{
    if (m_running == running)
        return;
    m_running = running;
    emit runningChanged();
}

void Launcher::setCurrentId(const QString &id)
{
    if (m_currentId == id)
        return;
    m_currentId = id;
    emit currentIdChanged();
}
