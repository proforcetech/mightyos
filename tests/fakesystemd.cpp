// Stand-ins for systemd-run, systemctl, flatpak, labwc and mightyos
// (tst_launcher, tst_session), one binary per FAKE_TOOL.
//
// Both log their arguments (one line each, fields separated by \x1f) to
// $XDG_STATE_HOME/fake-systemd/<tool>.log; XDG_* survives Launcher's
// environment allowlist.
//  - systemd-run --user --scope ... --unit=<unit> ... -- <program> <args>:
//    records its pid under units/<unit>, then execs the program (as the
//    real one does after creating the scope).
//  - systemctl --user kill --signal=SIGTERM|SIGKILL -- <unit>...: signals
//    the process group recorded for each unit. stop: SIGTERM likewise.
//  - A file fake-systemd/fail-<tool> makes systemd-run / systemctl fail
//    (exit 1, nothing done).
//  - flatpak run [--option=value...] <app id> [command...]: execs the
//    absolute command, or waits forever.
//  - labwc -C <dir> -S <command>: sets LABWC_PID, runs the command with
//    /bin/sh -c (as labwc does) and exits with its status.
//  - mightyos ...: exits with the first line of fake-systemd/plan (then
//    drops it), 0 when empty.
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QList>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace {

QByteArray stateDir()
{
    const QByteArray base = qgetenv("XDG_STATE_HOME");
    return base.isEmpty() ? QByteArray() : base + "/fake-systemd";
}

void log(int argc, char **argv)
{
    const QByteArray dir = stateDir();
    if (dir.isEmpty() || !QDir().mkpath(QString::fromLocal8Bit(dir)))
        return;
    QFile file(QString::fromLocal8Bit(dir + "/" FAKE_TOOL ".log"));
    if (!file.open(QIODevice::Append))
        return;
    QByteArray line;
    for (int i = 1; i < argc; ++i) {
        if (i > 1)
            line += '\x1f';
        line += argv[i];
    }
    file.write(line + '\n');
}

int runScope(int argc, char **argv)
{
    QByteArray unit;
    int command = -1;
    bool user = false, scope = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            command = i + 1;
            break;
        }
        if (std::strcmp(argv[i], "--user") == 0)
            user = true;
        else if (std::strcmp(argv[i], "--scope") == 0)
            scope = true;
        else if (std::strncmp(argv[i], "--unit=", 7) == 0)
            unit = argv[i] + 7;
    }
    if (!user || !scope || unit.isEmpty() || command < 0 || command >= argc || argv[command][0] != '/')
        return 2;
    const QByteArray dir = stateDir() + "/units";
    QDir().mkpath(QString::fromLocal8Bit(dir));
    QFile pidFile(QString::fromLocal8Bit(dir + "/" + unit));
    if (pidFile.open(QIODevice::WriteOnly))
        pidFile.write(QByteArray::number(qint64(::getpid())));
    pidFile.close();
    ::execv(argv[command], argv + command);
    return 127;
}

int control(int argc, char **argv)
{
    int sig = 0;
    QList<QByteArray> units;
    bool afterVerb = false, afterDashes = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg = argv[i];
        if (!afterDashes && (arg == "kill" || arg == "stop")) {
            afterVerb = true;
            if (arg == "stop")
                sig = SIGTERM;
        } else if (arg == "--") {
            afterDashes = true;
        } else if (arg.startsWith("--signal=")) {
            sig = arg.mid(9) == "SIGKILL" ? SIGKILL : SIGTERM;
        } else if (afterVerb && (afterDashes || !arg.startsWith("-"))) {
            units.append(arg);
        }
    }
    for (const QByteArray &unit : std::as_const(units)) {
        QFile pidFile(QString::fromLocal8Bit(stateDir() + "/units/" + unit));
        if (!pidFile.open(QIODevice::ReadOnly))
            continue;
        const pid_t pid = pid_t(pidFile.readAll().trimmed().toLongLong());
        if (pid > 1 && sig != 0) {
            ::kill(-pid, sig);
            ::kill(pid, sig);
        }
    }
    return 0;
}

bool failing()
{
    return QFile::exists(QString::fromLocal8Bit(stateDir() + "/fail-" FAKE_TOOL));
}

int runFlatpak(int argc, char **argv)
{
    int i = 1;
    if (i >= argc || std::strcmp(argv[i], "run") != 0)
        return 2;
    for (++i; i < argc && argv[i][0] == '-'; ++i) {
    }
    ++i;   // the app id
    if (i < argc && argv[i][0] == '/') {
        ::execv(argv[i], argv + i);
        return 127;
    }
    for (;;)
        ::pause();
}

int runLabwc(int argc, char **argv)
{
    const char *command = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "-S") == 0)
            command = argv[i + 1];
    }
    if (!command)
        return 2;
    ::setenv("LABWC_PID", QByteArray::number(qint64(::getpid())).constData(), 1);
    const pid_t child = ::fork();
    if (child == 0) {
        ::execl("/bin/sh", "sh", "-c", command, static_cast<char *>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int runMightyos()
{
    QFile plan(QString::fromLocal8Bit(stateDir() + "/plan"));
    if (!plan.open(QIODevice::ReadOnly))
        return 0;
    QList<QByteArray> lines = plan.readAll().split('\n');
    plan.close();
    while (!lines.isEmpty() && lines.constFirst().trimmed().isEmpty())
        lines.removeFirst();
    if (lines.isEmpty())
        return 0;
    const int code = lines.takeFirst().trimmed().toInt();
    if (plan.open(QIODevice::WriteOnly | QIODevice::Truncate))
        plan.write(lines.join('\n'));
    return code;
}

} // namespace

int main(int argc, char **argv)
{
    log(argc, argv);
    if (std::strcmp(FAKE_TOOL, "flatpak") == 0)
        return runFlatpak(argc, argv);
    if (std::strcmp(FAKE_TOOL, "labwc") == 0)
        return runLabwc(argc, argv);
    if (std::strcmp(FAKE_TOOL, "mightyos") == 0)
        return runMightyos();
    if (failing())
        return 1;
    if (std::strcmp(FAKE_TOOL, "systemd-run") == 0)
        return runScope(argc, argv);
    return control(argc, argv);
}
