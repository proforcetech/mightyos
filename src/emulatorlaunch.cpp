#include "emulatorlaunch.h"

#include "ids.h"
#include "launcher.h"
#include "steamlibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <algorithm>

LaunchEnv LaunchEnv::fromEnvironment()
{
    LaunchEnv env;
    env.home = QDir::homePath();
    env.dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    env.stateHome = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    env.runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    env.user = qEnvironmentVariable("USER");
    const QProcessEnvironment child = Launcher::buildChildEnvironment(QProcessEnvironment::systemEnvironment());
    env.pathDirs = child.value(QStringLiteral("PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (!env.dataHome.isEmpty())
        env.flatpakInstallations.append(env.dataHome + QStringLiteral("/flatpak"));
    env.flatpakInstallations.append(QStringLiteral("/var/lib/flatpak"));
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!config.isEmpty())
        env.retroarchCoreDirs.append(config + QStringLiteral("/retroarch/cores"));
    env.retroarchCoreDirs << QStringLiteral("/usr/lib/x86_64-linux-gnu/libretro")
                          << QStringLiteral("/usr/lib/aarch64-linux-gnu/libretro")
                          << QStringLiteral("/usr/lib/libretro") << QStringLiteral("/usr/lib64/libretro")
                          << QStringLiteral("/usr/local/lib/libretro");
    return env;
}

QString LaunchEnv::findProgram(const QString &name) const
{
    if (name.isEmpty())
        return {};
    if (name.contains(QLatin1Char('/'))) {
        const QFileInfo info(name);
        return Ids::isCleanAbsolutePath(name) && info.isFile() && info.isExecutable() ? name : QString();
    }
    return pathDirs.isEmpty() ? QString() : QStandardPaths::findExecutable(name, pathDirs);
}

bool LaunchEnv::flatpakInstalled(const QString &appId) const
{
    if (!Ids::isValidFlatpakId(appId))
        return false;
    for (const QString &installation : flatpakInstallations) {
        if (QFileInfo(installation + QStringLiteral("/app/") + appId + QStringLiteral("/current/active/files")).isDir())
            return true;
    }
    return false;
}

namespace EmulatorLaunch {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("EmulatorLaunch", text);
}

QString retroarchFlatpakHome(const LaunchEnv &env)
{
    return env.home + QStringLiteral("/.var/app/") + kRetroArchFlatpak;
}

QString findCore(const QStringList &dirs, const QString &core)
{
    for (const QString &dir : dirs) {
        const QString path = dir + QLatin1Char('/') + core + QStringLiteral("_libretro.so");
        if (Ids::isCleanAbsolutePath(path) && QFileInfo(path).isFile())
            return path;
    }
    return {};
}

// RetroArch config values are "quoted"; our paths are built from
// validated parts, but the base dirs come from the environment.
bool cfgSafe(const QString &path)
{
    if (!Ids::isCleanAbsolutePath(path))
        return false;
    for (const QChar c : path) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\') || c == QLatin1Char('|')
            || c.category() == QChar::Other_Control)
            return false;
    }
    return true;
}

} // namespace

QStringList substitute(const QStringList &args, const QHash<QString, QString> &values)
{
    QStringList out;
    out.reserve(args.size());
    for (const QString &arg : args)
        out.append(values.contains(arg) ? values.value(arg) : arg);
    return out;
}

QString allowedRomDir(const QString &dir, const LaunchEnv &env, QString *problem)
{
    QString why;
    QString &reason = problem ? *problem : why;
    if (!Ids::isCleanAbsolutePath(dir)) {
        reason = QStringLiteral("not a clean absolute path");
        return {};
    }
    const QString canonical = QFileInfo(dir).canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) {
        reason = QStringLiteral("missing");
        return {};
    }
    if (canonical.contains(QLatin1Char(':'))) {
        reason = QStringLiteral("contains ':'");
        return {};
    }
    const auto under = [&](const QString &root) {
        return !root.isEmpty() && canonical.startsWith(root + QLatin1Char('/'));
    };
    const QString runtime = env.runtimeDir.isEmpty() ? QString() : QFileInfo(env.runtimeDir).canonicalFilePath();
    if (under(runtime) || canonical == runtime) {
        reason = QStringLiteral("inside XDG_RUNTIME_DIR");
        return {};
    }
    // A strict subfolder of `root` with no hidden component.
    const auto visibleBelow = [&](const QString &root) {
        if (!under(root))
            return false;
        const QStringList parts = canonical.mid(root.size() + 1).split(QLatin1Char('/'));
        return std::none_of(parts.cbegin(), parts.cend(), [](const QString &p) { return p.startsWith(QLatin1Char('.')); });
    };
    const QString home = env.home.isEmpty() ? QString() : QFileInfo(env.home).canonicalFilePath();
    QStringList roots { QStringLiteral("/media"), QStringLiteral("/mnt"), QStringLiteral("/srv") };
    if (!env.user.isEmpty() && !env.user.contains(QLatin1Char('/')))
        roots.append(QStringLiteral("/run/media/") + env.user);
    if (!home.isEmpty() && home != QLatin1String("/"))
        roots.prepend(home);
    for (const QString &root : std::as_const(roots)) {
        if (visibleBelow(root))
            return canonical;
    }
    reason = !home.isEmpty() && (canonical == home || under(home))
        ? QStringLiteral("the home folder itself or a hidden folder in it")
        : QStringLiteral("not under the home folder, /media, /run/media/<user>, /mnt or /srv");
    return {};
}

