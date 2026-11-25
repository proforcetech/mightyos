#include "webapps.h"

#include "definitionlayers.h"
#include "ids.h"
#include "textsanitize.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>

#include <sys/stat.h>

Q_LOGGING_CATEGORY(lcWebApps, "mightyos.webapps", QtWarningMsg)

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("WebApps", text);
}

bool isCleanLabel(const QString &text, int maxChars)
{
    return !text.isEmpty() && text.size() <= maxChars && TextSanitize::label(text, maxChars) == text;
}

// A browser parses a host whose last label is a number (decimal, or 0x
// hex) as an IPv4 address in any of its legacy forms ("127.1", "0x7f.1",
// "0177.0.0.1"): such hosts are never DNS names here.
bool endsInNumber(const QString &host)
{
    static const QRegularExpression re(QStringLiteral("(^|\\.)([0-9]+|0x[0-9a-f]*)\\.?$"));
    return re.match(host).hasMatch();
}

bool isDnsName(const QString &host)
{
    static const QRegularExpression re(QStringLiteral(
        "^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+$"));
    return host.size() <= 253 && re.match(host).hasMatch() && !endsInNumber(host);
}

bool isIpv4(const QString &host)
{
    static const QRegularExpression re(QStringLiteral(
        "^((25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])\\.){3}(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])$"));
    return re.match(host).hasMatch();
}

bool hostAllowed(const QString &host, const QStringList &patterns)
{
    if (patterns.isEmpty())
        return true;
    for (const QString &p : patterns) {
        if (p == host)
            return true;
        if (p.startsWith(QLatin1String("*.")) && host.endsWith(p.mid(1)) && host.size() > p.size() - 1)
            return true;
    }
    return false;
}

bool keysAllowed(const QJsonObject &o, std::initializer_list<const char *> allowed, QString *bad)
{
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool ok = false;
        for (const char *a : allowed)
            ok = ok || it.key() == QLatin1String(a);
        if (!ok) {
            *bad = it.key();
            return false;
        }
    }
    return true;
}

const QStringList kBrowserIds { QStringLiteral("auto"), QStringLiteral("chrome"), QStringLiteral("chromium"),
                                QStringLiteral("brave"), QStringLiteral("edge") };

} // namespace

// --- definitions -------------------------------------------------------------

QStringList WebAppRegistry::defaultDirs()
{
    return DefinitionLayers::defaultDirs(kBuiltinDir, QStringLiteral("webapps.d"));
}

bool WebAppRegistry::isValidId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,31}$"));
    return re.match(id).hasMatch();
}

bool WebAppRegistry::isValidHostPattern(const QString &pattern)
{
    if (pattern.startsWith(QLatin1String("*.")))
        return isDnsName(pattern.mid(2));
    return isDnsName(pattern) || isIpv4(pattern);
}

bool WebAppRegistry::isValidHost(const QString &host)
{
    return isDnsName(host) || isIpv4(host);
}

bool WebAppRegistry::isValidUserAgent(const QString &ua)
{
    if (ua.isEmpty() || ua.size() > 256 || ua.startsWith(QLatin1Char('-')))
        return false;
    for (const QChar c : ua) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7e)
            return false;
    }
    return true;
}

bool WebAppRegistry::isValidUrl(const QString &url, const QStringList &hosts, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (url.isEmpty() || url.size() > kMaxUrlLength)
        return fail(tr("The address is empty or too long."));
    for (const QChar c : url) {
        const ushort u = c.unicode();
        if (u <= 0x20 || u >= 0x7f || c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('\\')
            || c == QLatin1Char('<') || c == QLatin1Char('>') || c == QLatin1Char('`'))
            return fail(tr("The address contains spaces or characters that aren't allowed."));
    }
    if (!url.startsWith(QLatin1String("https://")))
        return fail(tr("Only secure https:// addresses are allowed."));
    const QString rest = url.mid(8);
    qsizetype end = rest.size();
    for (const QChar stop : { QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#') }) {
        const qsizetype at = rest.indexOf(stop);
        if (at >= 0 && at < end)
            end = at;
    }
    const QString authority = rest.left(end);
    if (authority.contains(QLatin1Char('@')))
        return fail(tr("Addresses with a user name or password are not allowed."));
    QString host = authority;
    if (const qsizetype colon = authority.indexOf(QLatin1Char(':')); colon >= 0) {
        host = authority.left(colon);
        const QString port = authority.mid(colon + 1);
        bool ok = false;
        const int number = port.toInt(&ok);
        static const QRegularExpression digits(QStringLiteral("^[0-9]{1,5}$"));
        if (!digits.match(port).hasMatch() || !ok || number < 1 || number > 65535)
            return fail(tr("The address has an invalid port."));
    }
    if (!isDnsName(host) && !isIpv4(host))
        return fail(tr("The address needs a full host name like www.example.com (single names such as "
                       "\"jellyfin\" or \"localhost\" don't work) or an IP address like 192.168.0.10."));
    const QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.scheme() != QLatin1String("https") || !parsed.userInfo().isEmpty()
        || parsed.host() != host)
        return fail(tr("That isn't a valid web address."));
    if (!hostAllowed(host, hosts))
        return fail(tr("%1 is not one of this tile's allowed hosts.").arg(host));
    return true;
}

