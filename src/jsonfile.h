#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>

// Small, size-capped JSON object files written atomically (QSaveFile).
namespace JsonFile {

enum class ReadStatus { Ok, Missing, Invalid };

// Reads a regular file of at most `limit` bytes; nullopt otherwise. All
// checks run on the opened fd (no stat-then-open race): O_NONBLOCK so a
// FIFO planted under the name can't hang the caller, fstat() must say
// regular file, the read is capped. Symlinks are followed only if
// `followSymlinks` (application dirs: Flatpak exports are symlinks).
// On failure `*error` (if given) is the errno of open()/read() (ENOENT
// for a missing file, EACCES, EIO, ...), EINVAL for "not a regular file",
// EFBIG for over `limit`, EAGAIN for "changed while reading".
// `owner`/`mode`/`group` (if given) receive the opened file's st_uid /
// st_mode / st_gid.
std::optional<QByteArray> readRegularFile(const QString &path, qint64 limit, bool followSymlinks,
                                          int *error = nullptr, uint *owner = nullptr, uint *mode = nullptr,
                                          uint *group = nullptr);

// Writes `data` to <base>/<subdirs...>/<name> without following any symlink
// below `base` (PLAN section 4; for files other programs, e.g. a Flatpak
// sandbox, could tamper with): every subdir is created 0700 with mkdirat()
// and opened O_NOFOLLOW (a symlink there fails the write), must be ours
// and gets group/other access removed; the data goes to a fresh
// O_CREAT|O_EXCL|O_NOFOLLOW temporary, fchmod `mode`, fsync, then
// renameat() over <name> (a symlink at <name> is replaced, never
// followed). `base` itself must exist and be a directory (it may be a
// symlink: the user's own XDG dir). Returns false and leaves nothing
// behind on any failure.
bool writePrivate(const QString &base, const QStringList &subdirs, const QString &name, const QByteArray &data,
                  uint mode = 0600, QString *error = nullptr);

// Removes <base>/<subdirs...>/<name> (a file, a symlink or a whole tree)
// without following any symlink: `base` and every step are opened
// O_NOFOLLOW | O_DIRECTORY, entries are removed with unlinkat() (a symlink
// is removed, never followed), subdirectories are entered with openat()
// (O_NOFOLLOW: a directory swapped for a symlink mid-walk is not entered).
// For trees another program (a Flatpak sandbox) can write. Nothing there
// (any step missing) is success. Depth is capped at 64.
bool removeTreeNoFollow(const QString &base, const QStringList &subdirs, const QString &name);

// Renames `path` to `path + suffix` (replacing an older one), so an
// unusable or newer-format file is kept for the user instead of being
// overwritten by the next write.
bool moveAside(const QString &path, const QString &suffix);

// `status` tells a missing file (normal on first use) from an unusable one.
// Symlinks are not followed.
std::optional<QJsonObject> read(const QString &path, qint64 maxBytes, ReadStatus *status = nullptr);

// Creates the parent directory. Returns false on any failure; the old file
// is then untouched.
bool write(const QString &path, const QJsonObject &object);

} // namespace JsonFile