QStringList flatpakSandboxArgs(const SystemDef &system, const QString &romPath, bool strict, const LaunchEnv &env,
                               const QStringList &rwDirs, QStringList *rejected)
{
    QStringList args { QStringLiteral("--die-with-parent") };
    if (strict) {
        args << QStringLiteral("--nofilesystem=host:reset")
             << QStringLiteral("--filesystem=/run/udev:ro")
             << QStringLiteral("--nodevice=all") << QStringLiteral("--device=dri") << QStringLiteral("--device=input")
             << QStringLiteral("--nosocket=session-bus") << QStringLiteral("--nosocket=system-bus")
             << QStringLiteral("--nosocket=ssh-auth") << QStringLiteral("--nosocket=x11")
             << QStringLiteral("--nosocket=fallback-x11")
             << QStringLiteral("--unshare=network");
    }
    QStringList dirs;
    for (const QString &dir : system.romDirs) {
        QString problem;
        const QString allowed = allowedRomDir(dir, env, &problem);
        if (allowed.isEmpty()) {
            if (rejected && problem != QLatin1String("missing"))
                rejected->append(dir + QStringLiteral(" (") + problem + QLatin1Char(')'));
        } else if (!dirs.contains(allowed)) {
            dirs.append(allowed);
        }
    }
    const QString rom = QFileInfo(romPath).canonicalFilePath();
    const bool covered = std::any_of(dirs.cbegin(), dirs.cend(), [&](const QString &dir) {
        return (rom.isEmpty() ? romPath : rom).startsWith(dir + QLatin1Char('/'));
    });
    if (!covered) {
        const QString parent = QFileInfo(rom.isEmpty() ? romPath : rom).absolutePath();
        QString problem;
        const QString allowed = allowedRomDir(parent, env, &problem);
        if (!allowed.isEmpty() && !dirs.contains(allowed))
            dirs.append(allowed);
        else if (allowed.isEmpty() && rejected)
            rejected->append(parent + QStringLiteral(" (") + problem + QLatin1Char(')'));
    }
    for (const QString &dir : std::as_const(dirs))
        args.append(QStringLiteral("--filesystem=") + dir + QStringLiteral(":ro"));
    // Our own dirs only (a user-defined emulator's {saves}).
    const QStringList ownRoots {
        env.dataHome.isEmpty() ? QString() : env.dataHome + QStringLiteral("/mightyos/"),
        env.stateHome.isEmpty() ? QString() : env.stateHome + QStringLiteral("/mightyos/"),
    };
    for (const QString &dir : rwDirs) {
        const bool ours = Ids::isCleanAbsolutePath(dir) && !dir.contains(QLatin1Char(':'))
            && std::any_of(ownRoots.cbegin(), ownRoots.cend(),
                           [&](const QString &root) { return !root.isEmpty() && dir.startsWith(root); });
        if (ours)
            args.append(QStringLiteral("--filesystem=") + dir);
    }
    return args;
}

QString savesDir(const LaunchEnv &env, const QString &profileId, const QString &system)
{
    if (env.dataHome.isEmpty() || !Ids::isValidProfileId(profileId) || !SystemRegistry::isValidSystemId(system))
        return {};
    return env.dataHome + QStringLiteral("/mightyos/profiles/") + profileId + QStringLiteral("/saves/") + system;
}

