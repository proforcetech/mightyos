#include "textsanitize.h"

#include <QList>

namespace TextSanitize {

namespace {

bool isDropped(char32_t c)
{
    if (c < 0x20 && c != U'\t' && c != U'\n' && c != U'\r')
        return true;                                  // C0 (tab/newlines become spaces)
    if (c == 0x7F || (c >= 0x80 && c <= 0x9F))
        return true;                                  // DEL, C1
    switch (c) {
    case 0x061C:                                      // Arabic letter mark
    case 0x200B: case 0x200C: case 0x200D:            // zero-width space / (non-)joiner
    case 0x200E: case 0x200F:                         // LRM, RLM
    case 0x2060: case 0x2061: case 0x2062: case 0x2063: case 0x2064:   // word joiner, invisible ops
    case 0xFEFF:                                      // BOM / ZWNBSP
    case 0x180E:                                      // Mongolian vowel separator
        return true;
    }
    if (c >= 0x202A && c <= 0x202E)                   // LRE RLE PDF LRO RLO
        return true;
    if (c >= 0x2066 && c <= 0x2069)                   // LRI RLI FSI PDI
        return true;
    if (c >= 0xFFF9 && c <= 0xFFFB)                   // interlinear annotations
        return true;
    return false;
}

bool isSpace(char32_t c)
{
    return c == U'\t' || c == U'\n' || c == U'\r' || c == 0x2028 || c == 0x2029
        || QChar::isSpace(c);
}

} // namespace

QString label(const QString &text, int maxChars)
{
    QString out;
    out.reserve(qMin<qsizetype>(text.size(), maxChars * 2));
    int count = 0;
    bool pendingSpace = false;
    for (const uint u : text.toUcs4()) {
        const char32_t c = u;
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) || isDropped(c))
            continue;
        if (isSpace(c)) {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (pendingSpace) {
            if (count + 1 >= maxChars)
                break;
            out.append(QLatin1Char(' '));
            ++count;
            pendingSpace = false;
        }
        if (count >= maxChars)
            break;
        out.append(QString::fromUcs4(&c, 1));
        ++count;
    }
    return out;
}

QString escapeForTerminal(const QString &text)
{
    QString out;
    for (const uint u : text.toUcs4()) {
        const char32_t c = u;
        if (c == U'\\' || c == U'"') {
            out += QLatin1Char('\\');
            out += QChar(c);
        } else if (c < 0x20 || c == 0x7F) {
            out += QStringLiteral("\\x%1").arg(uint(c), 2, 16, QLatin1Char('0'));
        } else if ((c >= 0x80 && c <= 0x9F) || isDropped(c) || c == 0x2028 || c == 0x2029) {
            out += QStringLiteral("\\u{%1}").arg(uint(c), 4, 16, QLatin1Char('0'));
        } else {
            out += QString::fromUcs4(&c, 1);
        }
    }
    return out;
}

} // namespace TextSanitize