QString WebAppRegistry::hostOf(const QString &url)
{
    if (!isValidUrl(url, {}))
        return {};
    return QUrl(url, QUrl::StrictMode).host();
}

std::optional<WebAppDef> WebAppRegistry::parse(const QByteArray &json, QString *error, bool *disabled)
{
    auto fail = [error](const QString &why) -> std::optional<WebAppDef> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    if (disabled)
        *disabled = false;
    if (json.size() > kMaxFileBytes)
        return fail(QStringLiteral("file too large"));
    QJsonParseError parseError {};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("not a JSON object"));
    const QJsonObject o = doc.object();
    if (!o.value(QStringLiteral("version")).isDouble() || o.value(QStringLiteral("version")).toInt(-1) != kVersion)
        return fail(QStringLiteral("unsupported version"));
    WebAppDef app;
    app.id = o.value(QStringLiteral("id")).toString();
    if (!isValidId(app.id))
        return fail(QStringLiteral("invalid id"));

    QString bad;
    if (o.contains(QStringLiteral("disabled"))) {
        if (!keysAllowed(o, { "version", "id", "disabled" }, &bad) || !o.value(QStringLiteral("disabled")).isBool()
            || !o.value(QStringLiteral("disabled")).toBool())
            return fail(QStringLiteral("a disabled stub is { version, id, \"disabled\": true }"));
        if (disabled)
            *disabled = true;
        return app;
    }
    if (!keysAllowed(o, { "version", "id", "label", "subtitle", "url", "hosts", "icon", "tint", "order", "browser",
                          "drm", "userAgent", "kidsHosts", "kidsMap", "comment" }, &bad))
        return fail(QStringLiteral("unknown key \"%1\"").arg(bad.left(40)));
    for (const char *key : { "label", "subtitle", "url", "icon", "tint", "browser", "userAgent", "comment" }) {
        if (o.contains(QLatin1String(key)) && !o.value(QLatin1String(key)).isString())
            return fail(QStringLiteral("\"%1\" must be a string").arg(QLatin1String(key)));
    }
    app.label = o.value(QStringLiteral("label")).toString();
    if (!isCleanLabel(app.label, 32))
        return fail(QStringLiteral("invalid label"));
    app.subtitle = o.value(QStringLiteral("subtitle")).toString();
    if (!app.subtitle.isEmpty() && !isCleanLabel(app.subtitle, 64))
        return fail(QStringLiteral("invalid subtitle"));
    app.icon = o.value(QStringLiteral("icon")).toString(QStringLiteral("tv"));
    static const QRegularExpression iconRe(QStringLiteral("^[a-z0-9-]{1,32}$"));
    if (!iconRe.match(app.icon).hasMatch())
        return fail(QStringLiteral("invalid icon"));
    app.tint = o.value(QStringLiteral("tint")).toString();
    static const QRegularExpression tintRe(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    if (!app.tint.isEmpty() && !tintRe.match(app.tint).hasMatch())
        return fail(QStringLiteral("invalid tint"));
    if (o.contains(QStringLiteral("order"))) {
        const QJsonValue order = o.value(QStringLiteral("order"));
        if (!order.isDouble() || order.toDouble() != double(order.toInt()) || order.toInt() < 0
            || order.toInt() > 100000)
            return fail(QStringLiteral("order must be an integer 0..100000"));
        app.order = order.toInt();
    }
    app.browser = o.value(QStringLiteral("browser")).toString(QStringLiteral("auto"));
    if (!kBrowserIds.contains(app.browser))
        return fail(QStringLiteral("browser must be auto, chrome, chromium, brave or edge"));
    if (o.contains(QStringLiteral("drm"))) {
        if (!o.value(QStringLiteral("drm")).isBool())
            return fail(QStringLiteral("\"drm\" must be true or false"));
        app.drm = o.value(QStringLiteral("drm")).toBool();
    }
    if (app.drm && app.browser != QLatin1String("auto") && !WebAppLaunch::browser(app.browser)->widevine)
        return fail(QStringLiteral("a DRM service needs a browser with Widevine (not %1)").arg(app.browser));
    const QJsonValue hosts = o.value(QStringLiteral("hosts"));
    if (!hosts.isArray() || hosts.toArray().isEmpty() || hosts.toArray().size() > kMaxHosts)
        return fail(QStringLiteral("hosts: 1..%1 host names").arg(kMaxHosts));
    for (const QJsonValue &h : hosts.toArray()) {
        if (!h.isString() || !isValidHostPattern(h.toString()))
            return fail(QStringLiteral("hosts: invalid host name"));
        if (!app.hosts.contains(h.toString()))
            app.hosts.append(h.toString());
    }
    app.url = o.value(QStringLiteral("url")).toString();
    QString urlError;
    if (!isValidUrl(app.url, app.hosts, &urlError))
        return fail(QStringLiteral("url: ") + urlError);
    if (o.contains(QStringLiteral("userAgent"))) {
        app.userAgent = o.value(QStringLiteral("userAgent")).toString();
        if (!isValidUserAgent(app.userAgent))
            return fail(QStringLiteral("invalid userAgent"));
    }
    if (o.contains(QStringLiteral("kidsHosts"))) {
        const QJsonValue kh = o.value(QStringLiteral("kidsHosts"));
        if (!kh.isArray() || kh.toArray().size() > 2 * kMaxHosts)
            return fail(QStringLiteral("kidsHosts: at most %1 host names").arg(2 * kMaxHosts));
        for (const QJsonValue &h : kh.toArray()) {
            if (!h.isString() || !isValidHostPattern(h.toString()))
                return fail(QStringLiteral("kidsHosts: invalid host name"));
            if (!app.kidsHosts.contains(h.toString()))
                app.kidsHosts.append(h.toString());
        }
    }
    if (o.contains(QStringLiteral("kidsMap"))) {
        const QJsonValue km = o.value(QStringLiteral("kidsMap"));
        if (!km.isObject() || km.toObject().size() > kMaxHosts)
            return fail(QStringLiteral("kidsMap: an object of at most %1 host names").arg(kMaxHosts));
        const QJsonObject map = km.toObject();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (!isDnsName(it.key()) || !it.value().isString() || !isDnsName(it.value().toString()))
                return fail(QStringLiteral("kidsMap: host names only"));
            // An EXCLUDE (allowed host) covering a mapped name would undo the mapping.
            if (hostAllowed(it.key(), app.kidsHosts))
                return fail(QStringLiteral("kidsMap: %1 is also covered by kidsHosts").arg(it.key()));
            app.kidsMap.append({ it.key(), it.value().toString() });
        }
    }
    return app;
}

