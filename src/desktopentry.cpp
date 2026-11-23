#include "desktopentry.h"

#include <QRegularExpression>
#include <QStringDecoder>

namespace DesktopEntry {

namespace {

const QString kGroup = QStringLiteral("Desktop Entry");

bool isReservedUnquoted(QChar c)
{
    // Spec, "The Exec key": characters that require quoting. ' ' is the
    // separator and handled by the caller; '"' starts a quoted part.
    switch (c.unicode()) {
    case '\t': case '\n': case '\'': case '\\': case '>': case '<': case '~':
    case '|': case '&': case ';': case '$': case '*': case '?': case '#':
    case '(': case ')': case '`':
        return true;
    }
    return false;
}

QString stripLocale(const QString &locale)
{
    // "de_DE.UTF-8@euro" -> "de_DE@euro" (the encoding part is ignored).
    QString result = locale;
    const qsizetype dot = result.indexOf(QLatin1Char('.'));
    if (dot >= 0) {
        const qsizetype at = result.indexOf(QLatin1Char('@'), dot);
        result = result.left(dot) + (at >= 0 ? result.mid(at) : QString());
    }
    return result;
}

} // namespace

QString unescapeString(const QString &raw)
{
    QString out;
    out.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const QChar c = raw.at(i);
        if (c != QLatin1Char('\\') || i + 1 >= raw.size()) {
            out.append(c);
            continue;
        }
        const QChar n = raw.at(i + 1);
        switch (n.unicode()) {
        case 's':  out.append(QLatin1Char(' '));  ++i; break;
        case 'n':  out.append(QLatin1Char('\n')); ++i; break;
        case 't':  out.append(QLatin1Char('\t')); ++i; break;
        case 'r':  out.append(QLatin1Char('\r')); ++i; break;
        case '\\': out.append(QLatin1Char('\\')); ++i; break;
        default:   out.append(c); break;          // keep for the next level
        }
    }
    return out;
}

QString Entry::string(const QString &key) const
{
    return unescapeString(values.value(key));
}

QString Entry::localeString(const QString &key, const QString &locale) const
{
    // lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang, plain.
    const QString l = stripLocale(locale);
    const qsizetype at = l.indexOf(QLatin1Char('@'));
    const QString modifier = at >= 0 ? l.mid(at + 1) : QString();
    const QString base = at >= 0 ? l.left(at) : l;
    const qsizetype us = base.indexOf(QLatin1Char('_'));
    const QString lang = us >= 0 ? base.left(us) : base;
    const QString country = us >= 0 ? base.mid(us + 1) : QString();

    QStringList candidates;
    if (!country.isEmpty() && !modifier.isEmpty())
        candidates << lang + QLatin1Char('_') + country + QLatin1Char('@') + modifier;
    if (!country.isEmpty())
        candidates << lang + QLatin1Char('_') + country;
    if (!modifier.isEmpty())
        candidates << lang + QLatin1Char('@') + modifier;
    if (!lang.isEmpty() && lang != QLatin1String("C"))
        candidates << lang;
    for (const QString &candidate : std::as_const(candidates)) {
        const QString localized = key + QLatin1Char('[') + candidate + QLatin1Char(']');
        if (values.contains(localized))
            return string(localized);
    }
    return string(key);
}

bool Entry::boolean(const QString &key) const
{
    return values.value(key).trimmed() == QLatin1String("true");
}

QStringList Entry::list(const QString &key) const
{
    const QString raw = values.value(key);
    QStringList out;
    QString current;
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const QChar c = raw.at(i);
        if (c == QLatin1Char('\\') && i + 1 < raw.size() && raw.at(i + 1) == QLatin1Char(';')) {
            current.append(QLatin1Char(';'));
            ++i;
        } else if (c == QLatin1Char(';')) {
            out.append(unescapeString(current));
            current.clear();
        } else {
            current.append(c);
        }
    }
    out.append(unescapeString(current));
    out.removeAll(QString());
    return out;
}

std::optional<Entry> parse(const QByteArray &data, QString *error)
{
    auto fail = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return std::nullopt;
    };
    if (data.size() > kMaxFileBytes)
        return fail("file too large");

    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(data);
    if (decoder.hasError())
        return fail("not valid UTF-8");

    static const QRegularExpression keyRe(QStringLiteral("^[A-Za-z0-9-]+(\\[[A-Za-z0-9_.@-]+\\])?$"));

    Entry entry;
    bool inGroup = false;
    bool seenGroup = false;
    int lineCount = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        if (++lineCount > kMaxLines)
            return fail("too many lines");
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;
        if (trimmed.startsWith(QLatin1Char('['))) {
            if (!trimmed.endsWith(QLatin1Char(']')))
                return fail("malformed group header");
            const QString group = trimmed.mid(1, trimmed.size() - 2);
            if (!seenGroup && group != kGroup)
                return fail("first group is not [Desktop Entry]");
            if (group == kGroup && seenGroup)
                return fail("duplicate [Desktop Entry] group");
            inGroup = group == kGroup;
            seenGroup = true;
            continue;
        }
        if (!seenGroup)
            return fail("key outside a group");
        if (!inGroup)
            continue;
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            return fail("malformed line");
        const QString key = line.left(eq).trimmed();
        if (!keyRe.match(key).hasMatch())
            return fail("malformed key");
        // Spec: spaces before and after '=' are ignored.
        QString value = line.mid(eq + 1);
        while (value.startsWith(QLatin1Char(' ')) || value.startsWith(QLatin1Char('\t')))
            value.remove(0, 1);
        if (!entry.values.contains(key))   // first occurrence wins
            entry.values.insert(key, value);
    }
    if (!seenGroup)
        return fail("no [Desktop Entry] group");
    return entry;
}

