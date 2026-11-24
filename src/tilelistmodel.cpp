#include "tilelistmodel.h"

#include <QSet>

TileListModel::TileListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int TileListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

const TileItem *TileListModel::itemAt(int row) const
{
    return (row >= 0 && row < count()) ? &m_items.at(row) : nullptr;
}

QVariant TileListModel::data(const QModelIndex &index, int role) const
{
    const TileItem *item = index.isValid() ? itemAt(index.row()) : nullptr;
    if (!item)
        return {};
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:     return item->title;
    case KeyRole:       return item->key;
    case SubtitleRole:  return item->subtitle;
    case IconRole:      return item->icon;
    case ArtworkRole:   return item->artwork;
    case KindRole:      return item->kind;
    case AvailableRole: return item->available;
    case TintRole:      return item->tint;
    case SortTitleRole: return item->sortTitle.isEmpty() ? item->title : item->sortTitle;
    case BadgeRole:     return item->badge;
    }
    return {};
}

QHash<int, QByteArray> TileListModel::roleNames() const
{
    static const QHash<int, QByteArray> names {
        { KeyRole, "key" },
        { TitleRole, "title" },
        { SubtitleRole, "subtitle" },
        { IconRole, "icon" },
        { ArtworkRole, "artwork" },
        { KindRole, "kind" },
        { AvailableRole, "available" },
        { TintRole, "tint" },
        { SortTitleRole, "sortTitle" },
        { BadgeRole, "badge" },
    };
    return names;
}

QString TileListModel::sortKeyOf(const TileItem &item)
{
    return sortKey(item.sortTitle.isEmpty() ? item.title : item.sortTitle);
}

void TileListModel::rebuildIndex()
{
    m_index.clear();
    m_index.reserve(m_items.size());
    m_sortKeys.clear();
    m_sortKeys.reserve(m_items.size());
    for (const TileItem &item : std::as_const(m_items))
        m_sortKeys.append(sortKeyOf(item));
    reindexFrom(0);
}

// Re-numbers the hash for rows >= `row` (their position changed). With
// duplicate keys the "first row wins" rule needs the whole list, so the
// hash is rebuilt in full then (rare: favorites and libraries are unique).
void TileListModel::reindexFrom(int row)
{
    if (m_duplicates || row <= 0) {
        m_index.clear();
        m_index.reserve(m_items.size());
        m_duplicates = false;
        for (int i = 0; i < count(); ++i) {
            const auto inserted = m_index.tryEmplace(m_items.at(i).key, i);
            if (!inserted.inserted)
                m_duplicates = true;
        }
        return;
    }
    for (int i = row; i < count(); ++i) {
        const QString &key = m_items.at(i).key;
        const auto found = m_index.constFind(key);
        if (found != m_index.cend() && found.value() < row) {
            m_duplicates = true;   // an earlier row has this key: keep it
            continue;
        }
        m_index.insert(key, i);
    }
}

void TileListModel::resetTo(const QList<TileItem> &items)
{
    const int oldCount = count();
    emit itemsAboutToReset();
    beginResetModel();
    m_items = items;
    m_duplicates = false;
    rebuildIndex();
    endResetModel();
    ++m_resets;
    if (oldCount != count())
        emit countChanged();
    emit itemsReset();
}

