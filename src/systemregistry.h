#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

// System definitions (PLAN M4): one JSON file per system in systems.d.
//
// Layers, lowest priority first (a later file with the same id replaces
// the earlier one completely; { "version": 1, "id": "x", "disabled": true }
// hides a system):
//   1. built in (qrc :/mightyos/systems.d, a copy of data/systems.d),
//   2. $XDG_DATA_DIRS/mightyos/systems.d (installed: /usr/share/...),
//   3. $XDG_CONFIG_HOME/mightyos/systems.d (the user's overrides).
//
// File format (version 1), validated strictly: unknown keys, wrong types or
// any invalid field reject the whole file (logged with the file name, the
// previous layer's definition stays):
//   { "version": 1, "id": "snes", "label": "SNES",
//     "fullName": "Super Nintendo", "category": "emulators" | "games",
//     "kind": "roms" (default) | "steam", "order": 20,
//     "icon": "cartridge", "tint": "#9b8cff",
//     "extensions": ["sfc", "smc", "zip"],
//     "romDirs": ["~/ROMs/snes"],                  (default ~/ROMs/<id>)
//     "emulators": [
//       { "id": "snes9x", "label": "Snes9x", "type": "retroarch", "core": "snes9x" },
//       { "id": "dolphin", "label": "Dolphin", "type": "flatpak",
//         "app": "org.DolphinEmu.dolphin-emu", "args": ["-b", "-e", "{rom}"] },
//       { "id": "x", "label": "X", "type": "native", "program": "x", "args": ["{rom}"] } ],
//     "defaultEmulator": "snes9x" }
//
// Argument templates: placeholders are whole argv elements, never parts of
// one ("{rom}", "{saves}", "{appdata}"); an element containing '{' or '}' that is not
// exactly a placeholder rejects the emulator. "{rom}" must appear exactly
// once. Nothing is ever passed through a shell.
//
// Ids are the only way to name a system from outside (navstate params,
// favorites): find() looks them up here and never treats them as paths.
struct EmulatorDef {
    enum class Type { RetroArch, Flatpak, Native };
    QString id;
    QString label;
    Type type = Type::RetroArch;
    QString core;      // RetroArch: libretro core name ("snes9x")
    QString app;       // Flatpak: application id
    QString program;   // Native: bare name (PATH) or clean absolute path
    QStringList args;  // Flatpak / Native: argv template after the program
};

struct SystemDef {
    enum class Kind { Roms, Steam };
    QString id;
    QString label;
    QString fullName;
    QString category;
    Kind kind = Kind::Roms;
    int order = 1000;
    QString icon;
    QString tint;
    QStringList extensions;   // lower case, no dot
    QStringList romDirs;      // absolute, clean (~ expanded)
    QList<EmulatorDef> emulators;
    QString defaultEmulator;
    QString source;           // file it came from (diagnostics)

    const EmulatorDef *emulator(const QString &id) const;
};

class SystemRegistry
{
public:
    static constexpr int kVersion = 1;
    static constexpr qint64 kMaxFileBytes = 64 * 1024;
    static constexpr int kMaxFilesPerDir = 256;
    static constexpr int kMaxEmulators = 8;
    static constexpr int kMaxExtensions = 32;
    static constexpr int kMaxRomDirs = 8;
    static constexpr int kMaxArgs = 32;
    static inline const QString kBuiltinDir = QStringLiteral(":/mightyos/systems.d");

    // Default layers from the environment (see above).
    static QStringList defaultDirs();

    // Loads `dirs` (lowest priority first). `home` expands "~/".
    void load(const QStringList &dirs, const QString &home);

    // Parses one definition. nullopt + `*error` if invalid. A "disabled"
    // stub returns a SystemDef with an empty label and sets `*disabled`.
    static std::optional<SystemDef> parse(const QByteArray &json, const QString &home,
                                          QString *error = nullptr, bool *disabled = nullptr);

    static bool isValidSystemId(const QString &id);
    // "{rom}" etc., and the template check described above.
    static bool isValidArgTemplate(const QStringList &args, bool needsRom);

    const QList<SystemDef> &systems() const { return m_systems; }   // sorted (order, label)
    const SystemDef *find(const QString &id) const;
    QStringList errors() const { return m_errors; }

private:
    QList<SystemDef> m_systems;
    QHash<QString, int> m_index;
    QStringList m_errors;
};
