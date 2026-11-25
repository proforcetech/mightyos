#include "moonlight.h"

#include "jsonfile.h"
#include "textsanitize.h"

#include <QCoreApplication>
#include <QHash>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <cerrno>

Q_LOGGING_CATEGORY(lcMoonlight, "mightyos.moonlight", QtWarningMsg)

namespace Moonlight {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Moonlight", text);
}

const QString kPrefix = QStringLiteral("moonlight:");

// QSettings INI key: '\' is the group separator, %XX / %UXXXX escapes.
QString unescapeKey(const QByteArray &raw)
{
    QString out;
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const char c = raw.at(i);
        if (c == '\\') {
            out.append(QLatin1Char('/'));
        } else if (c == '%' && i + 1 < raw.size() && raw.at(i + 1) == 'U' && i + 5 < raw.size()) {
            bool ok = false;
            const uint code = raw.mid(i + 2, 4).toUInt(&ok, 16);
            out.append(ok ? QChar(char16_t(code)) : QChar::ReplacementCharacter);
            i += 5;
        } else if (c == '%' && i + 2 < raw.size()) {
            bool ok = false;
            const uint code = raw.mid(i + 1, 2).toUInt(&ok, 16);
            out.append(ok ? QChar(char16_t(code)) : QChar::ReplacementCharacter);
            i += 2;
        } else {
            out.append(QLatin1Char(c));
        }
    }
    return out;
}

int hexValue(QChar c)
{
    if (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
        return c.unicode() - '0';
    if (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
        return c.unicode() - 'a' + 10;
    if (c >= QLatin1Char('A') && c <= QLatin1Char('F'))
        return c.unicode() - 'A' + 10;
    return -1;
}

bool uuidLike(const QString &id)
{
    static const QRegularExpression re(QStringLiteral(
        "^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$"));
    return re.match(id).hasMatch();
}

} // namespace

bool isValidHostId(const QString &id)
{
    if (id.isEmpty() || id.size() > 253 || id.startsWith(QLatin1Char('-')))
        return false;
    if (uuidLike(id))
        return true;
    static const QRegularExpression name(QStringLiteral(
        "^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$"));
    if (!name.match(id).hasMatch())
        return false;
    // Ends in a number: only a strict dotted quad (no "127.1", "0x7f.1",
    // "0177.0.0.1", leading zeros), never a legacy IPv4 form.
    static const QRegularExpression numeric(QStringLiteral("(^|\\.)([0-9]+|0[xX][0-9A-Fa-f]*)$"));
    static const QRegularExpression quad(QStringLiteral(
        "^((25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])\\.){3}(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])$"));
    return !numeric.match(id).hasMatch() || quad.match(id).hasMatch();
}

bool isValidAppName(const QString &name)
{
    if (name.isEmpty() || name.size() > kMaxAppName || name.startsWith(QLatin1Char('-')))
        return false;
    return TextSanitize::label(name, kMaxAppName) == name;
}

bool parseIniString(const QByteArray &raw, QString *out)
{
    const QString text = QString::fromUtf8(raw);
    QString value;
    bool inQuotes = false;
    bool quoted = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            quoted = true;
            continue;
        }
        if (c == QLatin1Char('\\') && i + 1 < text.size()) {
            const QChar n = text.at(++i);
            switch (n.unicode()) {
            case 'a': value.append(QChar(0x07)); break;
            case 'b': value.append(QChar(0x08)); break;
            case 'f': value.append(QChar(0x0c)); break;
            case 'n': value.append(QLatin1Char('\n')); break;
            case 'r': value.append(QLatin1Char('\r')); break;
            case 't': value.append(QLatin1Char('\t')); break;
            case 'v': value.append(QChar(0x0b)); break;
            case 'x': {
                uint code = 0;
                int digits = 0;
                while (digits < 4 && i + 1 < text.size() && hexValue(text.at(i + 1)) >= 0) {
                    code = code * 16 + uint(hexValue(text.at(++i)));
                    ++digits;
                }
                value.append(QChar(char16_t(code)));
                break;
            }
            default:
                if (n >= QLatin1Char('0') && n <= QLatin1Char('7')) {
                    uint code = uint(n.unicode() - '0');
                    for (int d = 0; d < 2 && i + 1 < text.size() && text.at(i + 1) >= QLatin1Char('0')
                                    && text.at(i + 1) <= QLatin1Char('7'); ++d)
                        code = code * 8 + uint(text.at(++i).unicode() - '0');
                    value.append(QChar(char16_t(code)));
                } else {
                    value.append(n);   // \" \\ \? \' and anything else: literal
                }
                break;
            }
            continue;
        }
        if (!inQuotes && c == QLatin1Char(','))
            return false;   // a string list, not a string
        value.append(c);
    }
    if (!quoted)
        value = value.trimmed();
    if (value.startsWith(QLatin1String("@@")))
        value.remove(0, 1);
    else if (value.startsWith(QLatin1Char('@')))
        return false;   // @ByteArray(...), @Variant(...)
    *out = value;
    return true;
}

