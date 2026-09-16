#pragma once

#include <QSortFilterProxyModel>
#include <QString>

// A live view of one thread over a MessageModel (U-M8).
//
// The thread drawer used to rebuild a plain JS array — eventPreview() for
// the root, then threadReplies() for the children — every time the room's
// message count ticked. Three things followed from that, all of them
// visible:
//
//   * an edit or a reaction inside a thread never showed, because neither
//     changes the row count and nothing else re-ran the builder;
//   * every rebuild handed the ListView a brand-new model object, so all
//     the delegates were destroyed and recreated and the drawer's scroll
//     position jumped back to the top mid-conversation;
//   * the rows were snapshots, so they carried only the handful of fields
//     threadReplies() chose to copy — the drawer could never render
//     anything the timeline could.
//
// As a proxy it is the same rows, with the same roles, kept in step by the
// source model's own dataChanged / rowsInserted / rowsRemoved. An edit
// repaints one delegate and nothing moves.
//
// Row order is the source's, which is oldest-first — the order a thread
// reads in. Nothing is sorted.
class ThreadFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString rootEventId READ rootEventId WRITE setRootEventId
                   NOTIFY rootEventIdChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    explicit ThreadFilterModel(QObject* parent = nullptr);

    QString rootEventId() const { return m_rootEventId; }
    void setRootEventId(const QString& eventId);

signals:
    void rootEventIdChanged();
    void countChanged();

protected:
    // Accepts the thread's root message itself plus every message whose
    // ThreadRootIdRole names it, so the drawer shows [parent, ...replies]
    // exactly as it did when it built that list by hand.
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString m_rootEventId;
};
