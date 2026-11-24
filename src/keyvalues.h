#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringView>
#include <optional>

// Valve KeyValues text format (libraryfolders.vdf, appmanifest_*.acf,
// registry.vdf). Steam writes these files, but anything under ~/.steam can
// be edited by any program running as the user, so they are untrusted
// input (PLAN section 4): the parser is iterative (no recursion), and the
// input size, nesting depth, node count and token length are capped.
//
// Grammar (what Steam writes, plus the common variants):
//   file   := pair*
//   pair   := key ( value | '{' pair* '}' ) [ conditional ]
//   key, value := "quoted string" | bare-token
//   conditional := [$WIN32] style token, skipped
// Quoted strings understand \\ \" \n \t (other backslashes stay as is);
// "//" starts a comment up to the end of the line; a UTF-8 BOM is skipped.
// #include / #base lines parse as ordinary pairs and are never followed.
// Duplicate keys are kept in order; lookups return the first match and
// compare keys case-insensitively (as Steam does).
namespace KeyValues {

struct Limits {
    qsizetype maxBytes = 4 * 1024 * 1024;
    int maxDepth = 32;
    int maxNodes = 200000;
    int maxTokenLength = 64 * 1024;
};

struct Node {
    QString key;
    QString value;         // leaves only
    QList<Node> children;  // blocks only
    bool isBlock = false;

    // First child named `key` (case-insensitive), or nullptr.
    const Node *child(QStringView key) const;
    // Follows a path of block names; nullptr if any step is missing.
    const Node *path(std::initializer_list<QStringView> keys) const;
    // Value of the first leaf child named `key`, or `fallback`.
    QString valueOf(QStringView key, const QString &fallback = QString()) const;
};

// The root is a block with an empty key whose children are the top-level
// pairs. nullopt (and `*error`, if given) on malformed input or when a
// limit is hit.
std::optional<Node> parse(const QByteArray &data, const Limits &limits = Limits(), QString *error = nullptr);

} // namespace KeyValues