QList<MoonlightHost> parseConfig(const QByteArray &ini)
{
    QHash<QString, QByteArray> values;   // "1/uuid" -> raw value, [hosts] only
    bool inHosts = false;
    for (QByteArray line : ini.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(';') || trimmed.startsWith('#'))
            continue;
        if (trimmed.startsWith('[')) {
            inHosts = trimmed == "[hosts]";
            continue;
        }
        if (!inHosts)
            continue;
        const qsizetype eq = line.indexOf('=');
        if (eq <= 0 || values.size() >= 20000)
            continue;
        values.insert(unescapeKey(line.left(eq).trimmed()), line.mid(eq + 1));
    }
    auto string = [&values](const QString &key) {
        QString out;
        const auto found = values.constFind(key);
        if (found == values.cend() || !parseIniString(*found, &out))
            return QString();
        return out;
    };
    auto count = [&string](const QString &key, int max) {
        bool ok = false;
        const int n = string(key).toInt(&ok);
        return ok && n > 0 ? qMin(n, max) : 0;
    };

    QList<MoonlightHost> hosts;
    QSet<QString> seen;
    const int hostCount = count(QStringLiteral("size"), kMaxHosts);
    for (int h = 1; h <= hostCount; ++h) {
        const QString prefix = QString::number(h) + QLatin1Char('/');
        MoonlightHost host;
        const QString uuid = string(prefix + QStringLiteral("uuid"));
        const QString name = string(prefix + QStringLiteral("hostname"));
        if (uuidLike(uuid))
            host.id = uuid.toLower();
        else if (isValidHostId(name))
            host.id = name.toLower();   // keys and allow-lists use one case
        else
            continue;
        if (seen.contains(host.id.toLower()))
            continue;
        seen.insert(host.id.toLower());
        host.name = TextSanitize::label(name, 48);
        if (host.name.isEmpty())
            host.name = host.id;
        const int appCount = count(prefix + QStringLiteral("apps/size"), kMaxAppsPerHost);
        QSet<QString> appNames;
        for (int a = 1; a <= appCount; ++a) {
            const QString appPrefix = prefix + QStringLiteral("apps/") + QString::number(a) + QLatin1Char('/');
            MoonlightApp app;
            app.name = string(appPrefix + QStringLiteral("name"));
            app.hidden = string(appPrefix + QStringLiteral("hidden")) == QLatin1String("true");
            // Keys must fit (keys are <= 512 chars).
            if (!isValidAppName(app.name) || appNames.contains(app.name.toLower())
                || appKey(host.id, app.name).size() > 512)
                continue;
            appNames.insert(app.name.toLower());
            host.apps.append(app);
        }
        hosts.append(host);
    }
    return hosts;
}

QString canonicalKey(const QString &key)
{
    QString host, app;
    if (!parseKey(key, &host, &app))
        return key;
    return app.isEmpty() ? hostKey(host.toLower()) : appKey(host.toLower(), app);
}

QString hostKey(const QString &hostId)
{
    return isValidHostId(hostId) ? kPrefix + hostId : QString();
}

