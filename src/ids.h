#pragma once

#include <QString>

// Validation of every id or file name that ends up in a filesystem path or
// is read from untrusted files (PLAN.md section 4: validate every id before
// using it in a path). All functions are pure and thread-safe.
namespace Ids {

// Profile ids are single path components: ^[A-Za-z0-9_-]{1,64}$.
bool isValidProfileId(const QString &id);

// Flatpak application id (reverse DNS, at least two dots-separated parts,
// <= 255 chars), e.g. "tv.kodi.Kodi".
bool isValidFlatpakId(const QString &id);

// Desktop file id as derived from a file name, e.g. "org.gnome.Foo.desktop"
// or "kde-bar.desktop": printable ASCII without '/', no leading '.'.
bool isValidDesktopFileId(const QString &id);

// Manifest app id (the part after "app:"): ^[A-Za-z0-9._-]{1,64}$, no
// leading '.'.
bool isValidManifestId(const QString &id);

// Icon theme name (Icon= without a path): no '/', no leading '.', no
// control characters, <= 128 chars.
bool isValidIconName(const QString &name);

// An absolute, already-clean path (no "..", "." or "//" components) of at
// most 4096 characters.
bool isCleanAbsolutePath(const QString &path);

// Item keys (favorites, history): non-empty, <= 512 chars, no whitespace
// or control characters. Keys are never used as paths.
bool isValidItemKey(const QString &key);

} // namespace Ids
