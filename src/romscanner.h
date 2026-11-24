#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <atomic>

// One game file found under a system's ROM directory.
struct RomRecord {
    QString id;        // rom:<system>:<sha1(relPath)>
    QString system;
    QString root;      // the ROM directory it was found in (absolute)
    QString relPath;   // relative to root, '/'-separated
    QString title;     // cleaned from the file name
    QString region;    // "USA, Europe", from the name's tags
    QString tags;      // other tags: "Rev 1, Beta, En, Fr"
    int discs = 1;     // > 1 for .m3u playlists and "(Disc N)" groups
    int disc = 0;      // "(Disc N)" in the file name, 0 if none
    qint64 size = 0;
    qint64 mtime = 0;  // ms since the epoch

    bool operator==(const RomRecord &) const = default;
};

struct RomScanSpec {
    QString system;
    QStringList extensions;   // lower case, no dot
    QStringList roots;        // absolute
};

struct RomScanStats {
    int dirs = 0;
    int files = 0;            // regular files looked at
    int matched = 0;          // games produced
    int reused = 0;           // unchanged since the previous scan (size + mtime)
    int symlinksRejected = 0; // pointing outside the ROM root, or dangling
    int specialSkipped = 0;   // FIFOs, sockets, devices
    bool truncated = false;   // a limit was hit
    QStringList missingRoots; // configured ROM dirs that don't exist (unplugged drive?)
};

struct CleanTitle {
    QString title, region, tags;
    int disc = 0;
};

// The scanner runs on a worker thread; everything here is reentrant.
//
// Untrusted input: file names are arbitrary bytes. The walk
//  - never leaves a ROM root: a symlink is followed only if its target
//    (realpath) is inside the root's realpath; others count as rejected,
//  - opens directories with O_NOFOLLOW (except checked symlinks), uses
//    (dev, inode) to break cycles and goes at most kMaxDepth levels down,
//  - only takes regular files (FIFOs, sockets and devices are skipped,
//    never opened) and skips hidden entries,
//  - stops at kMaxFilesPerSystem games / kMaxEntries directory entries,
//  - reads .m3u and .cue playlists through the fd checks, capped.
// Titles are sanitised for display (TextSanitize::label).
//
// Multi-disc games:
//  - an .m3u playlist is the game; files it lists are hidden and its entry
//    count is `discs`,
//  - a .cue hides the track files it references (.bin, ...),
//  - the same stem with several extensions keeps one: .m3u, then .cue,
//    then the order of the system's extension list,
//  - without a playlist, "(Disc 1)", "(Disc 2)", ... of one title in one
//    directory become one game (disc 1, `discs` = how many).
namespace RomScanner {

constexpr int kMaxDepth = 4;
constexpr int kMaxFilesPerSystem = 50000;
constexpr int kMaxEntries = 400000;
constexpr qint64 kMaxPlaylistBytes = 64 * 1024;
constexpr int kMaxPlaylistEntries = 16;

QString romId(const QString &system, const QString &relPath);

CleanTitle cleanTitle(const QString &fileStem);

// `previous`: the last scan's records by id (cache), reused when size and
// mtime are unchanged. `cancel` may be null.
QList<RomRecord> scan(const RomScanSpec &spec, const QHash<QString, RomRecord> &previous,
                      RomScanStats *stats = nullptr, const std::atomic_bool *cancel = nullptr);

// Launch-time re-check of a cached record: root + relPath must be clean,
// still a regular file, and (through symlinks) still inside the root.
// Returns the resolved path (realpath, the file that was checked) or "".
QString verifiedPath(const QString &root, const QString &relPath);

} // namespace RomScanner