QString appKey(const QString &hostId, const QString &appName)
{
    if (!isValidHostId(hostId) || !isValidAppName(appName))
        return {};
    return kPrefix + hostId + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(appName));
}

bool parseKey(const QString &key, QString *hostId, QString *app)
{
    if (!key.startsWith(kPrefix) || key.size() > 512)
        return false;
    const QString rest = key.mid(kPrefix.size());
    const qsizetype slash = rest.indexOf(QLatin1Char('/'));
    const QString host = slash < 0 ? rest : rest.left(slash);
    if (!isValidHostId(host))
        return false;
    QString name;
    if (slash >= 0) {
        const QString encoded = rest.mid(slash + 1);
        for (const QChar c : encoded) {
            if (c.unicode() > 0x7e || c.unicode() <= 0x20)
                return false;
        }
        name = QUrl::fromPercentEncoding(encoded.toLatin1());
        // Canonical encoding only: one key per app.
        if (!isValidAppName(name) || QString::fromLatin1(QUrl::toPercentEncoding(name)) != encoded)
            return false;
    }
    if (hostId)
        *hostId = host;
    if (app)
        *app = name;
    return true;
}

QList<ConfigFile> configFiles(const QString &home, const QString &configHome)
{
    const QString rel = QStringLiteral("/Moonlight Game Streaming Project/Moonlight.conf");
    QList<ConfigFile> files;
    if (!home.isEmpty())
        files.append({ home + QStringLiteral("/.var/app/") + kFlatpakId + QStringLiteral("/config") + rel, true });
    if (!configHome.isEmpty())
        files.append({ configHome + rel, false });
    return files;
}

bool installed(const LaunchEnv &env, bool *flatpak)
{
    const bool fp = !env.flatpakProgram().isEmpty() && env.flatpakInstalled(kFlatpakId);
    if (flatpak)
        *flatpak = fp;
    return fp || !env.findProgram(QStringLiteral("moonlight")).isEmpty();
}

QList<MoonlightHost> readHosts(const QList<ConfigFile> &files, const LaunchEnv &env)
{
    bool flatpak = false;
    const bool any = installed(env, &flatpak);
    const bool native = any && !env.findProgram(QStringLiteral("moonlight")).isEmpty();
    QList<MoonlightHost> hosts;
    QSet<QString> seen;
    for (const ConfigFile &file : files) {
        if ((file.flatpak && !flatpak) || (!file.flatpak && !native))
            continue;
        int error = 0;
        const auto data = JsonFile::readRegularFile(file.path, kMaxConfigBytes, false, &error);
        if (!data) {
            if (error != ENOENT)
                qCWarning(lcMoonlight) << "Moonlight settings unreadable or too large, errno" << error;
            continue;
        }
        for (MoonlightHost &host : parseConfig(*data)) {
            if (seen.contains(host.id.toLower()) || hosts.size() >= kMaxHosts)
                continue;
            seen.insert(host.id.toLower());
            host.flatpak = file.flatpak;
            hosts.append(host);
        }
    }
    return hosts;
}

LaunchPlan planStream(const QString &hostId, const QString &app, bool preferFlatpak, const LaunchEnv &env)
{
    LaunchPlan p;
    p.emulatorId = QStringLiteral("moonlight");
    if (!isValidHostId(hostId) || !isValidAppName(app)) {
        p.error = tr("This stream can't be started.");
        return p;
    }
    const QString native = env.findProgram(QStringLiteral("moonlight"));
    const bool flatpak = !env.flatpakProgram().isEmpty() && env.flatpakInstalled(kFlatpakId);
    if (flatpak && (preferFlatpak || native.isEmpty()))
        p.argv = { env.flatpakProgram(), QStringLiteral("run"), QStringLiteral("--die-with-parent"), kFlatpakId };
    else if (!native.isEmpty())
        p.argv = { native };
    else {
        p.error = tr("Install Moonlight from the App Store to stream games.");
        p.install = QStringLiteral("Moonlight");
        return p;
    }
    p.argv << QStringLiteral("stream") << QStringLiteral("--") << hostId << app;
    return p;
}

} // namespace Moonlight
