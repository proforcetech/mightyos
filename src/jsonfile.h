#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <optional>

// Small, size-capped JSON object files written atomically (QSaveFile).
namespace JsonFile {

enum class ReadStatus { Ok, Missing, Invalid };

// Reads a regular file of at most `limit` bytes; nullopt otherwise. All
// checks run on the opened fd (no stat-then-open race): O_NONBLOCK so a
// FIFO planted under the name can't hang the caller, fstat() must say
// regular file, the read is capped. Symlinks are followed only if
// `followSymlinks` (application dirs: Flatpak exports are symlinks).
std::optional<QByteArray> readRegularFile(const QString &path, qint64 limit, bool followSymlinks);

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
