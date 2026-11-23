#include "launcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <csignal>
#include <sys/prctl.h>
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

} // namespace

Launcher::Launcher(QObject *parent)
    : QObject(parent)
{
}

Launcher::~Launcher()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // Leaving the launcher takes the running app down with it. M6 moves
        // apps into their own systemd scopes, which changes this policy.
        m_process->disconnect(this);
        signalGroup(m_process, SIGTERM);
        if (!m_process->waitForFinished(1500)) {
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

    auto *process = new QProcess(this);
    process->setProgram(program);
    process->setArguments(argv.mid(1));
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(env.value(QStringLiteral("HOME"), QDir::homePath()));
    process->setStandardInputFile(QProcess::nullDevice());
    // Child output goes to our stdout/stderr for now (dev visibility).
    // M6 routes it to the journal via systemd-run.
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    // Clean slate for the child: default signal handlers, no inherited fds,
    // its own session/process group.
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::ResetSignalHandlers
                                      | QProcess::UnixProcessFlag::CloseFileDescriptors
                                      | QProcess::UnixProcessFlag::CreateNewSession);
    // If MightyOS dies abnormally (crash, SIGKILL) the child gets SIGTERM
    // instead of being orphaned. M6 replaces this with systemd scopes.
    process->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });

    connect(process, &QProcess::started, this, [this, process] {
        if (process != m_process)
            return;
        qCInfo(lcLauncher) << "started" << m_currentId << "pid" << process->processId();
        emit launched(m_currentId);
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
        const int code = (status == QProcess::CrashExit) ? -1 : exitCode;
        qCInfo(lcLauncher) << "exited" << id << "code" << code;
        finish();
        emit exited(id, code);
    });

    m_process = process;
    setCurrentId(id);
    setRunning(true);

    qCInfo(lcLauncher) << "launching" << id;
    process->start();

    // start() may fail synchronously; errorOccurred has then already run
    // finish() and emitted failed().
    return m_process == process;
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

void Launcher::finish()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    setRunning(false);
    setCurrentId(QString());
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