void TileListModel::setItems(const QList<TileItem> &items)
{
    bool sameKeys = items.size() == m_items.size();
    for (qsizetype i = 0; sameKeys && i < items.size(); ++i)
        sameKeys = items.at(i).key == m_items.at(i).key;

    if (sameKeys) {
        // Same rows in the same order: update in place, so views keep their
        // delegates and focus (e.g. a re-scan that only changed a subtitle).
        for (int i = 0; i < count(); ++i) {
            if (m_items.at(i) == items.at(i))
                continue;
            const bool resort = m_items.at(i).title != items.at(i).title
                                || m_items.at(i).sortTitle != items.at(i).sortTitle;
            m_items[i] = items.at(i);
            if (resort)
                m_sortKeys[i] = sortKeyOf(m_items.at(i));
            const QModelIndex idx = index(i);
            emit dataChanged(idx, idx);
        }
        return;
    }

    // Diff: only for unique keys on both sides.
    QHash<QString, int> wanted;
    wanted.reserve(items.size());
    bool unique = !m_duplicates;
    for (int i = 0; unique && i < items.size(); ++i)
        unique = wanted.tryEmplace(items.at(i).key, i).inserted;
    if (!unique || m_items.isEmpty() || items.isEmpty()) {
        resetTo(items);
        return;
    }

    // Survivors (old rows that stay) must keep their relative order, and
    // the edit must be a few contiguous runs: every run is one
    // begin/endInsertRows (or Remove) that views process separately, so a
    // scattered edit (thousands of runs) is cheaper as one reset.
    QSet<QString> oldKeys;
    oldKeys.reserve(m_items.size());
    int lastPosition = -1;
    int removeRuns = 0;
    bool inRemoved = false;
    for (const TileItem &item : std::as_const(m_items)) {
        oldKeys.insert(item.key);
        const auto found = wanted.constFind(item.key);
        if (found == wanted.cend()) {
            removeRuns += inRemoved ? 0 : 1;
            inRemoved = true;
            continue;
        }
        inRemoved = false;
        if (found.value() < lastPosition) {
            resetTo(items);
            return;
        }
        lastPosition = found.value();
    }
    int insertRuns = 0;
    bool inInserted = false;
    int survivors = 0;
    for (const TileItem &item : items) {
        const bool isNew = !oldKeys.contains(item.key);
        survivors += isNew ? 0 : 1;
        insertRuns += (isNew && !inInserted) ? 1 : 0;
        inInserted = isNew;
    }
    if (survivors == 0 || removeRuns + insertRuns > kMaxDiffRuns) {
        resetTo(items);
        return;
    }

    const int oldCount = count();
    int firstTouched = count();
    // 1. Remove runs of vanished keys, from the end so row numbers hold.
    //    The key hash is rebuilt once, at the end.
    for (int i = count() - 1; i >= 0;) {
        if (wanted.contains(m_items.at(i).key)) {
            --i;
            continue;
        }
        int first = i;
        while (first > 0 && !wanted.contains(m_items.at(first - 1).key))
            --first;
        beginRemoveRows(QModelIndex(), first, i);
        m_items.remove(first, i - first + 1);
        m_sortKeys.remove(first, i - first + 1);
        endRemoveRows();
        firstTouched = qMin(firstTouched, first);
        i = first - 1;
    }
    // 2. Insert runs of new keys at their final positions (ascending, so
    //    every row before `i` already matches `items`).
    for (int i = 0; i < items.size();) {
        if (i < count() && m_items.at(i).key == items.at(i).key) {
            if (!(m_items.at(i) == items.at(i))) {
                const bool resort = m_items.at(i).title != items.at(i).title
                                    || m_items.at(i).sortTitle != items.at(i).sortTitle;
                m_items[i] = items.at(i);
                if (resort)
                    m_sortKeys[i] = sortKeyOf(m_items.at(i));
                const QModelIndex idx = index(i);
                emit dataChanged(idx, idx);
            }
            ++i;
            continue;
        }
        // items[i] is new (every survivor before it already matched); the
        // run continues while the keys are new too.
        int last = i;
        while (last + 1 < items.size() && !oldKeys.contains(items.at(last + 1).key))
            ++last;
        insertRun(i, items.mid(i, last - i + 1), false);
        firstTouched = qMin(firstTouched, i);
        i = last + 1;
    }
    Q_ASSERT(m_items == items);
    if (firstTouched < count() || oldCount != count()) {
        // Drop hash entries of removed keys, then renumber from the first
        // changed row (one pass).
        for (const QString &key : std::as_const(oldKeys)) {
            if (!wanted.contains(key))
                m_index.remove(key);
        }
        reindexFrom(firstTouched);
    }
    if (oldCount != count())
        emit countChanged();
}

