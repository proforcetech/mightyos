#pragma once

#include "emulatorlaunch.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <optional>

// Streaming services and other web apps as tiles (PLAN D10, M5), run in a
// Chromium-family browser in kiosk mode.
//
// Definitions: one JSON file per web app in webapps.d, layered like
// systems.d (DefinitionLayers: built in, $XDG_DATA_DIRS, then the user's
// $XDG_CONFIG_HOME/mightyos/webapps.d with the same ownership and
// permission checks; a later file with the same id replaces the earlier
// one, { "version": 1, "id": "x", "disabled": true } hides it).
// Validated strictly; unknown keys or any invalid field reject the file:
//   { "version": 1, "id": "netflix", "label": "Netflix",
//     "subtitle": "Movies and series",            (optional)
//     "url": "https://www.netflix.com/browse",
//     "hosts": ["www.netflix.com", "*.netflix.com"],
//     "icon": "tv", "tint": "#e50914", "order": 20,
//     "browser": "auto" | "chrome" | "chromium" | "brave" | "edge",
//     "drm": true,                                 (needs Widevine)
//     "userAgent": "Mozilla/5.0 (...)",            (optional)
//     "kidsHosts": ["*.nflxvideo.net"],            (optional, kids only)
//     "kidsMap": { "www.youtube.com": "restrict.youtube.com" },  (optional)
//     "comment": "..." }
//
// URL rules (isValidUrl): printable ASCII only (IDNs as punycode), at most
// 2048 chars, starts with "https://", no user name or password, a DNS host
// name (or a strict dotted-quad IPv4: no "127.1", "0x7f.1", "0177.0.0.1";
// a host whose last label is a number must be one) that the definition's
// `hosts` allowlist names exactly or through a "*.domain" entry, optional
// port, no backslashes, quotes or whitespace. A valid URL therefore never starts with '-' and can't be
// read as a browser flag; it is passed as the single argv element
// "--app=<url>" anyway.
//
// Standard profiles: the allowlist only governs the URL a tile *starts*;
// the browser can follow links anywhere. Kids profiles get network limits
// on top (WebAppLaunch::plan): only `hosts`, `kidsHosts` (the CDNs a
// service needs) and the `kidsMap` hosts are reachable, everything else
// (other sites, IP literals, loopback) goes to a dead proxy, and `kidsMap`
// re-points DNS names (YouTube Restricted Mode: restrict.youtube.com).
// Still a browser, with these limits (PLAN section 4).
struct WebAppDef {
    QString id;
    QString label;
    QString subtitle;
    QString url;
    QStringList hosts;
    QString icon = QStringLiteral("tv");
    QString tint;
    int order = 1000;
    QString browser = QStringLiteral("auto");
    bool drm = false;
    QString userAgent;
    QStringList kidsHosts;               // extra host patterns for kids profiles
    QList<QPair<QString, QString>> kidsMap;   // host -> host, kids profiles
    QString source;        // file it came from
    bool user = false;     // defined in the user's layer (removable)
    bool hidden = false;   // a built-in the user hid with a disabled stub
    bool shadowsLower = false;   // a user definition replacing a lower layer's id
};

// One Chromium-family browser MightyOS can drive. Flatpak first (PLAN D4),
// then a native install, only from fixed system paths (never PATH): the
// file and the directory it really lives in must belong to root and not be
// group/other writable. Snap installs are skipped (/usr/bin/snap, /snap/...,
// or a wrapper script mentioning /snap/, like Ubuntu's chromium-browser):
// their confinement refuses a --user-data-dir outside ~/snap.
struct BrowserDef {
    QString id;            // "chrome"
    QString label;         // "Google Chrome"
    QString flatpakId;     // "com.google.Chrome"
    QStringList paths;     // native: fixed absolute paths, preferred first
    bool widevine = false; // ships the Widevine CDM (Netflix, Disney+, ...)
};

// The browser a launch would use.
struct BrowserChoice {
    const BrowserDef *browser = nullptr;
    bool flatpak = false;
    QString program;       // absolute: flatpak or the native browser
    bool ok() const { return browser != nullptr; }
};

