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
// C++ owners fill it with setItems(), insertItem()/insertItems(),
// removeAt()/removeItems() and moveItem(). QML can fill static rows with
// setRows()/appendRow() (the category bar, settings sections, tests).
//
// setItems() is diff-based (PLAN M4, from the M2 review; 20k-ROM
// libraries): rows whose keys disappeared are removed and new keys are
// inserted in contiguous runs, changed rows get dataChanged(). Views keep
// their delegates and focus. It falls back to a reset (itemsAboutToReset /
// itemsReset) only when the surviving rows changed order, either list has
// duplicate keys, or the edit has more than kMaxDiffRuns contiguous runs
// (each run is one insert/remove notification; scattered edits are cheaper
// as one reset). During a diff the key hash is rebuilt once, at the end.
//
// The key hash and the sort keys are maintained incrementally: inserting
// or removing rows only re-numbers the rows after them and only computes
// sortKey() for the new rows.
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

    static constexpr int kMaxDiffRuns = 24;

    explicit TileListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_items.size()); }
    const QList<TileItem> &items() const { return m_items; }
    const TileItem *itemAt(int row) const;

    void setItems(const QList<TileItem> &items);
    void insertItem(int row, const TileItem &item);
    // One beginInsertRows() for the whole batch.
    void insertItems(int row, const QList<TileItem> &items);
    bool removeAt(int row);
    // Removes `count` rows starting at `row` (one beginRemoveRows()).
    bool removeItems(int row, int count);
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
    // Letter group of a row: the upper-case first character of its sort
    // key if that is a letter, "#" for digits and everything else; "" out
    // of range. The A-Z indicator shows it.
    Q_INVOKABLE QString letterAt(int row) const;
    // LT/RT letter stepping. delta > 0: first row of the next letter group;
    // delta < 0: first row of the current group, or of the previous group
    // if `row` already is that first row. -1 if there is none (an end).
    Q_INVOKABLE int letterStep(int row, int delta) const;

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

    // Statistics for tests: how many full resets setItems() did.
    int resetCount() const { return m_resets; }

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
    void reindexFrom(int row);
    void insertRun(int row, const QList<TileItem> &items, bool reindex);   // no countChanged()
    void resetTo(const QList<TileItem> &items);
    static QString sortKeyOf(const TileItem &item);
    static QString groupOf(const QString &sortKey);

    QList<TileItem> m_items;
    QHash<QString, int> m_index;
    QStringList m_sortKeys;   // sortKey() of each row (sortTitle, else title)
    bool m_duplicates = false;   // some key occurs twice: hash rebuilt in full
    int m_resets = 0;
};