void TileListModel::insertItem(int row, const TileItem &item)
{
    insertItems(row, { item });
}

void TileListModel::insertItems(int row, const QList<TileItem> &items)
{
    if (items.isEmpty())
        return;
    insertRun(qBound(0, row, count()), items, true);
    emit countChanged();
}

void TileListModel::insertRun(int row, const QList<TileItem> &items, bool reindex)
{
    const int last = row + static_cast<int>(items.size()) - 1;
    beginInsertRows(QModelIndex(), row, last);
    m_items.insert(row, items.size(), TileItem());
    QStringList keys;
    keys.reserve(items.size());
    for (qsizetype i = 0; i < items.size(); ++i) {
        m_items[row + i] = items.at(i);
        keys.append(sortKeyOf(items.at(i)));
    }
    m_sortKeys.insert(row, keys.size(), QString());
    for (qsizetype i = 0; i < keys.size(); ++i)
        m_sortKeys[row + i] = keys.at(i);
    if (reindex)
        reindexFrom(row);
    endInsertRows();
}

bool TileListModel::removeAt(int row)
{
    return removeItems(row, 1);
}

bool TileListModel::removeItems(int row, int n)
{
    if (row < 0 || n <= 0 || row + n > count())
        return false;
    beginRemoveRows(QModelIndex(), row, row + n - 1);
    for (int i = row; i < row + n; ++i) {
        const auto found = m_index.constFind(m_items.at(i).key);
        if (found != m_index.cend() && found.value() == i)
            m_index.remove(m_items.at(i).key);
    }
    m_items.remove(row, n);
    m_sortKeys.remove(row, n);
    reindexFrom(m_duplicates ? 0 : row);
    endRemoveRows();
    emit countChanged();
    return true;
}

bool TileListModel::moveItem(int from, int to)
{
    if (from < 0 || from >= count() || to < 0 || to >= count() || from == to)
        return false;
    // Qt's destination is the row *before which* the item lands, in the
    // pre-move numbering.
    const int destination = to > from ? to + 1 : to;
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), destination))
        return false;
    m_items.move(from, to);
    m_sortKeys.move(from, to);
    reindexFrom(m_duplicates ? 0 : qMin(from, to));
    endMoveRows();
    emit itemMoved(from, to);
    return true;
}

bool TileListModel::updateItem(int row, const TileItem &item)
{
    if (row < 0 || row >= count())
        return false;
    const bool keyChanged = m_items.at(row).key != item.key;
    m_items[row] = item;
    m_sortKeys[row] = sortKeyOf(item);
    if (keyChanged) {
        m_duplicates = true;   // force a full, correct rebuild
        reindexFrom(0);
    }
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx);
    return true;
}

int TileListModel::indexOfKey(const QString &key) const
{
    if (key.isEmpty())
        return -1;
    return m_index.value(key, -1);
}

QString TileListModel::keyAt(int row) const
{
    const TileItem *item = itemAt(row);
    return item ? item->key : QString();
}

QVariantMap TileListModel::get(int row) const
{
    const TileItem *item = itemAt(row);
    return item ? toVariant(*item) : QVariantMap();
}

int TileListModel::firstIndexForLetter(const QString &letter) const
{
    if (letter.isEmpty())
        return -1;
    const QString wanted = sortKey(letter).left(1);
    if (wanted.isEmpty())
        return -1;
    for (int i = 0; i < m_sortKeys.size(); ++i) {
        const QString &key = m_sortKeys.at(i);
        if (!key.isEmpty() && QStringView(key).left(1).compare(wanted) >= 0)
            return i;
    }
    return -1;
}

QString TileListModel::groupOf(const QString &key)
{
    if (key.isEmpty())
        return QStringLiteral("#");
    const QChar c = key.at(0);
    return c.isLetter() ? QString(c).toUpper() : QStringLiteral("#");
}

