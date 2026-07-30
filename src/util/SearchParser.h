#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// Parsing for POST /_matrix/client/v3/search responses.
//
// Split out of MatrixClient deliberately: it is pure, has no Qt Network
// dependency, and is the part with all the ways to be wrong (an error body, a
// truncated payload, a result entry with no event, an absent next_batch). Being
// a free function means test_models can drive it directly without a server.
namespace bsfchat::client {

// One rendered search hit. Everything the results list needs, already flattened
// out of the nested {rank, result: <event>} envelope.
struct SearchHit {
    QString eventId;
    QString roomId;
    QString sender;
    QString body;
    QString msgtype;
    qint64 timestamp = 0;
    // Server flips bm25 for us, so higher is a better match.
    double rank = 0.0;
};

struct SearchResponse {
    // False when the payload was not a usable search result — either a Matrix
    // error object or unparseable JSON. `errorMessage` is then set.
    bool ok = false;
    QVector<SearchHit> hits;
    // Total matches the server has, which can exceed hits.size() when the
    // result set is paginated.
    int count = 0;
    // The terms the server actually searched, for highlighting in the UI. Note
    // these are the SERVER's tokenisation of the query, not the raw input.
    QStringList highlights;
    // Opaque offset token. Empty means "no further pages".
    QString nextBatch;
    // Matrix errcode (e.g. M_INVALID_PARAM), when the body was an error object.
    QString errorCode;
    QString errorMessage;
};

// Parses a response body. `httpStatus` participates because a non-2xx status
// with an unparseable body still has to produce a sane message, and because the
// server distinguishes 400 (bad query) from 501 (no FTS5 module in this
// server's SQLite build) — which are very different things to tell a user.
SearchResponse parseSearchResponse(int httpStatus, const QByteArray& body);

} // namespace bsfchat::client
