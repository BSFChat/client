#pragma once

#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QUuid>

#include <array>
#include <memory>
#include <utility>

// Waiting for the answer to ONE request on a shared pair of signals.
//
// MatrixClient reports asynchronous results on connection-wide signals
// (mediaUploaded/mediaUploadError, createRoomSuccess/createRoomError), and
// several unrelated flows wait on the same pair at the same time: a file
// attachment and an avatar change both upload; a DM and a channel are both
// created. Those call sites used Qt::SingleShotConnection on the assumption
// that it means "only one slot runs per emission". It does not — it
// disconnects a slot *after* it has run, but one `emit` still invokes every
// connected slot. The consequences were all live bugs:
//
//   * two uploads in flight: whichever finished first delivered its mxc://
//     URI to BOTH handlers, so a file attachment was posted carrying the
//     avatar's image and the avatar was set to the attachment;
//   * the loser of each pair never fired and so never disconnected, leaving
//     a handler armed for somebody else's later upload — one leaked handler
//     per failed send, for the life of the connection;
//   * two DMs started before either replied: the first room id was written
//     to both handlers, so one DM was labelled with the other's peer and
//     the second room was never recorded as a DM at all.
//
// The fix is a token minted per request and echoed back by the signal.
// awaitTokenedReply connects both halves, ignores replies belonging to
// anyone else, and tears BOTH connections down as soon as either fires.
namespace bsfchat::net {

// A token unique for the process, to correlate a request with its reply.
inline QString newRequestToken()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Both signals must have the shape (QString token, QString payload).
// `onOk` / `onErr` are called with the payload only, exactly once between
// them, and only for `token`.
template <typename Sender, typename OkSignal, typename ErrSignal,
          typename OkFn, typename ErrFn>
void awaitTokenedReply(Sender* sender, QObject* context, QString token,
                       OkSignal okSignal, ErrSignal errSignal,
                       OkFn onOk, ErrFn onErr)
{
    // Shared so each lambda can disconnect the other; the last lambda
    // destroyed releases it.
    auto conns = std::make_shared<std::array<QMetaObject::Connection, 2>>();
    auto finish = [conns]() {
        QObject::disconnect((*conns)[0]);
        QObject::disconnect((*conns)[1]);
    };

    (*conns)[0] = QObject::connect(
        sender, okSignal, context,
        [token, finish, onOk = std::move(onOk)](const QString& id,
                                                const QString& payload) {
            if (id != token) return;
            finish();
            onOk(payload);
        });
    (*conns)[1] = QObject::connect(
        sender, errSignal, context,
        [token, finish, onErr = std::move(onErr)](const QString& id,
                                                  const QString& payload) {
            if (id != token) return;
            finish();
            onErr(payload);
        });
}

} // namespace bsfchat::net
