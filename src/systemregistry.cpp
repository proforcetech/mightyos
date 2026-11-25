#include "systemregistry.h"

#include <QFile>
#include <QFileInfo>
#include <sys/stat.h>

#include "definitionlayers.h"
#include "ids.h"
#include "textsanitize.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>


Q_LOGGING_CATEGORY(lcSystems, "mightyos.systems", QtWarningMsg)

const EmulatorDef *SystemDef::emulator(const QString &id) const
{
    for (const EmulatorDef &e : emulators) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

namespace {

const QStringList kPlaceholders { QStringLiteral("{rom}"), QStringLiteral("{saves}"), QStringLiteral("{appdata}") };

// Compiled once per pattern (the patterns are string literals).
bool matches(const char *pattern, const QString &text)
{
    static QMutex mutex;
    static QHash<const char *, QRegularExpression> cache;
    QRegularExpression re;
    {
        QMutexLocker lock(&mutex);
        auto it = cache.find(pattern);
        if (it == cache.end())
            it = cache.insert(pattern, QRegularExpression(QString::fromLatin1(pattern)));
        re = it.value();
    }
    return re.match(text).hasMatch();
}

// A display string: 1..maxChars, no control/bidi characters (the
// sanitised text must equal the input).
bool isCleanLabel(const QString &text, int maxChars, bool allowEmpty = false)
{
    if (text.isEmpty())
        return allowEmpty;
    return text.size() <= maxChars && TextSanitize::label(text, maxChars) == text;
}

QString expandHome(const QString &path, const QString &home)
{
    if (path == QLatin1String("~"))
        return home;
    if (path.startsWith(QLatin1String("~/")))
        return home + path.mid(1);
    return path;
}

bool keysAllowed(const QJsonObject &o, std::initializer_list<const char *> allowed, QString *bad)
{
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool ok = false;
        for (const char *a : allowed)
            ok = ok || it.key() == QLatin1String(a);
        if (!ok) {
            *bad = it.key();
            return false;
        }
    }
    return true;
}

bool stringList(const QJsonValue &v, QStringList *out, int maxItems)
{
    if (!v.isArray() || v.toArray().size() > maxItems)
        return false;
    for (const QJsonValue &item : v.toArray()) {
        if (!item.isString())
            return false;
        out->append(item.toString());
    }
    return true;
}

} // namespace

bool SystemRegistry::isValidSystemId(const QString &id)
{
    return matches("^[a-z0-9][a-z0-9_-]{0,31}$", id);
}

bool SystemRegistry::isValidArgTemplate(const QStringList &args, bool needsRom)
{
    if (args.size() > kMaxArgs)
        return false;
    int roms = 0;
    for (const QString &arg : args) {
        if (arg.size() > 512 || arg.contains(QChar(0)))
            return false;
        for (const QChar c : arg) {
            if (c.category() == QChar::Other_Control)
                return false;
        }
        if (arg.contains(QLatin1Char('{')) || arg.contains(QLatin1Char('}'))) {
            if (!kPlaceholders.contains(arg))
                return false;   // only whole-element placeholders
        }
        if (arg == QLatin1String("{rom}"))
            ++roms;
    }
    return needsRom ? roms == 1 : roms <= 1;
}