QByteArray WebAppRegistry::userDefinition(const QString &label, const QString &url, const QStringList &taken,
                                          QString *id, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return QByteArray();
    };
    const QString name = label.trimmed();
    if (!isCleanLabel(name, 32))
        return fail(tr("Give the tile a name of 1 to 32 characters."));
    QString address = url.trimmed();
    if (address.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
        return fail(tr("Only secure https:// addresses are allowed."));
    if (!address.contains(QLatin1String("://")))
        address.prepend(QStringLiteral("https://"));
    // Scheme and host are case-insensitive: "HTTPS://Jelly.Example.org/Web" is fine.
    if (const qsizetype sep = address.indexOf(QLatin1String("://")); sep > 0) {
        qsizetype end = address.size();
        for (const QChar stop : { QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#') }) {
            const qsizetype at = address.indexOf(stop, sep + 3);
            if (at >= 0 && at < end)
                end = at;
        }
        address = address.left(end).toLower() + address.mid(end);
    }
    QString urlError;
    if (!isValidUrl(address, {}, &urlError))
        return fail(urlError);
    const QString host = hostOf(address);

    QString slug;
    for (const QChar c : name.toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')))
            slug.append(c);
        else if (!slug.endsWith(QLatin1Char('-')))
            slug.append(QLatin1Char('-'));
    }
    while (slug.startsWith(QLatin1Char('-')))
        slug.remove(0, 1);
    slug = slug.left(20);
    while (slug.endsWith(QLatin1Char('-')))
        slug.chop(1);
    if (slug.isEmpty())
        slug = QStringLiteral("web");
    QString candidate = kUserPrefix + slug;
    for (int n = 2; taken.contains(candidate); ++n) {
        if (n > 99)
            return fail(tr("Too many tiles with that name."));
        candidate = kUserPrefix + slug + QLatin1Char('-') + QString::number(n);
    }
    if (!isValidId(candidate))
        return fail(tr("Give the tile a different name."));
    const QJsonObject o {
        { QStringLiteral("version"), kVersion }, { QStringLiteral("id"), candidate },
        { QStringLiteral("label"), name }, { QStringLiteral("url"), address },
        { QStringLiteral("hosts"), QJsonArray { host } }, { QStringLiteral("icon"), QStringLiteral("web") },
        { QStringLiteral("tint"), QStringLiteral("#5a8dee") }, { QStringLiteral("order"), 5000 },
        { QStringLiteral("browser"), QStringLiteral("auto") }, { QStringLiteral("drm"), false },
        { QStringLiteral("comment"), QStringLiteral("Added in MightyOS Settings > Media") },
    };
    const QByteArray json = QJsonDocument(o).toJson();
    if (!parse(json, error))   // the same rules as any other definition
        return {};
    if (id)
        *id = candidate;
    return json;
}

QByteArray WebAppRegistry::disabledStub(const QString &id)
{
    if (!isValidId(id))
        return {};
    return QJsonDocument(QJsonObject { { QStringLiteral("version"), kVersion }, { QStringLiteral("id"), id },
                                       { QStringLiteral("disabled"), true } }).toJson();
}

void WebAppRegistry::load(const QStringList &dirs, const QString &userDir)
{
    QMap<QString, WebAppDef> byId;
    m_errors.clear();
    for (const QString &dir : dirs) {
        const bool userLayer = !userDir.isEmpty() && dir == userDir;
        const DefinitionLayers::Layer layer = DefinitionLayers::read(dir, kMaxFilesPerDir, kMaxFileBytes);
        if (!layer.dirError.isEmpty()) {
            const QString message = QStringLiteral("%1: %2").arg(dir, layer.dirError);
            m_errors.append(message);
            qCWarning(lcWebApps).noquote() << "ignoring web app definitions in" << message;
            continue;
        }
        for (const DefinitionLayers::File &file : layer.files) {
            QString error = file.error;
            bool disabled = false;
            std::optional<WebAppDef> def;
            if (file.data)
                def = parse(*file.data, &error, &disabled);
            if (def && file.name != def->id + QStringLiteral(".json")) {
                def.reset();
                error = QStringLiteral("file name must be <id>.json");
            }
            if (!def) {
                const QString message = QStringLiteral("%1: %2").arg(file.path, error);
                m_errors.append(message);
                qCWarning(lcWebApps).noquote() << "ignoring web app definition" << message;
                continue;
            }
            if (disabled) {
                // Hidden by the user: kept (hidden) so Settings can show it again.
                auto it = byId.find(def->id);
                if (it != byId.end() && userLayer && !it->user)
                    it->hidden = true;
                else if (it != byId.end())
                    byId.erase(it);
                continue;
            }
            def->source = file.path;
            def->user = userLayer;
            def->shadowsLower = userLayer && byId.contains(def->id);
            byId.insert(def->id, *def);
        }
    }
    m_apps = byId.values();
    std::stable_sort(m_apps.begin(), m_apps.end(), [](const WebAppDef &a, const WebAppDef &b) {
        return a.order != b.order ? a.order < b.order : a.label.compare(b.label, Qt::CaseInsensitive) < 0;
    });
    m_index.clear();
    for (int i = 0; i < m_apps.size(); ++i)
        m_index.insert(m_apps.at(i).id, i);
}

const WebAppDef *WebAppRegistry::find(const QString &id) const
{
    const auto found = m_index.constFind(id);
    return found == m_index.cend() ? nullptr : &m_apps.at(found.value());
}

// --- launching ---------------------------------------------------------------

namespace WebAppLaunch {

const QList<BrowserDef> &browsers()
{
    static const QList<BrowserDef> list {
        { QStringLiteral("chrome"), QStringLiteral("Google Chrome"), QStringLiteral("com.google.Chrome"),
          { QStringLiteral("/opt/google/chrome/google-chrome"), QStringLiteral("/usr/bin/google-chrome-stable") }, true },
        { QStringLiteral("chromium"), QStringLiteral("Chromium"), QStringLiteral("org.chromium.Chromium"),
          { QStringLiteral("/usr/bin/chromium"), QStringLiteral("/usr/bin/chromium-browser") }, false },
        { QStringLiteral("brave"), QStringLiteral("Brave"), QStringLiteral("com.brave.Browser"),
          { QStringLiteral("/opt/brave.com/brave/brave-browser"), QStringLiteral("/usr/bin/brave-browser") }, true },
        { QStringLiteral("edge"), QStringLiteral("Microsoft Edge"), QStringLiteral("com.microsoft.Edge"),
          { QStringLiteral("/opt/microsoft/msedge/microsoft-edge"), QStringLiteral("/usr/bin/microsoft-edge-stable") }, true },
    };
    return list;
}

const BrowserDef *browser(const QString &id)
{
    for (const BrowserDef &b : browsers()) {
        if (b.id == id)
            return &b;
    }
    return nullptr;
}

QStringList order(const WebAppDef &app)
{
    QStringList ids = app.drm
        ? QStringList { QStringLiteral("chrome"), QStringLiteral("edge"), QStringLiteral("brave") }
        : QStringList { QStringLiteral("chrome"), QStringLiteral("chromium"), QStringLiteral("brave"),
                        QStringLiteral("edge") };
    if (app.browser != QLatin1String("auto") && ids.contains(app.browser)) {
        ids.removeAll(app.browser);
        ids.prepend(app.browser);
    }
    return ids;
}

BrowserChoice choose(const WebAppDef &app, const LaunchEnv &env)
{
    const QString flatpak = env.flatpakProgram();
    for (const QString &id : order(app)) {
        const BrowserDef *b = browser(id);
        if (!b || (app.drm && !b->widevine))
            continue;
        if (!flatpak.isEmpty() && env.flatpakInstalled(b->flatpakId))
            return { b, true, flatpak };
        for (const QString &path : b->paths) {
            if (const QString program = trustedNativeBrowser(path, env); !program.isEmpty())
                return { b, false, program };
        }
    }
    return {};
}

namespace {

bool rootOwnedNotWritable(const QString &path, uint uid)
{
    struct stat st {};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return false;
    return st.st_uid == uid && (st.st_mode & 022) == 0;
}

} // namespace

QString trustedNativeBrowser(const QString &path, const LaunchEnv &env)
{
    const QString full = env.systemRoot + path;
    const QFileInfo info(full);
    if (!Ids::isCleanAbsolutePath(path) || !info.isFile() || !info.isExecutable())
        return {};
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !canonical.startsWith(env.systemRoot + QLatin1Char('/')))
        return {};
    const QString real = canonical.mid(env.systemRoot.size());
    if (real == QLatin1String("/usr/bin/snap") || real.startsWith(QLatin1String("/snap/")))
        return {};
    // The file, its real directory and the link itself: root's, not writable by others.
    if (!rootOwnedNotWritable(canonical, env.trustedUid)
        || !rootOwnedNotWritable(QFileInfo(canonical).absolutePath(), env.trustedUid)
        || !rootOwnedNotWritable(info.absolutePath(), env.trustedUid))
        return {};
    // Wrapper scripts that hand over to a snap (Ubuntu's chromium-browser).
    QFile file(canonical);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray head = file.read(64 * 1024);
        if (head.startsWith("#!") && head.contains("/snap/"))
            return {};
    }
    return full;
}

