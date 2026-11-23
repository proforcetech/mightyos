#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// One tile. Every row/grid in the UI shows a list of these.
struct TileItem {
    QString key;        // stable id: "rom:snes:<hash>", "desktop:vim", "snes", ...
    QString title;
    QString subtitle;
    QString icon;       // built-in vector icon name (Icons.js), the fallback
    QString artwork;    // image URL (image://art/...) or empty
    QString kind;       // "game" | "app" | "system" | "category" | "profile" | ...
    QString tint;       // colour band, "#rrggbb"; empty = theme accent
    QString sortTitle;  // for A-Z jumps; empty = title
    QString badge;      // small corner label ("Kids"); empty = none
    bool available = true;

    bool operator==(const TileItem &) const = default;
};

// The model contract every TileRow / TileGrid consumes (PLAN M2, from the
// M1 review). QML never iterates a model in JavaScript: lookups by key are
// backed by a hash, by letter by one linear pass in C++.
//
// Roles (stable names): key, title, subtitle, icon, artwork, kind,
// available, plus tint, sortTitle and badge.
//
// C++ owners fill it with setItems() (a reset, or only dataChanged() when
// the keys and their order are unchanged), insertItem(), removeAt() and
// moveItem(). QML can fill static rows with setRows()/appendRow() (the
// category bar, settings sections, tests).
class TileListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        TitleRole,
        SubtitleRole,
        IconRole,
        ArtworkRole,
        KindRole,
        AvailableRole,
        TintRole,
        SortTitleRole,
        BadgeRole,
    };
    Q_ENUM(Role)

    explicit TileListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_items.size()); }
    const QList<TileItem> &items() const { return m_items; }
    const TileItem *itemAt(int row) const;

    void setItems(const QList<TileItem> &items);
    void insertItem(int row, const TileItem &item);
    bool removeAt(int row);
    bool moveItem(int from, int to);
    bool updateItem(int row, const TileItem &item);

    // -1 if absent. With duplicate keys, the first row wins.
    Q_INVOKABLE int indexOfKey(const QString &key) const;
    Q_INVOKABLE QString keyAt(int row) const;
    // { key, title, subtitle, icon, artwork, kind, available, tint,
    //   sortTitle, badge }, or an empty map when out of range.
    Q_INVOKABLE QVariantMap get(int row) const;
    // First row whose sort key starts with `letter` (folded the same way)
    // or a later one; -1 if none. Assumes rows sorted by sortKey().
    Q_INVOKABLE int firstIndexForLetter(const QString &letter) const;

    // The one ordering used for sorting (Library, AppScanner) and letter
    // jumps: Unicode-decomposed, accents and leading punctuation/symbols
    // removed, case-folded. "[Beta] X" -> "beta] x", "_Alpha" -> "alpha",
    // "Échec" -> "echec". Falls back to the folded title if nothing is left.
    static QString sortKey(const QString &title);
    static bool sortLess(const QString &a, const QString &b);

    // QML-side population. Unknown map keys are ignored.
    Q_INVOKABLE void setRows(const QVariantList &rows);
    Q_INVOKABLE void appendRow(const QVariantMap &row);
    Q_INVOKABLE void clear();

    static TileItem fromVariant(const QVariantMap &row);
    static QVariantMap toVariant(const TileItem &item);

signals:
    void countChanged();
    // Around a reset (setItems() with different keys). Views use them to
    // keep the focus on the same key: QML can't reliably connect to
    // QAbstractItemModel's private reset signals.
    void itemsAboutToReset();
    void itemsReset();
    // After moveItem(): views keep the focus on the moved tile (ListView
    // doesn't move its currentIndex with a moved row).
    void itemMoved(int from, int to);

private:
    void rebuildIndex();

    QList<TileItem> m_items;
    QHash<QString, int> m_index;
    QStringList m_sortKeys;   // sortKey() of each row (sortTitle, else title)
};