std::optional<SystemDef> SystemRegistry::parse(const QByteArray &json, const QString &home, QString *error,
                                               bool *disabled)
{
    auto fail = [error](const QString &why) -> std::optional<SystemDef> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    if (disabled)
        *disabled = false;
    if (json.size() > kMaxFileBytes)
        return fail(QStringLiteral("file too large"));
    QJsonParseError parseError {};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("not a JSON object"));
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("version")).toInt(-1) != kVersion || !o.value(QStringLiteral("version")).isDouble())
        return fail(QStringLiteral("unsupported version"));

    SystemDef s;
    s.id = o.value(QStringLiteral("id")).toString();
    if (!isValidSystemId(s.id))
        return fail(QStringLiteral("invalid id"));

    if (o.contains(QStringLiteral("disabled"))) {
        QString bad;
        if (!keysAllowed(o, { "version", "id", "disabled" }, &bad) || !o.value(QStringLiteral("disabled")).isBool())
            return fail(QStringLiteral("a disabled stub has only version, id and disabled"));
        if (o.value(QStringLiteral("disabled")).toBool()) {
            if (disabled)
                *disabled = true;
            return s;
        }
        return fail(QStringLiteral("\"disabled\": false is not a definition"));
    }

    QString bad;
    if (!keysAllowed(o, { "version", "id", "label", "fullName", "category", "kind", "order", "icon", "tint",
                          "extensions", "romDirs", "emulators", "defaultEmulator", "comment" }, &bad))
        return fail(QStringLiteral("unknown key \"%1\"").arg(bad.left(40)));

    const auto str = [&o](const char *key) { return o.value(QLatin1String(key)); };
    for (const char *key : { "label", "fullName", "category", "kind", "icon", "tint", "defaultEmulator", "comment" }) {
        if (o.contains(QLatin1String(key)) && !str(key).isString())
            return fail(QStringLiteral("\"%1\" must be a string").arg(QLatin1String(key)));
    }
    s.label = str("label").toString();
    s.fullName = str("fullName").toString(s.label);
    s.category = str("category").toString();
    const QString kind = str("kind").toString(QStringLiteral("roms"));
    s.icon = str("icon").toString(QStringLiteral("emulators"));
    s.tint = str("tint").toString();
    if (!isCleanLabel(s.label, 24))
        return fail(QStringLiteral("invalid label"));
    if (!isCleanLabel(s.fullName, 64))
        return fail(QStringLiteral("invalid fullName"));
    if (s.category != QLatin1String("emulators") && s.category != QLatin1String("games"))
        return fail(QStringLiteral("category must be \"emulators\" or \"games\""));
    if (kind == QLatin1String("roms"))
        s.kind = SystemDef::Kind::Roms;
    else if (kind == QLatin1String("steam"))
        s.kind = SystemDef::Kind::Steam;
    else
        return fail(QStringLiteral("kind must be \"roms\" or \"steam\""));
    if (!matches("^[a-z0-9-]{1,32}$", s.icon))
        return fail(QStringLiteral("invalid icon"));
    if (!s.tint.isEmpty() && !matches("^#[0-9a-fA-F]{6}$", s.tint))
        return fail(QStringLiteral("invalid tint"));
    if (o.contains(QStringLiteral("order"))) {
        const QJsonValue order = o.value(QStringLiteral("order"));
        if (!order.isDouble() || order.toDouble() != double(order.toInt()) || order.toInt() < 0
            || order.toInt() > 100000)
            return fail(QStringLiteral("order must be an integer 0..100000"));
        s.order = order.toInt();
    }

    if (s.kind == SystemDef::Kind::Steam) {
        for (const char *key : { "extensions", "romDirs", "emulators", "defaultEmulator" }) {
            if (o.contains(QLatin1String(key)))
                return fail(QStringLiteral("a steam system has no \"%1\"").arg(QLatin1String(key)));
        }
        return s;
    }

    QStringList extensions;
    if (!stringList(str("extensions"), &extensions, kMaxExtensions) || extensions.isEmpty())
        return fail(QStringLiteral("extensions: 1..%1 strings").arg(kMaxExtensions));
    for (const QString &ext : std::as_const(extensions)) {
        const QString lower = ext.toLower();
        if (!matches("^[a-z0-9]{1,8}$", lower))
            return fail(QStringLiteral("invalid extension"));
        if (!s.extensions.contains(lower))
            s.extensions.append(lower);
    }

    QStringList dirs;
    if (o.contains(QStringLiteral("romDirs"))) {
        if (!stringList(str("romDirs"), &dirs, kMaxRomDirs) || dirs.isEmpty())
            return fail(QStringLiteral("romDirs: 1..%1 strings").arg(kMaxRomDirs));
    } else {
        dirs = { QStringLiteral("~/ROMs/") + s.id };
    }
    for (const QString &dir : std::as_const(dirs)) {
        QString path = expandHome(dir, home);
        while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
            path.chop(1);
        if (!Ids::isCleanAbsolutePath(path) || path == QLatin1String("/"))
            return fail(QStringLiteral("romDirs must be clean absolute paths or ~/..."));
        if (!s.romDirs.contains(path))
            s.romDirs.append(path);
    }

    const QJsonValue emulators = str("emulators");
    if (!emulators.isUndefined()) {
        if (!emulators.isArray() || emulators.toArray().size() > kMaxEmulators)
            return fail(QStringLiteral("emulators: at most %1").arg(kMaxEmulators));
        QSet<QString> ids;
        for (const QJsonValue &value : emulators.toArray()) {
            if (!value.isObject())
                return fail(QStringLiteral("emulators: objects only"));
            const QJsonObject e = value.toObject();
            if (!keysAllowed(e, { "id", "label", "type", "core", "app", "program", "args", "sandbox" }, &bad))
                return fail(QStringLiteral("emulator: unknown key \"%1\"").arg(bad.left(40)));
            for (const char *key : { "id", "label", "type", "core", "app", "program", "sandbox" }) {
                if (e.contains(QLatin1String(key)) && !e.value(QLatin1String(key)).isString())
                    return fail(QStringLiteral("emulator: \"%1\" must be a string").arg(QLatin1String(key)));
            }
            EmulatorDef def;
            def.id = e.value(QStringLiteral("id")).toString();
            def.label = e.value(QStringLiteral("label")).toString();
            const QString type = e.value(QStringLiteral("type")).toString();
            if (!isValidSystemId(def.id) || ids.contains(def.id))
                return fail(QStringLiteral("emulator: invalid or duplicate id"));
            if (!isCleanLabel(def.label, 48))
                return fail(QStringLiteral("emulator: invalid label"));
            ids.insert(def.id);
            const QString sandbox = e.value(QStringLiteral("sandbox")).toString(QStringLiteral("strict"));
            if (sandbox != QLatin1String("strict") && sandbox != QLatin1String("app"))
                return fail(QStringLiteral("emulator: sandbox must be \"strict\" or \"app\""));
            if (e.contains(QStringLiteral("sandbox")) && type == QLatin1String("native"))
                return fail(QStringLiteral("native emulator: no \"sandbox\" (Flatpak only)"));
            def.appSandbox = sandbox == QLatin1String("app");
            const bool hasArgs = e.contains(QStringLiteral("args"));
            if (hasArgs && !stringList(e.value(QStringLiteral("args")), &def.args, kMaxArgs))
                return fail(QStringLiteral("emulator: args must be strings"));
            if (type == QLatin1String("retroarch")) {
                def.type = EmulatorDef::Type::RetroArch;
                def.core = e.value(QStringLiteral("core")).toString();
                if (!matches("^[a-z0-9_]{1,64}$", def.core) || e.contains(QStringLiteral("app"))
                    || e.contains(QStringLiteral("program")) || hasArgs)
                    return fail(QStringLiteral("retroarch emulator: a valid \"core\" only"));
            } else if (type == QLatin1String("flatpak")) {
                def.type = EmulatorDef::Type::Flatpak;
                def.app = e.value(QStringLiteral("app")).toString();
                if (!Ids::isValidFlatpakId(def.app) || e.contains(QStringLiteral("core"))
                    || e.contains(QStringLiteral("program")) || !isValidArgTemplate(def.args, true))
                    return fail(QStringLiteral("flatpak emulator: a valid \"app\" and args with {rom}"));
            } else if (type == QLatin1String("native")) {
                def.type = EmulatorDef::Type::Native;
                def.program = e.value(QStringLiteral("program")).toString();
                const bool programOk = def.program.contains(QLatin1Char('/'))
                    ? Ids::isCleanAbsolutePath(def.program)
                    : matches("^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$", def.program);
                if (!programOk || e.contains(QStringLiteral("core")) || e.contains(QStringLiteral("app"))
                    || !isValidArgTemplate(def.args, true))
                    return fail(QStringLiteral("native emulator: a valid \"program\" and args with {rom}"));
            } else {
                return fail(QStringLiteral("emulator type must be retroarch, flatpak or native"));
            }
            s.emulators.append(def);
        }
    }
    s.defaultEmulator = str("defaultEmulator").toString();
    if (s.defaultEmulator.isEmpty() && !s.emulators.isEmpty())
        s.defaultEmulator = s.emulators.constFirst().id;
    if (!s.defaultEmulator.isEmpty() && !s.emulator(s.defaultEmulator))
        return fail(QStringLiteral("defaultEmulator is not one of the emulators"));
    return s;
}

