#include "appscanner.h"

#include "iconlocator.h"
#include "ids.h"
#include "jsonfile.h"
#include "launcher.h"
#include "tilelistmodel.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QLoggingCategory>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>

Q_LOGGING_CATEGORY(lcAppScan, "mightyos.apps", QtWarningMsg)

namespace {

constexpr int kMaxTitle = 100;
constexpr int kMaxSubtitle = 120;

QString cleanText(const QString &text, int max)
{
    QString out;
    for (const QChar c : text) {
        if (c.category() != QChar::Other_Control)
            out.append(c);
    }
    out = out.simplified();
    if (out.size() > max)
        out = out.left(max - 1) + QChar(0x2026);
    return out;
}

void addDir(QStringList &list, const QString &dir)
{
    // Spec: relative entries in XDG_DATA_DIRS are invalid and ignored.
    if (!QDir::isAbsolutePath(dir))
        return;
    const QString clean = QDir::cleanPath(dir);
    if (!list.contains(clean))
        list.append(clean);
}

bool programResolves(const QString &program, const AppScanConfig &config)
{
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), config.path);
    return !Launcher::resolveProgram(program, env).isEmpty();
}

bool cancelled(const std::atomic_bool *cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

// Nearest existing ancestor (at most 3 levels up), so a later mkdir of a
// missing dir (first Flatpak install) still triggers a re-scan.
QString watchTargetFor(const QString &dir)
{
    QString current = dir;
    for (int up = 0; up <= 3; ++up) {
        if (QFileInfo(current).isDir())
            return current;
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current)
            break;
        current = parent;
    }
    return {};
}

} // namespace

AppScanConfig AppScanConfig::fromEnvironment(const QString &systemManifestDir)
{
    AppScanConfig config;
    const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString userFlatpak = QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share");
    const QString systemFlatpak = QStringLiteral("/var/lib/flatpak/exports/share");

    QStringList dataDirs;
    addDir(dataDirs, dataHome);
    addDir(dataDirs, userFlatpak);
    addDir(dataDirs, systemFlatpak);
    QString env = qEnvironmentVariable("XDG_DATA_DIRS");
    if (env.isEmpty())
        env = QStringLiteral("/usr/local/share:/usr/share");
    for (const QString &dir : env.split(QLatin1Char(':'), Qt::SkipEmptyParts))
        addDir(dataDirs, dir);

    for (const QString &dir : std::as_const(dataDirs))
        config.applicationDirs.append(dir + QStringLiteral("/applications"));
    config.flatpakApplicationDirs = { QDir::cleanPath(userFlatpak) + QStringLiteral("/applications"),
                                      systemFlatpak + QStringLiteral("/applications") };
    // Any XDG_DATA_DIRS entry inside a flatpak installation counts too.
    for (const QString &dir : std::as_const(dataDirs)) {
        if (dir.endsWith(QLatin1String("/flatpak/exports/share")))
            addDir(config.flatpakApplicationDirs, dir + QStringLiteral("/applications"));
    }

    if (!dataHome.isEmpty())
        config.manifestDirs.append(dataHome + QStringLiteral("/mightyos/apps"));
    if (!systemManifestDir.isEmpty())
        config.manifestDirs.append(systemManifestDir);

    config.currentDesktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP")
                                 .split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (config.currentDesktops.isEmpty())
        config.currentDesktops = { QStringLiteral("MightyOS") };

    const QProcessEnvironment childEnv = Launcher::buildChildEnvironment(QProcessEnvironment::systemEnvironment());
    config.path = childEnv.value(QStringLiteral("PATH"));
    config.locale = QLocale().name();
    return config;
}