class WebAppRegistry
{
public:
    static constexpr int kVersion = 1;
    static constexpr qint64 kMaxFileBytes = 16 * 1024;
    static constexpr int kMaxFilesPerDir = 128;
    static constexpr int kMaxHosts = 16;
    static constexpr int kMaxUrlLength = 2048;
    static inline const QString kBuiltinDir = QStringLiteral(":/mightyos/webapps.d");
    static inline const QString kUserPrefix = QStringLiteral("user-");

    static QStringList defaultDirs();

    // Loads `dirs` (lowest priority first). The last dir is the user's
    // layer when `userDir` equals it (its definitions are user == true).
    void load(const QStringList &dirs, const QString &userDir = QString());

    static std::optional<WebAppDef> parse(const QByteArray &json, QString *error = nullptr,
                                          bool *disabled = nullptr);
    static bool isValidId(const QString &id);
    static bool isValidHostPattern(const QString &pattern);
    // `hosts` empty: any valid host. `error` gets a short user-facing reason.
    static bool isValidUrl(const QString &url, const QStringList &hosts, QString *error = nullptr);
    // The lower-case host of a valid URL, else "".
    static QString hostOf(const QString &url);
    static bool isValidUserAgent(const QString &ua);
    // A host name or strict IPv4 (no patterns).
    static bool isValidHost(const QString &host);

    // The JSON for a user-added tile (id "user-<slug>"), or empty with
    // `error` set. `taken` are ids that already exist.
    static QByteArray userDefinition(const QString &label, const QString &url, const QStringList &taken,
                                     QString *id, QString *error);
    static QByteArray disabledStub(const QString &id);

    const QList<WebAppDef> &webApps() const { return m_apps; }   // sorted (order, label), hidden included
    const WebAppDef *find(const QString &id) const;
    QStringList errors() const { return m_errors; }

private:
    QList<WebAppDef> m_apps;
    QHash<QString, int> m_index;
    QStringList m_errors;
};

namespace WebAppLaunch {

const QList<BrowserDef> &browsers();
const BrowserDef *browser(const QString &id);

// Resolution order: the definition's own preference first (if installed and
// suitable), then for DRM services chrome, edge, brave (Chromium has no
// Widevine), for everything else chrome, chromium, brave, edge.
BrowserChoice choose(const WebAppDef &app, const LaunchEnv &env);
QStringList order(const WebAppDef &app);

// Per-profile browser data (logins, cookies): never shared between
// profiles, so a kids profile doesn't inherit a parent's sessions.
//  - Flatpak: ~/.var/app/<browser app id>/data/mightyos/profiles/<profile>
//    (inside the browser's own sandbox-writable dir; MightyOS never reads
//    or trusts anything there, the browser creates it),
//  - native: $XDG_DATA_HOME/mightyos/profiles/<profile>/browsers/<browser>.
QString userDataDir(const BrowserChoice &choice, const QString &profileId, const LaunchEnv &env);

// A native browser at `path` (relative to env.systemRoot) that may be run:
// see BrowserDef. Returns the full path or "".
QString trustedNativeBrowser(const QString &path, const LaunchEnv &env);

// Sandbox narrowing for Flatpak browsers (after `flatpak run`): no devices
// but the GPU and input (gamepads: YouTube's TV UI uses the Gamepad API),
// no host, home, documents or downloads access.
QStringList flatpakSandboxArgs();

// Kids-profile network limits (see WebAppDef):
//   --proxy-server=127.0.0.1:9 (nothing listens on discard: refused),
//   --proxy-bypass-list=<-loopback>;<allowed hosts>,
//   --host-resolver-rules=MAP <from> <to>, ..., MAP * ~NOTFOUND, EXCLUDE <allowed>, ...
QStringList kidsArgs(const WebAppDef &app);

// argv only (PLAN section 4):
//   [flatpak run <sandbox args> <app id> | <browser>] --user-data-dir=<dir>
//   --kiosk --no-first-run --no-default-browser-check --password-store=basic
//   --hide-crash-restore-bubble --noerrdialogs --ozone-platform-hint=auto
//   [kids args] [--user-agent=<ua>] --app=<url>
// Every element is built from validated parts; the URL is re-validated.
// `install` names the browser to get when none is usable.
LaunchPlan plan(const WebAppDef &app, const QString &profileId, const LaunchEnv &env, bool restricted = false);

} // namespace WebAppLaunch
