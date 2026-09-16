#include "model/ThreadFilterModel.h"
#include "model/MessageModel.h"

ThreadFilterModel::ThreadFilterModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    // `count` is what the drawer's empty state binds to, and the proxy's
    // row count moves for reasons the source's does not — a reply landing
    // in a DIFFERENT thread inserts a source row and changes nothing here.
    connect(this, &QAbstractItemModel::rowsInserted,
            this, &ThreadFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved,
            this, &ThreadFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset,
            this, &ThreadFilterModel::countChanged);
}

void ThreadFilterModel::setRootEventId(const QString& eventId)
{
    if (m_rootEventId == eventId) return;
    m_rootEventId = eventId;
    invalidateFilter();
    emit rootEventIdChanged();
    emit countChanged();
}

bool ThreadFilterModel::filterAcceptsRow(int sourceRow,
                                         const QModelIndex& sourceParent) const
{
    if (m_rootEventId.isEmpty()) return false;
    const QAbstractItemModel* src = sourceModel();
    if (!src) return false;

    const QModelIndex idx = src->index(sourceRow, 0, sourceParent);
    if (!idx.isValid()) return false;

    // The root message itself heads the list.
    if (src->data(idx, MessageModel::EventIdRole).toString() == m_rootEventId)
        return true;

    return src->data(idx, MessageModel::ThreadRootIdRole).toString() == m_rootEventId;
}