namespace AppScanner {

QString categoryFor(const QStringList &categories)
{
    for (const QString &c : categories) {
        if (c == QLatin1String("AudioVideo") || c == QLatin1String("Video")
            || c == QLatin1String("Audio") || c == QLatin1String("TV"))
            return QStringLiteral("media");
    }
    if (categories.contains(QStringLiteral("Game")))
        return QStringLiteral("games");
    return QStringLiteral("apps");
}

QString artworkFor(const QString &icon, const IconLocator *icons)
{
    if (icon.isEmpty())
        return {};
    if (icon.startsWith(QLatin1Char('/'))) {
        if (!Ids::isCleanAbsolutePath(icon))
            return {};
        const QString suffix = QFileInfo(icon).suffix().toLower();
        static const QSet<QString> allowed { QStringLiteral("png"), QStringLiteral("svg"),
                                             QStringLiteral("xpm"),
                                             QStringLiteral("jpg"), QStringLiteral("jpeg") };
        const QFileInfo info(icon);
        if (!allowed.contains(suffix) || !info.isFile() || info.size() > kMaxIconFileBytes)
            return {};
        return QStringLiteral("file:") + icon;
    }
    // A relative path ("icons/foo.png") or a name with odd characters is
    // never resolved: no traversal out of the theme dirs.
    if (!Ids::isValidIconName(icon))
        return {};
    // Spec: a name may carry an extension by mistake ("foo.png").
    QString name = icon;
    for (const char *ext : { ".png", ".svg", ".xpm" }) {
        if (name.endsWith(QLatin1String(ext)))
            name.chop(4);
    }
    if (icons && icons->find(name, 128).isEmpty())
        return {};
    return QStringLiteral("theme:") + name;
}

std::optional<AppInfo> fromDesktopEntry(const DesktopEntry::Entry &entry, const QString &desktopFileId,
                                        const QString &path, bool inFlatpakDir,
                                        const AppScanConfig &config, QString *why)
{
    auto skip = [why](const char *reason) {
        if (why)
            *why = QString::fromLatin1(reason);
        return std::nullopt;
    };

    if (entry.string(QStringLiteral("Type")) != QLatin1String("Application"))
        return skip("not an application");
    if (entry.boolean(QStringLiteral("Hidden")))
        return skip("Hidden");
    if (entry.boolean(QStringLiteral("NoDisplay")))
        return skip("NoDisplay");

    const QStringList only = entry.list(QStringLiteral("OnlyShowIn"));
    if (!only.isEmpty()) {
        bool shown = false;
        for (const QString &desktop : config.currentDesktops)
            shown = shown || only.contains(desktop);
        if (!shown)
            return skip("OnlyShowIn");
    }
    for (const QString &desktop : entry.list(QStringLiteral("NotShowIn"))) {
        if (config.currentDesktops.contains(desktop))
            return skip("NotShowIn");
    }
    // There is no terminal emulator in a TV session.
    if (entry.boolean(QStringLiteral("Terminal")))
        return skip("Terminal");

    if (entry.has(QStringLiteral("TryExec"))) {
        const QString tryExec = entry.string(QStringLiteral("TryExec")).trimmed();
        if (tryExec.isEmpty() || !programResolves(tryExec, config))
            return skip("TryExec not found");
    }

    const QString title = cleanText(entry.localeString(QStringLiteral("Name"), config.locale), kMaxTitle);
    if (title.isEmpty())
        return skip("no Name");
    if (!entry.has(QStringLiteral("Exec")))
        return skip("no Exec");   // e.g. DBusActivatable only

    const QString icon = entry.string(QStringLiteral("Icon")).trimmed();
    const DesktopEntry::ExecResult exec = DesktopEntry::parseExec(
        entry.values.value(QStringLiteral("Exec")),
        { entry.localeString(QStringLiteral("Name"), config.locale), icon, path });
    if (!exec.ok) {
        if (why)
            *why = QStringLiteral("Exec rejected: ") + exec.error;
        return std::nullopt;
    }
    if (!programResolves(exec.argv.first(), config))
        return skip("program not found");

    AppInfo app;
    // flatpak:<appid> only for files Flatpak itself exported. Elsewhere an
    // X-Flatpak= key is just a claim: any file in ~/.local/share/applications
    // could impersonate an installed Flatpak (and its favorites/history).
    QString flatpakId;
    if (inFlatpakDir) {
        flatpakId = entry.string(QStringLiteral("X-Flatpak")).trimmed();
        if (!Ids::isValidFlatpakId(flatpakId))
            flatpakId = desktopFileId.chopped(8);   // minus ".desktop"
    }
    if (Ids::isValidFlatpakId(flatpakId))
        app.id = QStringLiteral("flatpak:") + flatpakId;
    else
        app.id = QStringLiteral("desktop:") + desktopFileId.chopped(8);

    app.title = title;
    QString subtitle = entry.localeString(QStringLiteral("GenericName"), config.locale);
    if (subtitle.isEmpty())
        subtitle = entry.localeString(QStringLiteral("Comment"), config.locale);
    app.subtitle = cleanText(subtitle, kMaxSubtitle);
    app.category = categoryFor(entry.list(QStringLiteral("Categories")));
    app.artwork = artworkFor(icon, config.icons);
    app.argv = exec.argv;
    app.source = path;
    return app;
}

std::optional<AppInfo> fromManifest(const QByteArray &data, const QString &path,
                                    const AppScanConfig &config, QString *why)
{
    auto skip = [why](const char *reason) {
        if (why)
            *why = QString::fromLatin1(reason);
        return std::nullopt;
    };
    if (data.size() > kMaxManifestBytes)
        return skip("manifest too large");
    QJsonParseError error {};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return skip("malformed JSON");
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("version")).toInt(-1) != 1)
        return skip("unsupported version");

    const QString id = o.value(QStringLiteral("id")).toString();
    if (!Ids::isValidManifestId(id))
        return skip("invalid id");
    const QJsonValue nameValue = o.value(QStringLiteral("name"));
    const QString title = cleanText(nameValue.toString(), kMaxTitle);
    if (!nameValue.isString() || title.isEmpty())
        return skip("invalid name");

    // exec must be an argument array; a string would need a shell.
    const QJsonValue execValue = o.value(QStringLiteral("exec"));
    if (!execValue.isArray())
        return skip("exec is not an array");
    const QJsonArray execArray = execValue.toArray();
    if (execArray.isEmpty() || execArray.size() > DesktopEntry::kMaxArgs)
        return skip("exec has no or too many arguments");
    QStringList argv;
    qsizetype total = 0;
    for (const QJsonValue &arg : execArray) {
        if (!arg.isString())
            return skip("exec argument is not a string");
        const QString s = arg.toString();
        if (s.contains(QChar(0)))
            return skip("NUL in exec argument");
        total += s.size();
        argv.append(s);
    }
    if (total > DesktopEntry::kMaxExecLength)
        return skip("exec too long");
    const QString &program = argv.first();
    if (program.isEmpty() || (program.contains(QLatin1Char('/')) && !Ids::isCleanAbsolutePath(program)))
        return skip("invalid program");
    if (!programResolves(program, config))
        return skip("program not found");

    QString category = o.value(QStringLiteral("category")).toString(QStringLiteral("apps"));
    if (category != QLatin1String("media") && category != QLatin1String("games")
        && category != QLatin1String("apps"))
        return skip("invalid category");

    AppInfo app;
    app.id = QStringLiteral("app:") + id;
    app.title = title;
    app.subtitle = cleanText(o.value(QStringLiteral("subtitle")).toString(), kMaxSubtitle);
    app.category = category;
    app.artwork = artworkFor(o.value(QStringLiteral("icon")).toString(), config.icons);
    app.argv = argv;
    app.source = path;
    return app;
}

