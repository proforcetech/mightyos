#include "tilelistmodel.h"

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

void TileListModel::rebuildIndex()
{
    m_index.clear();
    m_index.reserve(m_items.size());
    m_sortKeys.clear();
    m_sortKeys.reserve(m_items.size());
    for (const TileItem &item : std::as_const(m_items))
        m_sortKeys.append(sortKey(item.sortTitle.isEmpty() ? item.title : item.sortTitle));
    // Walk backwards so that, with duplicate keys, the first row wins.
    for (int i = count() - 1; i >= 0; --i)
        m_index.insert(m_items.at(i).key, i);
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
            m_items[i] = items.at(i);
            const QModelIndex idx = index(i);
            emit dataChanged(idx, idx);
        }
        return;
    }

    const int oldCount = count();
    emit itemsAboutToReset();
    beginResetModel();
    m_items = items;
    rebuildIndex();
    endResetModel();
    if (oldCount != count())
        emit countChanged();
    emit itemsReset();
}

void TileListModel::insertItem(int row, const TileItem &item)
{
    row = qBound(0, row, count());
    beginInsertRows(QModelIndex(), row, row);
    m_items.insert(row, item);
    rebuildIndex();
    endInsertRows();
    emit countChanged();
}

bool TileListModel::removeAt(int row)
{
    if (row < 0 || row >= count())
        return false;
    beginRemoveRows(QModelIndex(), row, row);
    m_items.removeAt(row);
    rebuildIndex();
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
    rebuildIndex();
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
    if (keyChanged)
        rebuildIndex();
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
