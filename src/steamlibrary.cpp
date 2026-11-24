#include "steamlibrary.h"

#include "ids.h"
#include "jsonfile.h"
#include "keyvalues.h"
#include "textsanitize.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <sys/stat.h>
#include <unistd.h>

namespace SteamLibrary {

bool isValidAppId(const QString &appid)
{
    static const QRegularExpression re(QStringLiteral("^[1-9][0-9]{0,9}$"));
    return re.match(appid).hasMatch();
}

bool isTool(const QString &appid, const QString &name)
{
    static const QSet<QString> tools {
        QStringLiteral("228980"),   // Steamworks Common Redistributables
        QStringLiteral("1007"),     // Steamworks SDK Redist
        QStringLiteral("1070560"),  // Steam Linux Runtime 1.0 (scout)
        QStringLiteral("1391110"),  // Steam Linux Runtime 2.0 (soldier)
        QStringLiteral("1628350"),  // Steam Linux Runtime 3.0 (sniper)
        QStringLiteral("1493710"),  // Proton Experimental
        QStringLiteral("2180100"),  // Proton Hotfix
        QStringLiteral("1826330"),  // Proton EasyAntiCheat Runtime
        QStringLiteral("1161040"),  // Proton BattlEye Runtime
        QStringLiteral("250820"),   // SteamVR
        QStringLiteral("858280"), QStringLiteral("930400"), QStringLiteral("961940"), QStringLiteral("1054830"),
        QStringLiteral("1113280"), QStringLiteral("1245040"), QStringLiteral("1420170"), QStringLiteral("1580130"),
        QStringLiteral("1887720"), QStringLiteral("2348590"), QStringLiteral("2805730"),   // Proton x.y
    };
    if (tools.contains(appid))
        return true;
    for (const char *prefix : { "Proton ", "Steam Linux Runtime", "Steamworks Common", "Steamworks SDK" }) {
        if (name.startsWith(QLatin1String(prefix), Qt::CaseInsensitive))
            return true;
    }
    return name.compare(QLatin1String("Proton"), Qt::CaseInsensitive) == 0;
}

namespace {

QString realPath(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return Ids::isCleanAbsolutePath(canonical) ? canonical : QString();
}

std::optional<KeyValues::Node> readVdf(const QString &path, qint64 limit)
{
    // Steam's own files; symlinks are followed (~/.steam/steam is one).
    const auto data = JsonFile::readRegularFile(path, limit, true);
    if (!data)
        return std::nullopt;
    KeyValues::Limits limits;
    limits.maxBytes = limit;
    return KeyValues::parse(*data, limits);
}

QString flatpakHome(const QString &home)
{
    return home + QStringLiteral("/.var/app/") + kFlatpakId;
}

} // namespace

QList<SteamRoot> findRoots(const QString &home)
{
    QList<SteamRoot> roots;
    if (!Ids::isCleanAbsolutePath(home))
        return roots;
    const QString fp = flatpakHome(home);
    const struct { QString path; bool flatpak; } candidates[] = {
        { home + QStringLiteral("/.steam/steam"), false },
        { home + QStringLiteral("/.steam/root"), false },
        { home + QStringLiteral("/.local/share/Steam"), false },
        { fp + QStringLiteral("/.local/share/Steam"), true },
        { fp + QStringLiteral("/.steam/steam"), true },
    };
    QSet<QString> seen;
    for (const auto &c : candidates) {
        const QString real = realPath(c.path);
        if (real.isEmpty() || seen.contains(real) || !QFileInfo(real + QStringLiteral("/steamapps")).isDir())
            continue;
        seen.insert(real);
        roots.append({ real, c.flatpak || real.startsWith(fp + QLatin1Char('/')) });
    }
    return roots;
}

QStringList libraryFolders(const SteamRoot &root, const QString &home)
{
    QStringList out { root.path };
    std::optional<KeyValues::Node> vdf;
    for (const char *rel : { "/steamapps/libraryfolders.vdf", "/config/libraryfolders.vdf" }) {
        vdf = readVdf(root.path + QLatin1String(rel), kMaxVdfBytes);
        if (vdf)
            break;
    }
    const KeyValues::Node *folders = vdf ? vdf->child(u"libraryfolders") : nullptr;
    if (!folders || !folders->isBlock)
        return out;
    const QString fp = flatpakHome(home);
    for (const KeyValues::Node &entry : folders->children) {
        if (out.size() >= kMaxLibraries)
            break;
        // New format: "0" { "path" "..." }; old format: "1" "/path".
        QString path = entry.isBlock ? entry.valueOf(u"path") : entry.value;
        if (!entry.isBlock && !path.startsWith(QLatin1Char('/')))
            continue;   // "contentstatsid" and friends
        while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
            path.chop(1);
        if (!Ids::isCleanAbsolutePath(path))
            continue;
        if (root.flatpak && path.startsWith(home + QLatin1Char('/')) && !path.startsWith(fp + QLatin1Char('/'))) {
            const QString mapped = fp + path.mid(home.size());
            if (QFileInfo(mapped + QStringLiteral("/steamapps")).isDir())
                path = mapped;
        }
        const QString real = realPath(path);
        if (real.isEmpty() || out.contains(real) || !QFileInfo(real + QStringLiteral("/steamapps")).isDir())
            continue;
        out.append(real);
    }
    return out;
}

std::optional<SteamGame> parseManifest(const QByteArray &data, const QString &expectedAppId)
{
    KeyValues::Limits limits;
    limits.maxBytes = kMaxManifestBytes;
    limits.maxDepth = 8;
    const auto doc = KeyValues::parse(data, limits);
    const KeyValues::Node *state = doc ? doc->child(u"AppState") : nullptr;
    if (!state || !state->isBlock)
        return std::nullopt;
    SteamGame game;
    game.appid = state->valueOf(u"appid");
    if (!isValidAppId(game.appid) || (!expectedAppId.isEmpty() && game.appid != expectedAppId))
        return std::nullopt;
    bool ok = false;
    const qulonglong flags = state->valueOf(u"StateFlags").toULongLong(&ok);
    if (!ok || !(flags & 4))
        return std::nullopt;   // not fully installed (downloading, uninstalled, ...)
    game.name = TextSanitize::label(state->valueOf(u"name"), 100);
    if (game.name.isEmpty())
        game.name = QStringLiteral("Steam app %1").arg(game.appid);
    if (isTool(game.appid, game.name))
        return std::nullopt;
    game.lastPlayed = qMax<qint64>(0, state->valueOf(u"LastPlayed").toLongLong());
    return game;
}

QString artworkFor(const QString &root, const QString &appid)
{
    if (!isValidAppId(appid) || !Ids::isCleanAbsolutePath(root))
        return {};
    const QString cache = root + QStringLiteral("/appcache/librarycache");
    const auto regular = [](const QString &path) {
        const QFileInfo info(path);
        return info.isFile() && info.size() > 0 ? path : QString();
    };
    for (const QString &candidate : { cache + QLatin1Char('/') + appid + QStringLiteral("_library_600x900.jpg"),
                                      cache + QLatin1Char('/') + appid + QStringLiteral("/library_600x900.jpg") }) {
        if (const QString found = regular(candidate); !found.isEmpty())
            return found;
    }
    // Newer clients: librarycache/<appid>/<hash>/library_600x900.jpg
    const QDir dir(cache + QLatin1Char('/') + appid);
    const QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (int i = 0; i < subdirs.size() && i < 16; ++i) {
        if (const QString found = regular(dir.absoluteFilePath(subdirs.at(i) + QStringLiteral("/library_600x900.jpg")));
            !found.isEmpty() && Ids::isCleanAbsolutePath(found))
            return found;
    }
    for (const QString &candidate : { cache + QLatin1Char('/') + appid + QStringLiteral("_header.jpg"),
                                      cache + QLatin1Char('/') + appid + QStringLiteral("/header.jpg") }) {
        if (const QString found = regular(candidate); !found.isEmpty())
            return found;
    }
    return {};
}

QList<SteamGame> scan(const QString &home, const std::atomic_bool *cancel)
{
    QList<SteamGame> games;
    QSet<QString> seen;
    static const QRegularExpression manifestRe(QStringLiteral("^appmanifest_([1-9][0-9]{0,9})\\.acf$"));
    for (const SteamRoot &root : findRoots(home)) {
        for (const QString &library : libraryFolders(root, home)) {
            const QDir steamapps(library + QStringLiteral("/steamapps"));
            const QStringList names = steamapps.entryList({ QStringLiteral("appmanifest_*.acf") },
                                                          QDir::Files | QDir::NoSymLinks, QDir::Name);
            int count = 0;
            for (const QString &name : names) {
                if (cancel && cancel->load(std::memory_order_relaxed))
                    return {};
                if (++count > kMaxManifests)
                    break;
                const auto m = manifestRe.match(name);
                if (!m.hasMatch() || seen.contains(m.captured(1)))
                    continue;
                const auto data = JsonFile::readRegularFile(steamapps.absoluteFilePath(name), kMaxManifestBytes, false);
                if (!data)
                    continue;
                auto game = parseManifest(*data, m.captured(1));
                if (!game)
                    continue;
                game->root = root.path;
                game->flatpak = root.flatpak;
                game->artwork = artworkFor(root.path, game->appid);
                seen.insert(game->appid);
                games.append(*game);
            }
        }
    }
    return games;
}

QStringList registryFiles(const QString &home)
{
    return { home + QStringLiteral("/.steam/registry.vdf"),
             flatpakHome(home) + QStringLiteral("/.steam/registry.vdf") };
}

bool registrySaysRunning(const QByteArray &registryVdf, const QString &appid)
{
    if (!isValidAppId(appid))
        return false;
    KeyValues::Limits limits;
    limits.maxBytes = kMaxVdfBytes;
    const auto doc = KeyValues::parse(registryVdf, limits);
    if (!doc)
        return false;
    const KeyValues::Node *steam = doc->path({ u"Registry", u"HKCU", u"Software", u"Valve", u"Steam" });
    if (!steam)
        return false;
    if (steam->valueOf(u"RunningAppID") == appid)
        return true;
    const KeyValues::Node *apps = steam->child(u"apps");
    const KeyValues::Node *app = apps && apps->isBlock ? apps->child(appid) : nullptr;
    return app && app->isBlock && app->valueOf(u"Running") == QLatin1String("1");
}

QStringList steamPidFiles(const QString &home)
{
    return { home + QStringLiteral("/.steam/steam.pid"), flatpakHome(home) + QStringLiteral("/.steam/steam.pid") };
}

bool steamClientAlive(const QString &procRoot, const QStringList &pidFiles)
{
    static const QRegularExpression digits(QStringLiteral("^[1-9][0-9]{0,9}$"));
    for (const QString &file : pidFiles) {
        const auto data = JsonFile::readRegularFile(file, 64, true);
        const QString pid = data ? QString::fromLatin1(*data).trimmed() : QString();
        if (!digits.match(pid).hasMatch())
            continue;
        const QString dir = procRoot + QLatin1Char('/') + pid;
        struct stat st {};
        if (::stat(QFile::encodeName(dir).constData(), &st) != 0 || st.st_uid != ::getuid())
            continue;
        QFile comm(dir + QStringLiteral("/comm"));
        if (comm.open(QIODevice::ReadOnly | QIODevice::Unbuffered) && comm.read(64).startsWith("steam"))
            return true;
    }
    return false;
}

bool processRunning(const QString &procRoot, const QString &appid)
{
    if (!isValidAppId(appid))
        return false;
    const QByteArray a = "SteamAppId=" + appid.toLatin1();
    const QByteArray b = "SteamGameId=" + appid.toLatin1();
    const uid_t uid = ::getuid();
    const QDir proc(procRoot);
    const QStringList pids = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    for (const QString &pid : pids) {
        if (!digits.match(pid).hasMatch())
            continue;
        const QString dir = proc.absoluteFilePath(pid);
        struct stat st {};
        if (::stat(QFile::encodeName(dir).constData(), &st) != 0 || st.st_uid != uid)
            continue;
        // /proc files report size 0; readRegularFile reads what fstat
        // promises plus one byte, so read through QFile instead (capped).
        QFile environ(dir + QStringLiteral("/environ"));
        if (!environ.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
            continue;
        const QByteArray env = environ.read(256 * 1024);
        for (const QByteArray &entry : env.split('\0')) {
            if (entry == a || entry == b)
                return true;
        }
    }
    return false;
}

} // namespace SteamLibrary