AppScanResult scan(const AppScanConfig &config, const std::atomic_bool *cancel)
{
    AppScanResult result;
    // New icons may have been installed: re-index the themes whose
    // directories changed (on this worker thread, never the GUI thread).
    if (config.icons)
        config.icons->refreshIfChanged();
    QHash<QString, AppInfo> byId;
    QSet<QString> claimedDesktopIds;
    QStringList watch;

    auto addWatch = [&watch](const QString &dir) {
        if (!dir.isEmpty() && !watch.contains(dir) && watch.size() < kMaxWatchPaths)
            watch.append(dir);
    };

    // --- .desktop files: the first file with a given desktop-file id wins,
    // even if it is Hidden/NoDisplay or broken (that is how a user override
    // in ~/.local/share/applications hides a system entry).
    for (const QString &root : config.applicationDirs) {
        if (cancelled(cancel))
            break;
        if (!QFileInfo(root).isDir()) {
            addWatch(watchTargetFor(root));
            continue;
        }
        addWatch(root);
        const bool flatpak = config.flatpakApplicationDirs.contains(root);
        QDirIterator it(root, { QStringLiteral("*.desktop") }, QDir::Files | QDir::Readable,
                        QDirIterator::Subdirectories);   // no FollowSymlinks: no dir loops
        int files = 0;
        while (it.hasNext() && files < kMaxFilesPerDir && !cancelled(cancel)) {
            const QString path = it.next();
            ++files;
            const QString relative = QDir(root).relativeFilePath(path);
            if (relative.count(QLatin1Char('/')) > kMaxDepth)
                continue;
            addWatch(QFileInfo(path).absolutePath());
            QString desktopId = relative;
            desktopId.replace(QLatin1Char('/'), QLatin1Char('-'));
            if (!Ids::isValidDesktopFileId(desktopId)) {
                ++result.skipped;
                continue;
            }
            if (claimedDesktopIds.contains(desktopId))
                continue;   // shadowed by a higher-precedence dir
            claimedDesktopIds.insert(desktopId);
            ++result.desktopFiles;

            const std::optional<QByteArray> data = JsonFile::readRegularFile(path, DesktopEntry::kMaxFileBytes, true);
            QString why;
            std::optional<DesktopEntry::Entry> entry;
            if (data)
                entry = DesktopEntry::parse(*data, &why);
            else
                why = QStringLiteral("unreadable or too large");
            std::optional<AppInfo> app;
            if (entry)
                app = fromDesktopEntry(*entry, desktopId, path, flatpak, config, &why);
            if (!app) {
                ++result.skipped;
                qCDebug(lcAppScan) << "skipped" << desktopId << why;
                continue;
            }
            if (byId.contains(app->id)) {
                ++result.skipped;   // e.g. two files claiming one X-Flatpak id
                continue;
            }
            byId.insert(app->id, *app);
        }
    }

    // --- MightyOS manifests: user dir first; the first id wins.
    for (const QString &dir : config.manifestDirs) {
        if (cancelled(cancel))
            break;
        if (!QFileInfo(dir).isDir()) {
            addWatch(watchTargetFor(dir));
            continue;
        }
        addWatch(dir);
        const QStringList files = QDir(dir).entryList({ QStringLiteral("*.json") },
                                                     QDir::Files | QDir::Readable, QDir::Name);
        int count = 0;
        for (const QString &name : files) {
            if (++count > kMaxManifestsPerDir || cancelled(cancel))
                break;
            const QString path = dir + QLatin1Char('/') + name;
            ++result.manifests;
            const std::optional<QByteArray> data = JsonFile::readRegularFile(path, kMaxManifestBytes, false);
            QString why = QStringLiteral("unreadable or too large");
            std::optional<AppInfo> app;
            if (data)
                app = fromManifest(*data, path, config, &why);
            if (!app) {
                ++result.skipped;
                qCDebug(lcAppScan) << "skipped manifest" << name << why;
                continue;
            }
            if (byId.contains(app->id))
                continue;
            byId.insert(app->id, *app);
        }
    }

    result.apps = byId.values();
    // Same ordering as TileListModel's letter jump.
    std::sort(result.apps.begin(), result.apps.end(), [](const AppInfo &a, const AppInfo &b) {
        if (a.title != b.title)
            return TileListModel::sortLess(a.title, b.title);
        return a.id < b.id;
    });
    result.watchPaths = watch;
    return result;
}

} // namespace AppScanner