bool SystemRegistry::sandboxLayerTrusted(const QString &dir, const QString &file, const QString &home)
{
    if (dir == kBuiltinDir)
        return true;
    const QString canonicalDir = QFileInfo(dir).canonicalFilePath();
    const QString canonicalHome = home.isEmpty() ? QString() : QFileInfo(home).canonicalFilePath();
    if (canonicalDir.isEmpty()
        || (!canonicalHome.isEmpty() && (canonicalDir == canonicalHome || canonicalDir.startsWith(canonicalHome + QLatin1Char('/')))))
        return false;
    struct stat d {};
    struct stat f {};
    return ::stat(QFile::encodeName(canonicalDir).constData(), &d) == 0 && d.st_uid == 0 && !(d.st_mode & (S_IWGRP | S_IWOTH))
        && ::lstat(QFile::encodeName(file).constData(), &f) == 0 && S_ISREG(f.st_mode) && f.st_uid == 0
        && !(f.st_mode & (S_IWGRP | S_IWOTH));
}

QStringList SystemRegistry::defaultDirs()
{
    return DefinitionLayers::defaultDirs(kBuiltinDir, QStringLiteral("systems.d"));
}

void SystemRegistry::load(const QStringList &dirs, const QString &home)
{
    QMap<QString, SystemDef> byId;
    m_errors.clear();
    for (const QString &dir : dirs) {
        const DefinitionLayers::Layer layer = DefinitionLayers::read(dir, kMaxFilesPerDir, kMaxFileBytes);
        if (!layer.dirError.isEmpty()) {
            const QString message = QStringLiteral("%1: %2").arg(dir, layer.dirError);
            m_errors.append(message);
            qCWarning(lcSystems).noquote() << "ignoring system definitions in" << message;
            continue;
        }
        for (const DefinitionLayers::File &file : layer.files) {
            QString error = file.error;
            bool disabled = false;
            std::optional<SystemDef> def;
            if (file.data)
                def = parse(*file.data, home, &error, &disabled);
            if (def && file.name != def->id + QStringLiteral(".json")) {
                def.reset();
                error = QStringLiteral("file name must be <id>.json");
            }
            if (!def) {
                const QString message = QStringLiteral("%1: %2").arg(file.path, error);
                m_errors.append(message);
                qCWarning(lcSystems).noquote() << "ignoring system definition" << message;
                continue;
            }
            if (disabled) {
                byId.remove(def->id);
                continue;
            }
            def->source = file.path;
            // "sandbox": "app" weakens the Flatpak sandbox: only from the
            // built-in layer or a root-owned file in a root-owned dir outside
            // $HOME (/usr/share/mightyos, /etc/...), never the user's layer.
            if (!sandboxLayerTrusted(dir, file.path, home)) {
                for (EmulatorDef &emulator : def->emulators) {
                    if (emulator.appSandbox) {
                        qCWarning(lcSystems).noquote() << file.path << ": \"sandbox\": \"app\" ignored for"
                                                       << emulator.id << "(only root-owned definitions may relax it)";
                        emulator.appSandbox = false;
                    }
                }
            }
            if (const auto previous = byId.constFind(def->id); previous != byId.cend())
                qCWarning(lcSystems).noquote() << file.path << "overrides the definition of" << def->id
                                               << "from" << previous->source;
            byId.insert(def->id, *def);
        }
    }
    m_systems = byId.values();
    std::stable_sort(m_systems.begin(), m_systems.end(), [](const SystemDef &a, const SystemDef &b) {
        return a.order != b.order ? a.order < b.order : a.label.compare(b.label, Qt::CaseInsensitive) < 0;
    });
    m_index.clear();
    for (int i = 0; i < m_systems.size(); ++i)
        m_index.insert(m_systems.at(i).id, i);
}

const SystemDef *SystemRegistry::find(const QString &id) const
{
    const auto found = m_index.constFind(id);
    return found == m_index.cend() ? nullptr : &m_systems.at(found.value());
}