bool available(const EmulatorDef &emulator, const LaunchEnv &env)
{
    switch (emulator.type) {
    case EmulatorDef::Type::RetroArch: {
        const bool native = !env.findProgram(QStringLiteral("retroarch")).isEmpty()
                            && !findCore(env.retroarchCoreDirs, emulator.core).isEmpty();
        const bool flatpak = env.flatpakInstalled(kRetroArchFlatpak) && !env.flatpakProgram().isEmpty()
            && !findCore({ retroarchFlatpakHome(env) + QStringLiteral("/config/retroarch/cores") }, emulator.core).isEmpty();
        return native || flatpak;
    }
    case EmulatorDef::Type::Flatpak:
        return env.flatpakInstalled(emulator.app) && !env.flatpakProgram().isEmpty();
    case EmulatorDef::Type::Native:
        return !env.findProgram(emulator.program).isEmpty();
    }
    return false;
}

LaunchPlan plan(const SystemDef &system, const EmulatorDef &emulator, const QString &romPath,
                const QString &profileId, const LaunchEnv &env, bool restricted)
{
    LaunchPlan p;
    p.emulatorId = emulator.id;
    if (!Ids::isCleanAbsolutePath(romPath) || !Ids::isValidProfileId(profileId)) {
        p.error = tr("This game's file can't be used.");
        return p;
    }
    const QString saves = savesDir(env, profileId, system.id);

    if (emulator.type == EmulatorDef::Type::RetroArch) {
        const QString nativeProgram = env.findProgram(QStringLiteral("retroarch"));
        const QString nativeCore = nativeProgram.isEmpty() ? QString() : findCore(env.retroarchCoreDirs, emulator.core);
        const bool flatpakRa = env.flatpakInstalled(kRetroArchFlatpak) && !env.flatpakProgram().isEmpty();
        const QString raHome = retroarchFlatpakHome(env);
        const QString flatpakCore = flatpakRa
            ? findCore({ raHome + QStringLiteral("/config/retroarch/cores") }, emulator.core) : QString();
        // The cfg is ours: $XDG_STATE_HOME/mightyos/profiles/<id>/retroarch/<system>.cfg.
        const QStringList cfgSubdirs { QStringLiteral("mightyos"), QStringLiteral("profiles"), profileId,
                                       QStringLiteral("retroarch") };
        const QString cfgDir = env.stateHome.isEmpty() ? QString() : env.stateHome + QLatin1Char('/') + cfgSubdirs.join(QLatin1Char('/'));
        const QString cfg = cfgDir.isEmpty() ? QString() : cfgDir + QLatin1Char('/') + system.id + QStringLiteral(".cfg");
        QString saveBase;
        QStringList fsArg;
        if (!nativeCore.isEmpty()) {
            p.argv = { nativeProgram, QStringLiteral("-L"), nativeCore };
            saveBase = saves;
        } else if (!flatpakCore.isEmpty()) {
            if (cfgSafe(cfgDir))
                fsArg = { QStringLiteral("--filesystem=") + cfgDir + QStringLiteral(":ro") };
            p.argv = QStringList { env.flatpakProgram(), QStringLiteral("run") }
                     + flatpakSandboxArgs(system, romPath, !emulator.appSandbox, env) + fsArg
                     + QStringList { kRetroArchFlatpak, QStringLiteral("-L"), flatpakCore };
            saveBase = raHome + QStringLiteral("/data/mightyos/profiles/") + profileId + QStringLiteral("/saves/") + system.id;
        } else if (!nativeProgram.isEmpty() || flatpakRa) {
            p.error = tr("RetroArch is missing the %1 core. Open RetroArch and install it under "
                         "Online Updater > Core Downloader.").arg(emulator.core);
            return p;
        } else {
            p.error = tr("Install RetroArch from the App Store to play %1 games.").arg(system.label);
            p.install = QStringLiteral("RetroArch");
            return p;
        }
        if (cfgSafe(saveBase) && cfgSafe(cfg)) {
            p.configPath = cfg;
            p.configBase = env.stateHome;
            p.configSubdirs = cfgSubdirs;
            p.configName = system.id + QStringLiteral(".cfg");
            QByteArray c = QByteArrayLiteral("# Written by MightyOS for each launch; edits are overwritten.\n")
                + "video_fullscreen = \"true\"\n"
                + "savefile_directory = \"" + saveBase.toUtf8() + "\"\n"
                + "savestate_directory = \"" + saveBase.toUtf8() + "/states\"\n"
                // PLAN section 4: emulator network features off by default.
                + "network_cmd_enable = \"false\"\n"
                + "stdin_cmd_enable = \"false\"\n"
                + "network_remote_enable = \"false\"\n"
                + "netplay_public_announce = \"false\"\n"
                + "discord_allow = \"false\"\n";
            if (restricted) {
                c += "kiosk_mode_enable = \"true\"\n"
                     "menu_show_online_updater = \"false\"\n"
                     "menu_show_core_updater = \"false\"\n"
                     "menu_show_load_core = \"false\"\n"
                     "menu_show_load_content = \"false\"\n"
                     "content_show_netplay = \"false\"\n"
                     "menu_show_quit_retroarch = \"true\"\n";
            }
            p.configContent = c;
            p.configArgv = fsArg + QStringList { QStringLiteral("--appendconfig"), cfg };
            p.argv << QStringLiteral("--appendconfig") << cfg;
        } else {
            for (const QString &a : std::as_const(fsArg))
                p.argv.removeOne(a);
        }
        p.argv << romPath;
        return p;
    }

    // {appdata}: a per-profile dir in the Flatpak's own data dir (the
    // sandbox can write it; the emulator creates it); natively the saves dir.
    const QString appdata = emulator.type == EmulatorDef::Type::Flatpak
        ? env.home + QStringLiteral("/.var/app/") + emulator.app + QStringLiteral("/data/mightyos/profiles/") + profileId
        : saves;
    const QHash<QString, QString> values {
        { QStringLiteral("{rom}"), romPath },
        { QStringLiteral("{saves}"), saves },
        { QStringLiteral("{appdata}"), appdata },
    };
    if ((emulator.args.contains(QStringLiteral("{saves}")) && saves.isEmpty())
        || (emulator.args.contains(QStringLiteral("{appdata}")) && (env.home.isEmpty() || !Ids::isCleanAbsolutePath(appdata)))) {
        p.error = tr("No save folder is available for this profile.");
        return p;
    }
    if (emulator.type == EmulatorDef::Type::Flatpak) {
        const QString flatpak = env.flatpakProgram();
        if (flatpak.isEmpty() || !env.flatpakInstalled(emulator.app)) {
            p.error = tr("Install %1 from the App Store to play this game.").arg(emulator.label);
            p.install = emulator.label;
            return p;
        }
        // A {saves} dir is ours (data dir): it must be writable inside.
        const QStringList rw = emulator.args.contains(QStringLiteral("{saves}")) ? QStringList { saves } : QStringList();
        if (!rw.isEmpty())
            QDir().mkpath(saves);
        p.argv = QStringList { flatpak, QStringLiteral("run") }
                 + flatpakSandboxArgs(system, romPath, !emulator.appSandbox, env, rw)
                 + QStringList { emulator.app } + substitute(emulator.args, values);
        return p;
    }
    const QString program = env.findProgram(emulator.program);
    if (program.isEmpty()) {
        p.error = tr("Install %1 to play this game.").arg(emulator.label);
        p.install = emulator.label;
        return p;
    }
    p.argv = QStringList { program } + substitute(emulator.args, values);
    return p;
}

