#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

// Layered JSON definition directories (systems.d, webapps.d), lowest
// priority first:
//   1. built in (a qrc dir, ":/mightyos/<name>"),
//   2. $XDG_DATA_DIRS/mightyos/<name> (installed: /usr/share/...),
//   3. $XDG_CONFIG_HOME/mightyos/<name> (the user's overrides).
//
// Definitions decide which programs run and with what arguments, so files
// outside qrc are only read when the file *and* its directory belong to us
// or root and are not writable by other users. Group write is accepted only
// for our own primary group (Ubuntu: private user groups, umask 002).
// Files are read with JsonFile::readRegularFile: no symlinks, regular files
// only, size-capped, all checks on the opened fd.
namespace DefinitionLayers {

struct File {
    QString name;                      // "<id>.json"
    QString path;
    std::optional<QByteArray> data;    // nullopt: `error` says why
    QString error;
};

struct Layer {
    QString dir;
    bool exists = false;
    QString dirError;                  // non-empty: the whole dir was ignored
    QList<File> files;                 // sorted by name, at most maxFiles
};

bool trustedOwnerAndMode(uint owner, uint group, uint mode);

// The three layers for `name` ("systems.d") with the built-in qrc dir first.
QStringList defaultDirs(const QString &builtinDir, const QString &name);

// Reads every *.json in `dir` (a qrc dir or a clean absolute path).
Layer read(const QString &dir, int maxFiles, qint64 maxBytes);

} // namespace DefinitionLayers
