#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <optional>

// Desktop Entry Specification 1.5 parsing, for untrusted input.
//
// Only the [Desktop Entry] group is read (actions and other groups are
// ignored). Files are capped in size and line count; anything that isn't
// valid UTF-8 is rejected.
//
// parseExec() turns an Exec= value into an argument vector *without a
// shell*. It implements the spec's two levels of unescaping (string
// escapes first, then the quoting rules), drops or expands field codes,
// and rejects every command that would need a shell to mean what it says
// (unquoted reserved characters such as | > $ ; ` ~ * etc.) or that is
// malformed (unterminated quotes, invalid escapes, unknown field codes,
// more than one file field code).
namespace DesktopEntry {

constexpr qint64 kMaxFileBytes = 64 * 1024;
constexpr int kMaxLines = 2000;
constexpr int kMaxExecLength = 4096;
constexpr int kMaxArgs = 256;

struct Entry {
    QHash<QString, QString> values;   // raw (still escaped) values; keys incl. "[locale]"

    bool has(const QString &key) const { return values.contains(key); }
    // String value with the general escapes (\s \n \t \r \\) applied.
    QString string(const QString &key) const;
    // Localised string: key[lang_COUNTRY@MODIFIER] fallbacks per the spec,
    // then the plain key. `locale` like "de_DE" or "de_DE.UTF-8@euro".
    QString localeString(const QString &key, const QString &locale) const;
    // "true" / "false" (anything else: false).
    bool boolean(const QString &key) const;
    // ';'-separated list with "\;" escapes; empty items dropped.
    QStringList list(const QString &key) const;
};

// nullopt (with `error`) if the data is not a usable desktop file.
std::optional<Entry> parse(const QByteArray &data, QString *error = nullptr);

// General string escapes: \s \n \t \r \\. Unknown escapes are kept as-is
// (they matter to the Exec quoting level).
QString unescapeString(const QString &raw);

struct ExecContext {
    QString name;             // %c: translated Name
    QString icon;             // %i: Icon key (expanded to --icon <icon>)
    QString desktopFilePath;  // %k
};

struct ExecResult {
    bool ok = false;
    QStringList argv;
    QString error;            // when !ok; never contains the command itself
};

// `raw` is the Exec value exactly as it appears in the file.
ExecResult parseExec(const QString &raw, const ExecContext &context);

} // namespace DesktopEntry