ExecResult parseExec(const QString &raw, const ExecContext &context)
{
    ExecResult result;
    auto fail = [&result](const char *why) {
        result.ok = false;
        result.argv.clear();
        result.error = QString::fromLatin1(why);
        return result;
    };

    if (raw.size() > kMaxExecLength)
        return fail("Exec too long");

    // Level 1: general string escapes (\\ -> \, \s -> space, ...).
    const QString s = unescapeString(raw);

    struct Arg {
        QString text;
        bool quoted = false;      // had a quoted part ("" is a real argument)
        bool fieldCode = false;   // contained a field code
        QStringList replace;      // %i: replaces the whole argument
        bool replaced = false;
    };
    QList<Arg> args;
    Arg current;
    bool inArg = false;
    bool inQuote = false;
    int fileCodes = 0;

    auto endArg = [&] {
        if (inArg)
            args.append(current);
        current = Arg();
        inArg = false;
    };
    auto standalone = [&](qsizetype nextPos) {
        // The field code must be a whole, unquoted argument.
        return !inQuote && current.text.isEmpty() && !current.quoted
               && (nextPos >= s.size() || s.at(nextPos) == QLatin1Char(' '));
    };

    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);

        // Control characters (incl. NUL, tab, newline from \t \n) are
        // never valid, quoted or not.
        if (c.category() == QChar::Other_Control)
            return fail("control character");

        if (c == QLatin1Char('%')) {
            if (i + 1 >= s.size())
                return fail("dangling %");
            const QChar code = s.at(++i);
            // Spec: field codes must not be used inside a quoted argument
            // (a literal "%%" is not a field code and stays allowed).
            if (inQuote && code != QLatin1Char('%'))
                return fail("field code inside quotes");
            inArg = true;
            switch (code.unicode()) {
            case '%':
                current.text.append(QLatin1Char('%'));
                break;
            case 'f': case 'u':
                // No files/URLs are ever passed: expands to nothing.
                ++fileCodes;
                current.fieldCode = true;
                break;
            case 'F': case 'U':
                if (!standalone(i + 1))
                    return fail("%F/%U must be an argument on its own");
                ++fileCodes;
                current.fieldCode = true;
                break;
            case 'i':
                if (!standalone(i + 1))
                    return fail("%i must be an argument on its own");
                current.fieldCode = true;
                current.replaced = true;
                if (!context.icon.isEmpty())
                    current.replace = { QStringLiteral("--icon"), context.icon };
                break;
            case 'c':
                current.text.append(context.name);
                current.fieldCode = true;
                break;
            case 'k':
                current.text.append(context.desktopFilePath);
                current.fieldCode = true;
                break;
            case 'd': case 'D': case 'n': case 'N': case 'v': case 'm':
                current.fieldCode = true;   // deprecated: removed
                break;
            default:
                return fail("unknown field code");
            }
            if (fileCodes > 1)
                return fail("more than one file field code");
            continue;
        }

        if (inQuote) {
            if (c == QLatin1Char('"')) {
                inQuote = false;
            } else if (c == QLatin1Char('\\')) {
                if (i + 1 >= s.size())
                    return fail("dangling backslash in quotes");
                const QChar n = s.at(++i);
                if (n != QLatin1Char('"') && n != QLatin1Char('`') && n != QLatin1Char('$')
                    && n != QLatin1Char('\\'))
                    return fail("invalid escape in quotes");
                current.text.append(n);
            } else if (c == QLatin1Char('`') || c == QLatin1Char('$')) {
                return fail("unescaped ` or $ in quotes (needs a shell)");
            } else {
                current.text.append(c);
            }
            continue;
        }

        if (c == QLatin1Char(' ')) {
            endArg();
        } else if (c == QLatin1Char('"')) {
            inArg = true;
            inQuote = true;
            current.quoted = true;
        } else if (isReservedUnquoted(c)) {
            return fail("unquoted reserved character (needs a shell)");
        } else {
            inArg = true;
            current.text.append(c);
        }
    }
    if (inQuote)
        return fail("unterminated quote");
    endArg();

    QStringList argv;
    for (const Arg &arg : std::as_const(args)) {
        if (arg.replaced) {
            argv.append(arg.replace);
            continue;
        }
        // An argument that consisted only of field codes expanding to
        // nothing is dropped instead of becoming an empty argument.
        if (arg.text.isEmpty() && arg.fieldCode)
            continue;
        argv.append(arg.text);
    }

    if (argv.isEmpty() || argv.first().isEmpty())
        return fail("no program");
    if (argv.size() > kMaxArgs)
        return fail("too many arguments");
    const QString &program = argv.first();
    if (program.contains(QLatin1Char('/')) && !program.startsWith(QLatin1Char('/')))
        return fail("relative program path");
    if (program.contains(QLatin1Char('=')))
        return fail("environment assignment as program (needs a shell)");

    result.ok = true;
    result.argv = argv;
    return result;
}

} // namespace DesktopEntry
