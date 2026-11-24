#include "apikeystore.h"

#include "ids.h"
#include "jsonfile.h"

#include <QByteArray>
#include <QFile>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MIGHTYOS_HAVE_LIBSECRET
// GLib uses "signals" as a struct member name; Qt defines it as a macro.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")
#endif

namespace {

#ifdef MIGHTYOS_HAVE_LIBSECRET
const SecretSchema *schema()
{
    static const SecretSchema s = {
        "org.mightyos.ApiKey", SECRET_SCHEMA_NONE,
        { { "service", SECRET_SCHEMA_ATTRIBUTE_STRING }, { nullptr, SecretSchemaAttributeType(0) } },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    return &s;
}
#endif

void wipe(QByteArray &bytes)
{
    // Best effort: the QString copies elsewhere can't be wiped.
    volatile char *p = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i)
        p[i] = 0;
    bytes.clear();
}

} // namespace

ApiKeyStore::ApiKeyStore(const QString &configDir, bool useSecretService)
    : m_configDir(configDir)
    , m_useSecret(useSecretService && builtWithSecretService())
{
}

bool ApiKeyStore::builtWithSecretService()
{
#ifdef MIGHTYOS_HAVE_LIBSECRET
    return true;
#else
    return false;
#endif
}

QString ApiKeyStore::normalized(const QString &key)
{
    return key.trimmed();
}

bool ApiKeyStore::isValidKey(const QString &key)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9]{16,128}$"));
    return re.match(key).hasMatch();
}

QString ApiKeyStore::masked(const QString &key)
{
    if (key.isEmpty())
        return {};
    return QString(8, QChar(0x2022)) + key.right(qMin<qsizetype>(4, key.size() / 4));
}

QString ApiKeyStore::filePath() const
{
    return m_configDir.isEmpty() ? QString() : m_configDir + QStringLiteral("/secrets/steamgriddb.key");
}

bool ApiKeyStore::ensureDir(QString *error) const
{
    auto fail = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return false;
    };
    if (!Ids::isCleanAbsolutePath(m_configDir))
        return fail("no config directory");
    const QByteArray base = QFile::encodeName(m_configDir);
    const QByteArray dir = base + "/secrets";
    // The config dir itself may not exist yet.
    if (::mkdir(base.constData(), 0700) != 0 && errno != EEXIST)
        return fail("cannot create the config directory");
    if (::mkdir(dir.constData(), 0700) != 0 && errno != EEXIST)
        return fail("cannot create the secrets directory");
    struct stat st {};
    if (::lstat(dir.constData(), &st) != 0 || !S_ISDIR(st.st_mode))
        return fail("the secrets directory is not a directory (or is a symlink)");
    if (st.st_uid != ::getuid())
        return fail("the secrets directory belongs to another user");
    if ((st.st_mode & 077) != 0 && ::chmod(dir.constData(), 0700) != 0)
        return fail("cannot restrict the secrets directory");
    return true;
}

std::optional<QString> ApiKeyStore::readFile(QString *error) const
{
    auto fail = [error](const char *why) -> std::optional<QString> {
        if (error)
            *error = QString::fromLatin1(why);
        return std::nullopt;
    };
    if (!Ids::isCleanAbsolutePath(m_configDir))
        return fail("no config directory");
    // The secrets dir without following a symlink, then the file relative
    // to it (openat): nothing can be swapped in between.
    const int dfd = ::open((QFile::encodeName(m_configDir) + "/secrets").constData(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0)
        return errno == ENOENT ? std::nullopt : fail("the secrets directory is not a directory (or is a symlink)");
    struct stat dst {};
    if (::fstat(dfd, &dst) != 0 || dst.st_uid != ::getuid() || (dst.st_mode & 077) != 0) {
        ::close(dfd);
        return fail("the secrets directory is not private; ignoring the key");
    }
    const int fd = ::openat(dfd, "steamgriddb.key", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    ::close(dfd);
    if (fd < 0)
        return errno == ENOENT ? std::nullopt : fail("cannot open the key file");
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 077) != 0
        || st.st_size > kMaxFileBytes) {
        ::close(fd);
        return fail("the key file is not a private regular file (expected mode 0600); ignoring it");
    }
    QByteArray bytes(kMaxFileBytes + 1, '\0');
    qint64 total = 0;
    for (;;) {
        const ssize_t n = ::read(fd, bytes.data() + total, size_t(bytes.size() - total));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        total += n;
        if (total > kMaxFileBytes)
            break;
    }
    ::close(fd);
    bytes.truncate(total);
    const QString key = normalized(QString::fromLatin1(bytes));
    wipe(bytes);
    if (!isValidKey(key))
        return fail("the key file does not contain a valid key");
    m_lastBackend = Backend::File;
    return key;
}