QStringList flatpakSandboxArgs()
{
    return { QStringLiteral("--nodevice=all"), QStringLiteral("--device=dri"), QStringLiteral("--device=input"),
             QStringLiteral("--nofilesystem=host"), QStringLiteral("--nofilesystem=home"),
             QStringLiteral("--nofilesystem=xdg-documents"), QStringLiteral("--nofilesystem=xdg-download") };
}

QStringList kidsArgs(const WebAppDef &app)
{
    QStringList allowed = app.hosts + app.kidsHosts;
    QStringList mapped;
    QStringList rules;
    for (const auto &[from, to] : app.kidsMap) {
        rules.append(QStringLiteral("MAP ") + from + QLatin1Char(' ') + to);
        mapped.append(from);
        allowed.append(from);
        allowed.append(to);
    }
    allowed.removeDuplicates();
    rules.append(QStringLiteral("MAP * ~NOTFOUND"));
    for (const QString &host : std::as_const(allowed)) {
        if (!mapped.contains(host))
            rules.append(QStringLiteral("EXCLUDE ") + host);
    }
    // All parts are validated host names / patterns: no ',', ';' or spaces.
    return { QStringLiteral("--proxy-server=127.0.0.1:9"),
             QStringLiteral("--proxy-bypass-list=<-loopback>;") + allowed.join(QLatin1Char(';')),
             QStringLiteral("--host-resolver-rules=") + rules.join(QStringLiteral(", ")) };
}

