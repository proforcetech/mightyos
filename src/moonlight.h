#pragma once

#include "emulatorlaunch.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Moonlight game streaming (PLAN D10, M5): paired hosts and their apps,
// read from Moonlight-Qt's own settings file, and `moonlight stream`.
//
// Source: Moonlight.conf (QSettings INI) of the Flatpak
//   ~/.var/app/com.moonlight_stream.Moonlight/config/Moonlight Game Streaming Project/Moonlight.conf
// or of a native install
//   $XDG_CONFIG_HOME/Moonlight Game Streaming Project/Moonlight.conf.
// The Flatpak's copy lives in its sandbox-writable dir, so the file is
// untrusted: read-only, size-capped (kMaxConfigBytes, readRegularFile:
// no symlinks, regular files only), parsed by our own minimal INI reader
// (only the [hosts] section), and every value validated:
//   hosts\N\uuid       host id used in item keys and argv (Moonlight's CLI
//                      matches a host by name, UUID or address),
//   hosts\N\hostname   display name (sanitised),
//   hosts\N\apps\M\name / hidden   the app list Moonlight cached the last
//                      time it talked to the host.
// MightyOS never runs `moonlight list` (a network round trip of up to 30 s
// per host): the cached list is what Moonlight's own UI shows too. Opening
// Moonlight once refreshes it.
//
// Item keys: moonlight:<host> (a host) and moonlight:<host>/<app> with the
// app name percent-encoded (keys have no whitespace). <host> is the UUID,
// or for hosts without one a DNS name / IPv4 address.
struct MoonlightApp {
    QString name;
    bool hidden = false;
};

struct MoonlightHost {
    QString id;      // uuid (or host name), see isValidHostId
    QString name;    // display
    QList<MoonlightApp> apps;
    bool flatpak = false;   // from the Flatpak's config (launch with the Flatpak)
};

namespace Moonlight {

static inline const QString kFlatpakId = QStringLiteral("com.moonlight_stream.Moonlight");
constexpr qint64 kMaxConfigBytes = 1024 * 1024;
constexpr int kMaxHosts = 32;
constexpr int kMaxAppsPerHost = 256;
constexpr int kMaxAppName = 64;

// A UUID (8-4-4-4-12 hex), a DNS name or an IPv4 address; never starts
// with '-'.
bool isValidHostId(const QString &id);
// 1..kMaxAppName chars, sanitised text (no control/bidi chars), no leading
// '-' or whitespace.
bool isValidAppName(const QString &name);

// Hosts from a Moonlight.conf. Invalid hosts / apps are skipped.
QList<MoonlightHost> parseConfig(const QByteArray &ini);

// One QSettings INI value: "quoted" parts, escapes, @-prefix. False for
// string lists (an unquoted ',') and variants (@ByteArray(...), ...).
bool parseIniString(const QByteArray &raw, QString *out);

QString hostKey(const QString &hostId);
QString appKey(const QString &hostId, const QString &appName);
// The key with its host part in lower case (Moonlight matches hosts
// case-insensitively; allow-lists and favorites compare exactly); other
// keys unchanged.
QString canonicalKey(const QString &key);
// Splits a moonlight: key; `app` is empty for a host key. False if invalid.
bool parseKey(const QString &key, QString *hostId, QString *app);

// The config files to read, Flatpak first, with whether each is the Flatpak's.
struct ConfigFile {
    QString path;
    bool flatpak = false;
};
QList<ConfigFile> configFiles(const QString &home, const QString &configHome);
// Reads and merges them (a host in both: the Flatpak's wins); worker-thread
// safe. Only files whose Moonlight is installed (`env`) are read.
QList<MoonlightHost> readHosts(const QList<ConfigFile> &files, const LaunchEnv &env);

bool installed(const LaunchEnv &env, bool *flatpak = nullptr);

// [flatpak run com.moonlight_stream.Moonlight | moonlight] stream -- <host> <app>
// ("--": nothing after it is an option for Moonlight's QCommandLineParser).
LaunchPlan planStream(const QString &hostId, const QString &app, bool preferFlatpak, const LaunchEnv &env);

} // namespace Moonlight