QString TileListModel::letterAt(int row) const
{
    return (row >= 0 && row < m_sortKeys.size()) ? groupOf(m_sortKeys.at(row)) : QString();
}

int TileListModel::letterStep(int row, int delta) const
{
    if (row < 0 || row >= count() || delta == 0)
        return -1;
    const QString group = groupOf(m_sortKeys.at(row));
    if (delta > 0) {
        for (int i = row + 1; i < count(); ++i) {
            if (groupOf(m_sortKeys.at(i)) != group)
                return i;
        }
        return -1;
    }
    int first = row;
    while (first > 0 && groupOf(m_sortKeys.at(first - 1)) == group)
        --first;
    if (first < row)
        return first;
    if (first == 0)
        return -1;
    const QString previous = groupOf(m_sortKeys.at(first - 1));
    int start = first - 1;
    while (start > 0 && groupOf(m_sortKeys.at(start - 1)) == previous)
        --start;
    return start;
}

QString TileListModel::sortKey(const QString &title)
{
    const QString decomposed = title.normalized(QString::NormalizationForm_KD);
    QString key;
    key.reserve(decomposed.size());
    bool leading = true;
    for (const QChar c : decomposed) {
        const QChar::Category cat = c.category();
        if (cat == QChar::Mark_NonSpacing || cat == QChar::Mark_SpacingCombining || cat == QChar::Mark_Enclosing)
            continue;   // accents
        if (leading && !c.isLetterOrNumber())
            continue;   // "[", "_", "\"", "...", spaces at the start
        leading = false;
        key.append(c);
    }
    if (key.isEmpty())
        key = decomposed;
    return key.toCaseFolded();
}

bool TileListModel::sortLess(const QString &a, const QString &b)
{
    const int c = sortKey(a).compare(sortKey(b));
    return c != 0 ? c < 0 : a < b;
}

void TileListModel::setRows(const QVariantList &rows)
{
    QList<TileItem> items;
    items.reserve(rows.size());
    for (const QVariant &row : rows)
        items.append(fromVariant(row.toMap()));
    setItems(items);
}

void TileListModel::appendRow(const QVariantMap &row)
{
    insertItem(count(), fromVariant(row));
}

void TileListModel::clear()
{
    if (m_items.isEmpty())
        return;
    // A removal, not a reset: views drop their delegates (including ones
    // still incubating) cleanly.
    beginRemoveRows(QModelIndex(), 0, count() - 1);
    m_items.clear();
    m_index.clear();
    m_sortKeys.clear();
    m_duplicates = false;
    endRemoveRows();
    emit countChanged();
}

TileItem TileListModel::fromVariant(const QVariantMap &row)
{
    TileItem item;
    item.key = row.value(QStringLiteral("key")).toString();
    item.title = row.value(QStringLiteral("title")).toString();
    item.subtitle = row.value(QStringLiteral("subtitle")).toString();
    item.icon = row.value(QStringLiteral("icon")).toString();
    item.artwork = row.value(QStringLiteral("artwork")).toString();
    item.kind = row.value(QStringLiteral("kind")).toString();
    item.tint = row.value(QStringLiteral("tint")).toString();
    item.sortTitle = row.value(QStringLiteral("sortTitle")).toString();
    item.badge = row.value(QStringLiteral("badge")).toString();
    item.available = row.value(QStringLiteral("available"), true).toBool();
    return item;
}

QVariantMap TileListModel::toVariant(const TileItem &item)
{
    return {
        { QStringLiteral("key"), item.key },
        { QStringLiteral("title"), item.title },
        { QStringLiteral("subtitle"), item.subtitle },
        { QStringLiteral("icon"), item.icon },
        { QStringLiteral("artwork"), item.artwork },
        { QStringLiteral("kind"), item.kind },
        { QStringLiteral("available"), item.available },
        { QStringLiteral("tint"), item.tint },
        { QStringLiteral("sortTitle"), item.sortTitle.isEmpty() ? item.title : item.sortTitle },
        { QStringLiteral("badge"), item.badge },
    };
}
