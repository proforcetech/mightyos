#pragma once

#include "systemregistry.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

// Where emulators and their pieces are looked for. fromEnvironment() for
// the app; tests build their own.
struct LaunchEnv {
    QString home;
    QString dataHome;                    // $XDG_DATA_HOME
    QString stateHome;                   // $XDG_STATE_HOME
    QStringList pathDirs;                // the *sanitised* child PATH
    QStringList flatpakInstallations;    // ~/.local/share/flatpak, /var/lib/flatpak
    QStringList retroarchCoreDirs;       // native RetroArch cores

    static LaunchEnv fromEnvironment();
    QString findProgram(const QString &name) const;
    bool flatpakInstalled(const QString &appId) const;
    QString flatpakProgram() const { return findProgram(QStringLiteral("flatpak")); }
};

// The result of planning one launch. Either `argv` is set, or `error` is a
// user-facing message; `install` then names what to install from the App
// Store ("RetroArch", "Dolphin", "Steam"), or is empty for other problems.
struct LaunchPlan {
    QStringList argv;
    QString error;
    QString install;
    QString emulatorId;
    // A RetroArch --appendconfig file to write before launching, with
    // JsonFile::writePrivate(configBase, configSubdirs, configName) (0600,
    // never through a symlink). configArgv are the argv elements that only
    // make sense with that file: drop them if writing it fails.
    QString configPath;
    QString configBase;
    QStringList configSubdirs;
    QString configName;
    QByteArray configContent;
    QStringList configArgv;

    bool ok() const { return !argv.isEmpty(); }
};

// Builds argv lists, never command strings (PLAN section 4):
//  - RetroArch: native `retroarch` from PATH with a core from the native
//    core dirs, else Flatpak org.libretro.RetroArch with a core from its
//    own config dir: [retroarch | flatpak run --filesystem=<cfgdir>:ro
//    org.libretro.RetroArch] -L <core.so> --appendconfig <cfg> <rom>.
//    The cfg lives in $XDG_STATE_HOME/mightyos/profiles/<id>/retroarch/
//    (never in the sandbox-writable ~/.var/app tree; the Flatpak gets it
//    read-only). It sets fullscreen, the per-profile save and state dirs
//    (Flatpak: inside its own ~/.var/app dir, which the sandbox can
//    write), turns network commands, stdin commands and netplay
//    announcing off (PLAN section 4) and, for kids profiles, enables kiosk
//    mode and hides the online/core updaters, Load Core and Load Content.
//  - Flatpak: flatpak run <app> <args with {rom}/{saves}/{appdata}
//    substituted>. {appdata} is a per-profile dir inside the app's own
//    ~/.var/app/<app>/data (writable by the sandbox), for emulators with a
//    "user folder" option (Dolphin -u).
//  - Native: <program from PATH or absolute> <args>.
// Placeholders are replaced only as whole elements. The ROM path is the
// verified absolute path, so it can't be mistaken for an option.
namespace EmulatorLaunch {

static inline const QString kRetroArchFlatpak = QStringLiteral("org.libretro.RetroArch");

QStringList substitute(const QStringList &args, const QHash<QString, QString> &values);

// Whether `emulator` could launch right now (for menus).
bool available(const EmulatorDef &emulator, const LaunchEnv &env);

// `profileId` must be a valid profile id (it becomes a path component).
// `restricted`: a kids profile (stricter RetroArch config).
LaunchPlan plan(const SystemDef &system, const EmulatorDef &emulator, const QString &romPath,
                const QString &profileId, const LaunchEnv &env, bool restricted = false);

// `steam -shutdown` for the client that a launch used (kids profiles: the
// Steam client is unrestricted once it runs, so it is closed after the game).
QStringList steamShutdownArgv(bool flatpakSteam, const LaunchEnv &env);

LaunchPlan planSteam(const QString &appid, bool flatpakSteam, const LaunchEnv &env);

// Per-profile save directory for `system` (native emulators).
QString savesDir(const LaunchEnv &env, const QString &profileId, const QString &system);

} // namespace EmulatorLaunch