QString userDataDir(const BrowserChoice &choice, const QString &profileId, const LaunchEnv &env)
{
    if (!choice.ok() || !Ids::isValidProfileId(profileId))
        return {};
    const QString dir = choice.flatpak
        ? (env.home.isEmpty() ? QString()
                              : env.home + QStringLiteral("/.var/app/") + choice.browser->flatpakId
                                    + QStringLiteral("/data/mightyos/profiles/") + profileId)
        : (env.dataHome.isEmpty() ? QString()
                                  : env.dataHome + QStringLiteral("/mightyos/profiles/") + profileId
                                        + QStringLiteral("/browsers/") + choice.browser->id);
    return Ids::isCleanAbsolutePath(dir) ? dir : QString();
}

LaunchPlan plan(const WebAppDef &app, const QString &profileId, const LaunchEnv &env, bool restricted)
{
    LaunchPlan p;
    p.emulatorId = QStringLiteral("browser");
    if (!Ids::isValidProfileId(profileId)) {
        p.error = tr("No profile is active, so %1 has no place for its logins.").arg(app.label);
        return p;
    }
    if (!WebAppRegistry::isValidUrl(app.url, app.hosts)
        || (!app.userAgent.isEmpty() && !WebAppRegistry::isValidUserAgent(app.userAgent))) {
        p.error = tr("%1 can't be opened: its address isn't allowed.").arg(app.label);
        return p;
    }
    const BrowserChoice choice = choose(app, env);
    if (!choice.ok()) {
        p.error = app.drm ? tr("Install Google Chrome from the App Store to watch %1.").arg(app.label)
                          : tr("Install Google Chrome or Chromium from the App Store to open %1.").arg(app.label);
        p.install = QStringLiteral("Google Chrome");
        return p;
    }
    const QString dataDir = userDataDir(choice, profileId, env);
    if (dataDir.isEmpty()) {
        p.error = tr("No browser data folder is available for this profile.");
        return p;
    }
    p.emulatorId = choice.browser->id;
    if (choice.flatpak)
        p.argv = QStringList { choice.program, QStringLiteral("run") } + flatpakSandboxArgs()
                 + QStringList { choice.browser->flatpakId };
    else
        p.argv = { choice.program };
    p.argv << QStringLiteral("--user-data-dir=") + dataDir
           << QStringLiteral("--kiosk")
           << QStringLiteral("--no-first-run")
           << QStringLiteral("--no-default-browser-check")
           // No keyring prompt in a kiosk session (logins stay in the per-profile dir).
           << QStringLiteral("--password-store=basic")
           << QStringLiteral("--hide-crash-restore-bubble")
           << QStringLiteral("--noerrdialogs")
           << QStringLiteral("--ozone-platform-hint=auto");
    if (restricted)
        p.argv << kidsArgs(app);
    if (!app.userAgent.isEmpty())
        p.argv << QStringLiteral("--user-agent=") + app.userAgent;
    p.argv << QStringLiteral("--app=") + app.url;
    return p;
}

} // namespace WebAppLaunch