QStringList steamShutdownArgv(bool flatpakSteam, const LaunchEnv &env)
{
    const QString native = env.findProgram(QStringLiteral("steam"));
    if ((flatpakSteam || native.isEmpty()) && env.flatpakInstalled(SteamLibrary::kFlatpakId)
        && !env.flatpakProgram().isEmpty())
        return { env.flatpakProgram(), QStringLiteral("run"), SteamLibrary::kFlatpakId, QStringLiteral("-shutdown") };
    if (!native.isEmpty())
        return { native, QStringLiteral("-shutdown") };
    return {};
}

LaunchPlan planSteam(const QString &appid, bool flatpakSteam, const LaunchEnv &env)
{
    LaunchPlan p;
    p.emulatorId = QStringLiteral("steam");
    if (!SteamLibrary::isValidAppId(appid)) {
        p.error = tr("This Steam game can't be launched.");
        return p;
    }
    const QString url = QStringLiteral("steam://rungameid/") + appid;
    const QString native = env.findProgram(QStringLiteral("steam"));
    const bool flatpak = env.flatpakInstalled(SteamLibrary::kFlatpakId) && !env.flatpakProgram().isEmpty();
    if ((flatpakSteam || native.isEmpty()) && flatpak) {
        p.argv = { env.flatpakProgram(), QStringLiteral("run"), SteamLibrary::kFlatpakId, url };
    } else if (!native.isEmpty()) {
        p.argv = { native, url };
    } else {
        p.error = tr("Install Steam from the App Store to play this game.");
        p.install = QStringLiteral("Steam");
    }
    return p;
}

} // namespace EmulatorLaunch
