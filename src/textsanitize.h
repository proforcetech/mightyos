#pragma once

#include <QString>

// Cleaning of untrusted display strings (device names from USB/Bluetooth
// descriptors, CEC adapter names), PLAN section 4. Shown with
// Text.PlainText anyway; this removes what plain text still can't make
// harmless.
namespace TextSanitize {

// Drops C0/C1 controls, DEL, bidi embeddings/overrides/isolates and marks,
// zero-width characters and the BOM; line/paragraph separators and tabs
// become spaces; whitespace is collapsed and trimmed; at most `maxChars`
// code points (never splits a surrogate pair). Empty input -> "".
QString label(const QString &text, int maxChars = 64);

// For terminal output (mightyos-inputtest): every non-printable or
// format character becomes \xNN or \u{NNNN}; backslash and quote escaped.
QString escapeForTerminal(const QString &text);

} // namespace TextSanitize
