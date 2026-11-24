#pragma once

#include <QString>
#include <optional>

// The user's SteamGridDB API key (PLAN D11). Never logged, never put in
// argv or the environment, only ever sent in the Authorization header to
// the SteamGridDB API host.
//
// Backends:
//  - libsecret (Secret Service), if MightyOS was built with it
//    (MIGHTYOS_HAVE_LIBSECRET) and a Secret Service answers at run time.
//    Calls can block (a locked keyring may prompt), so call this class from
//    a worker thread, never the GUI thread.
//  - otherwise a file, <config>/secrets/steamgriddb.key:
//      directory created 0700 (never a symlink, owned by us, no group or
//      other access), the file written as a fresh O_CREAT|O_EXCL|O_NOFOLLOW
//      0600 temporary that is fsync'd, verified with fstat (regular, ours,
//      mode 0600) and renamed over the old one. Reads re-check all of that
//      on the opened fd and refuse a file other users could read.
//
// Keys are 16..128 ASCII letters/digits (SteamGridDB issues 32 hex
// digits); surrounding whitespace is trimmed.
class ApiKeyStore
{
public:
    enum class Backend { None, Secret, File };
    static constexpr qint64 kMaxFileBytes = 1024;

    // `configDir`: $XDG_CONFIG_HOME/mightyos. `useSecretService` false
    // forces the file backend (tests).
    explicit ApiKeyStore(const QString &configDir, bool useSecretService = true);

    static bool isValidKey(const QString &key);
    static QString normalized(const QString &key);
    // "••••••••1a2b": never more than the last four characters.
    static QString masked(const QString &key);
    static bool builtWithSecretService();

    QString filePath() const;

    // nullopt: no key (or an unusable one; `*error` says why, without the key).
    std::optional<QString> read(QString *error = nullptr) const;
    bool write(const QString &key, QString *error = nullptr);
    bool clear();
    // Which backend the last successful read/write used.
    Backend lastBackend() const { return m_lastBackend; }

private:
    bool ensureDir(QString *error) const;
    std::optional<QString> readFile(QString *error) const;
    bool writeFile(const QString &key, QString *error) const;

    QString m_configDir;
    bool m_useSecret;
    mutable Backend m_lastBackend = Backend::None;
};