bool ApiKeyStore::writeFile(const QString &key, QString *error) const
{
    if (!ensureDir(error))
        return false;
    // 0700 dir checks, O_EXCL|O_NOFOLLOW 0600 temporary, fsync, renameat.
    QByteArray bytes = key.toLatin1() + '\n';
    const bool ok = JsonFile::writePrivate(m_configDir, { QStringLiteral("secrets") }, QStringLiteral("steamgriddb.key"),
                                           bytes, 0600, error);
    wipe(bytes);
    if (ok)
        m_lastBackend = Backend::File;
    return ok;
}

std::optional<QString> ApiKeyStore::read(QString *error) const
{
#ifdef MIGHTYOS_HAVE_LIBSECRET
    if (m_useSecret) {
        GError *gerror = nullptr;
        gchar *secret = secret_password_lookup_sync(schema(), nullptr, &gerror, "service", "steamgriddb", nullptr);
        if (gerror)
            g_error_free(gerror);   // no Secret Service: try the file
        if (secret) {
            const QString key = normalized(QString::fromUtf8(secret));
            secret_password_free(secret);
            if (isValidKey(key)) {
                m_lastBackend = Backend::Secret;
                return key;
            }
        }
    }
#endif
    return readFile(error);
}

bool ApiKeyStore::write(const QString &rawKey, QString *error)
{
    const QString key = normalized(rawKey);
    if (!isValidKey(key)) {
        if (error)
            *error = QStringLiteral("not a valid SteamGridDB API key");
        return false;
    }
#ifdef MIGHTYOS_HAVE_LIBSECRET
    if (m_useSecret) {
        GError *gerror = nullptr;
        const QByteArray utf8 = key.toUtf8();
        const gboolean stored = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT,
                                                           "MightyOS: SteamGridDB API key", utf8.constData(),
                                                           nullptr, &gerror, "service", "steamgriddb", nullptr);
        if (gerror)
            g_error_free(gerror);
        if (stored) {
            ::unlink(QFile::encodeName(filePath()).constData());   // one copy only
            m_lastBackend = Backend::Secret;
            return true;
        }
    }
#endif
    return writeFile(key, error);
}

bool ApiKeyStore::clear()
{
#ifdef MIGHTYOS_HAVE_LIBSECRET
    if (m_useSecret) {
        GError *gerror = nullptr;
        secret_password_clear_sync(schema(), nullptr, &gerror, "service", "steamgriddb", nullptr);
        if (gerror)
            g_error_free(gerror);
    }
#endif
    m_lastBackend = Backend::None;
    if (!Ids::isCleanAbsolutePath(m_configDir))
        return true;
    const int dfd = ::open((QFile::encodeName(m_configDir) + "/secrets").constData(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0)
        return errno == ENOENT;
    bool ok = ::unlinkat(dfd, "steamgriddb.key", 0) == 0 || errno == ENOENT;
    // Temporaries a crashed write may have left (old and new naming).
    if (DIR *d = ::fdopendir(::dup(dfd))) {
        while (struct dirent *e = ::readdir(d)) {
            const QByteArray name(e->d_name);
            if (name.startsWith("steamgriddb.key.tmp-") || name.startsWith(".steamgriddb.key.tmp-"))
                ok = ::unlinkat(dfd, name.constData(), 0) == 0 && ok;
        }
        ::closedir(d);
    }
    ::close(dfd);
    return ok;
}
