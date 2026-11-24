#include "romscanner.h"

#include "ids.h"
#include "jsonfile.h"
#include "textsanitize.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace RomScanner {

QString romId(const QString &system, const QString &relPath)
{
    return QStringLiteral("rom:") + system + QLatin1Char(':')
           + QString::fromLatin1(QCryptographicHash::hash(relPath.toUtf8(), QCryptographicHash::Sha1).toHex());
}

namespace {

const QSet<QString> &regionWords()
{
    static const QSet<QString> words {
        QStringLiteral("usa"), QStringLiteral("europe"), QStringLiteral("japan"), QStringLiteral("world"),
        QStringLiteral("asia"), QStringLiteral("korea"), QStringLiteral("china"), QStringLiteral("taiwan"),
        QStringLiteral("brazil"), QStringLiteral("australia"), QStringLiteral("canada"), QStringLiteral("france"),
        QStringLiteral("germany"), QStringLiteral("italy"), QStringLiteral("spain"), QStringLiteral("netherlands"),
        QStringLiteral("sweden"), QStringLiteral("uk"), QStringLiteral("russia"), QStringLiteral("hong kong"),
        QStringLiteral("scandinavia"), QStringLiteral("latin america"), QStringLiteral("unknown"),
        QStringLiteral("u"), QStringLiteral("e"), QStringLiteral("j"), QStringLiteral("ue"), QStringLiteral("ju"),
        QStringLiteral("jue"), QStringLiteral("eu"), QStringLiteral("us"), QStringLiteral("jp"),
    };
    return words;
}

bool isRegionGroup(const QString &content)
{
    const QStringList parts = content.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return false;
    for (const QString &part : parts) {
        if (!regionWords().contains(part.trimmed().toLower()))
            return false;
    }
    return true;
}

int discNumber(const QString &content)
{
    static const QRegularExpression re(QStringLiteral("^(?:disc|disk|cd)\\s*(\\d{1,2})(?:\\s*of\\s*\\d{1,2})?$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(content.trimmed());
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

} // namespace

CleanTitle cleanTitle(const QString &fileStem)
{
    CleanTitle out;
    static const QRegularExpression groupRe(QStringLiteral("\\(([^()]*)\\)|\\[([^\\[\\]]*)\\]"));
    QStringList regions, tags;
    QString rest;
    qsizetype last = 0;
    auto it = groupRe.globalMatch(fileStem);
    while (it.hasNext()) {
        const auto m = it.next();
        rest += fileStem.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const bool round = m.capturedStart(1) >= 0;
        const QString content = (round ? m.captured(1) : m.captured(2)).trimmed();
        if (content.isEmpty())
            continue;
        if (round && isRegionGroup(content)) {
            for (const QString &part : content.split(QLatin1Char(','), Qt::SkipEmptyParts))
                regions.append(part.trimmed());
        } else if (const int disc = discNumber(content); disc > 0) {
            out.disc = disc;
        } else {
            tags.append(content);
        }
    }
    rest += fileStem.mid(last);

    QString title = rest;
    if (!title.contains(QLatin1Char(' ')) && title.contains(QLatin1Char('_')))
        title.replace(QLatin1Char('_'), QLatin1Char(' '));
    title = title.simplified();
    while (title.endsWith(QLatin1Char('-')) || title.endsWith(QLatin1Char(',')) || title.endsWith(QLatin1Char('.')))
        title = title.chopped(1).trimmed();
    // "Legend of Zelda, The - A Link to the Past" -> "The Legend of Zelda - ..."
    static const QRegularExpression articleRe(QStringLiteral("^(.+?), (The|A|An)(\\s+-\\s+.*)?$"));
    if (const auto m = articleRe.match(title); m.hasMatch())
        title = m.captured(2) + QLatin1Char(' ') + m.captured(1) + m.captured(3);
    title = TextSanitize::label(title, 120);
    if (title.isEmpty())
        title = TextSanitize::label(fileStem, 120);
    out.title = title;
    out.region = TextSanitize::label(regions.join(QStringLiteral(", ")), 64);
    out.tags = TextSanitize::label(tags.join(QStringLiteral(", ")), 120);
    return out;
}

namespace {

struct Found {
    QString relPath;   // '/'-separated
    QString relDir;    // "" for the root
    QString stem;
    QString ext;
    qint64 size = 0;
    qint64 mtime = 0;
};

QString canonical(const QByteArray &path)
{
    char buffer[PATH_MAX];
    if (!::realpath(path.constData(), buffer))
        return {};
    return QFile::decodeName(buffer);
}

bool inside(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root + QLatin1Char('/'));
}

qint64 mtimeMs(const struct stat &st)
{
    return qint64(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
}

struct Walker {
    const RomScanSpec &spec;
    QSet<QString> extensions;
    RomScanStats &stats;
    const std::atomic_bool *cancel;
    int entries = 0;

    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }

    // Walks one root; appends to `out`.
    void walk(const QString &root, QList<Found> &out)
    {
        const QString realRoot = canonical(QFile::encodeName(root));
        if (realRoot.isEmpty()) {
            stats.missingRoots.append(root);   // not created yet, or a drive that isn't plugged in
            return;
        }
        struct Dir { QString abs; QString rel; int depth; };
        QList<Dir> queue { { realRoot, QString(), 0 } };
        QSet<QPair<quint64, quint64>> visited;
        while (!queue.isEmpty()) {
            if (cancelled())
                return;
            const Dir dir = queue.takeLast();
            const int fd = ::open(QFile::encodeName(dir.abs).constData(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0)
                continue;
            struct stat dst {};
            if (::fstat(fd, &dst) != 0
                || visited.contains({ quint64(dst.st_dev), quint64(dst.st_ino) })) {
                ::close(fd);
                continue;
            }
            visited.insert({ quint64(dst.st_dev), quint64(dst.st_ino) });
            DIR *d = ::fdopendir(fd);
            if (!d) {
                ::close(fd);
                continue;
            }
            ++stats.dirs;
            while (struct dirent *e = ::readdir(d)) {
                if (++entries > kMaxEntries || out.size() >= kMaxFilesPerSystem) {
                    stats.truncated = true;
                    break;
                }
                const char *name = e->d_name;
                if (name[0] == '.')
                    continue;   // ".", "..", hidden files and dirs
                const QString qname = QFile::decodeName(name);
                const QString rel = dir.rel.isEmpty() ? qname : dir.rel + QLatin1Char('/') + qname;
                const QString abs = dir.abs + QLatin1Char('/') + qname;

                unsigned char type = e->d_type;
                struct stat st {};
                bool haveStat = false;
                if (type == DT_UNKNOWN) {
                    if (::fstatat(::dirfd(d), name, &st, AT_SYMLINK_NOFOLLOW) != 0)
                        continue;
                    haveStat = true;
                    type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG
                         : S_ISLNK(st.st_mode) ? DT_LNK : DT_FIFO;
                }
                QString targetAbs = abs;
                if (type == DT_LNK) {
                    const QString target = canonical(QFile::encodeName(abs));
                    if (target.isEmpty() || !inside(target, realRoot)) {
                        ++stats.symlinksRejected;
                        continue;
                    }
                    if (::stat(QFile::encodeName(target).constData(), &st) != 0) {
                        ++stats.symlinksRejected;
                        continue;
                    }
                    haveStat = true;
                    targetAbs = target;
                    type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : DT_FIFO;
                }
                if (type == DT_DIR) {
                    if (dir.depth + 1 <= kMaxDepth)
                        queue.append({ targetAbs, rel, dir.depth + 1 });
                    else
                        stats.truncated = true;
                    continue;
                }
                if (type != DT_REG) {
                    ++stats.specialSkipped;
                    continue;
                }
                ++stats.files;
                const qsizetype dot = qname.lastIndexOf(QLatin1Char('.'));
                if (dot <= 0)
                    continue;
                const QString ext = qname.mid(dot + 1).toLower();
                if (!extensions.contains(ext))
                    continue;
                if (!haveStat && ::fstatat(::dirfd(d), name, &st, AT_SYMLINK_NOFOLLOW) != 0)
                    continue;
                if (!S_ISREG(st.st_mode))
                    continue;
                out.append({ rel, dir.rel, qname.left(dot), ext, qint64(st.st_size), mtimeMs(st) });
            }
            ::closedir(d);
            if (stats.truncated && (entries > kMaxEntries || out.size() >= kMaxFilesPerSystem))
                break;
        }
    }
};

// Relative paths a playlist (.m3u) or cue sheet (.cue) refers to,
// resolved against its directory and cleaned; ".." escapes are dropped.
QStringList playlistEntries(const QString &absPath, const QString &relDir, bool cue)
{
    const auto data = JsonFile::readRegularFile(absPath, kMaxPlaylistBytes, true);
    if (!data)
        return {};
    QStringList out;
    static const QRegularExpression fileRe(QStringLiteral("^\\s*FILE\\s+\"([^\"]+)\"|^\\s*FILE\\s+(\\S+)"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QStringList lines = QString::fromUtf8(*data).split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        QString entry = line;
        if (cue) {
            const auto m = fileRe.match(line);
            if (!m.hasMatch())
                continue;
            entry = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        }
        entry.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (entry.startsWith(QLatin1Char('/')))
            continue;
        const QString rel = QDir::cleanPath(relDir.isEmpty() ? entry : relDir + QLatin1Char('/') + entry);
        if (rel.startsWith(QLatin1String("..")) || rel.startsWith(QLatin1Char('/')))
            continue;
        out.append(rel);
        if (out.size() >= kMaxPlaylistEntries)
            break;
    }
    return out;
}

} // namespace

QList<RomRecord> scan(const RomScanSpec &spec, const QHash<QString, RomRecord> &previous, RomScanStats *statsOut,
                      const std::atomic_bool *cancel)
{
    RomScanStats stats;
    Walker walker { spec, QSet<QString>(spec.extensions.cbegin(), spec.extensions.cend()), stats, cancel };
    QList<RomRecord> records;
    QSet<QString> seenRel;   // first root wins for the same relPath (same id)

    for (const QString &root : spec.roots) {
        if (!Ids::isCleanAbsolutePath(root))
            continue;
        QList<Found> found;
        walker.walk(root, found);
        if (walker.cancelled())
            return {};

        // Multi-disc / playlist handling, per directory.
        QSet<QString> hidden;
        QHash<QString, int> playlistDiscs;   // relPath -> entries
        QHash<QString, int> byRel;
        for (int i = 0; i < found.size(); ++i)
            byRel.insert(found.at(i).relPath, i);
        for (const Found &f : std::as_const(found)) {
            if (f.ext != QLatin1String("m3u") && f.ext != QLatin1String("cue"))
                continue;
            const bool cue = f.ext == QLatin1String("cue");
            const QStringList entries = playlistEntries(root + QLatin1Char('/') + f.relPath, f.relDir, cue);
            for (const QString &rel : entries) {
                if (rel != f.relPath && byRel.contains(rel))
                    hidden.insert(rel);
            }
            if (!cue)
                playlistDiscs.insert(f.relPath, qMax(1, static_cast<int>(entries.size())));
        }
        // Same stem, several extensions: keep the best one.
        auto rank = [&spec](const QString &ext) {
            if (ext == QLatin1String("m3u"))
                return -2;
            if (ext == QLatin1String("cue"))
                return -1;
            return static_cast<int>(spec.extensions.indexOf(ext));
        };
        QHash<QString, int> bestByStem;   // relDir/stem -> index
        for (int i = 0; i < found.size(); ++i) {
            const Found &f = found.at(i);
            if (hidden.contains(f.relPath))
                continue;
            const QString key = f.relDir + QLatin1Char('/') + f.stem;
            const auto it = bestByStem.constFind(key);
            if (it == bestByStem.cend()) {
                bestByStem.insert(key, i);
            } else if (rank(f.ext) < rank(found.at(it.value()).ext)) {
                hidden.insert(found.at(it.value()).relPath);
                bestByStem.insert(key, i);
            } else {
                hidden.insert(f.relPath);
            }
        }

        // Build records; group "(Disc N)" siblings without a playlist.
        struct DiscGroup { int record; int lowest; int count; };
        QHash<QString, DiscGroup> discGroups;   // relDir|title|region|tags
        QList<RomRecord> rootRecords;
        QList<bool> removed;
        for (const Found &f : std::as_const(found)) {
            if (hidden.contains(f.relPath) || seenRel.contains(f.relPath))
                continue;
            RomRecord r;
            r.id = romId(spec.system, f.relPath);
            const auto prev = previous.constFind(r.id);
            if (prev != previous.cend() && prev->root == root && prev->relPath == f.relPath
                && prev->size == f.size && prev->mtime == f.mtime) {
                r = *prev;   // unchanged: no title parsing
                ++stats.reused;
            } else {
                const CleanTitle clean = cleanTitle(f.stem);
                r.system = spec.system;
                r.root = root;
                r.relPath = f.relPath;
                r.title = clean.title;
                r.region = clean.region;
                r.tags = clean.tags;
                r.disc = clean.disc;
                r.size = f.size;
                r.mtime = f.mtime;
            }
            r.discs = playlistDiscs.value(f.relPath, 1);
            if (r.disc > 0 && f.ext != QLatin1String("m3u")) {
                const QString key = f.relDir + QLatin1Char('|') + r.title + QLatin1Char('|') + r.region
                                    + QLatin1Char('|') + r.tags;
                auto g = discGroups.find(key);
                if (g == discGroups.end()) {
                    discGroups.insert(key, { static_cast<int>(rootRecords.size()), r.disc, 1 });
                } else {
                    ++g->count;
                    if (r.disc < g->lowest) {
                        removed[g->record] = true;
                        g->record = static_cast<int>(rootRecords.size());
                        g->lowest = r.disc;
                    } else {
                        continue;   // a later disc: hidden behind disc 1
                    }
                }
            }
            rootRecords.append(r);
            removed.append(false);
        }
        for (const DiscGroup &g : std::as_const(discGroups))
            rootRecords[g.record].discs = qMax(rootRecords[g.record].discs, g.count);
        for (int i = 0; i < rootRecords.size(); ++i) {
            if (removed.at(i))
                continue;
            seenRel.insert(rootRecords.at(i).relPath);
            records.append(rootRecords.at(i));
        }
    }
    stats.matched = static_cast<int>(records.size());
    if (statsOut)
        *statsOut = stats;
    return records;
}

QString verifiedPath(const QString &root, const QString &relPath)
{
    if (!Ids::isCleanAbsolutePath(root) || relPath.isEmpty() || relPath.startsWith(QLatin1Char('/')))
        return {};
    const QString abs = root + QLatin1Char('/') + relPath;
    if (!Ids::isCleanAbsolutePath(abs))
        return {};   // ".." or "." components
    const QString realRoot = canonical(QFile::encodeName(root));
    const QString real = canonical(QFile::encodeName(abs));
    if (realRoot.isEmpty() || real.isEmpty() || !inside(real, realRoot) || real == realRoot)
        return {};
    struct stat st {};
    if (::stat(QFile::encodeName(real).constData(), &st) != 0 || !S_ISREG(st.st_mode))
        return {};
    return real;   // what was checked is what the emulator gets
}

} // namespace RomScanner
