#include "util/SearchParser.h"

#include <nlohmann/json.hpp>

namespace bsfchat::client {

namespace {

using json = nlohmann::json;

QString str(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return QString::fromStdString(it->get<std::string>());
}

// Turns a Matrix error object into something worth showing a user.
//
// The server's own message is preferred when there is one — it is written for
// humans and is more specific than anything we could guess ("search_term is too
// long", "Search term is not usable"). The fallbacks exist because a proxy or a
// crash can produce a non-2xx with a body that is not a Matrix error at all,
// and "nothing happened" is the one outcome a search box must never have.
QString messageForStatus(int httpStatus, const QString& serverMessage,
                         const QString& errorCode)
{
    if (!serverMessage.isEmpty()) return serverMessage;
    switch (httpStatus) {
    case 400:
        return QStringLiteral("That search couldn't be run. Try different words.");
    case 401:
    case 403:
        return QStringLiteral("You're not allowed to search here. Try signing in again.");
    case 501:
        return QStringLiteral("This server doesn't support message search.");
    case 502:
    case 503:
    case 504:
        return QStringLiteral("The server is unavailable. Try again shortly.");
    default:
        break;
    }
    if (!errorCode.isEmpty()) {
        return QStringLiteral("Search failed (%1).").arg(errorCode);
    }
    return QStringLiteral("Search failed (HTTP %1).").arg(httpStatus);
}

} // namespace

SearchResponse parseSearchResponse(int httpStatus, const QByteArray& body)
{
    SearchResponse out;

    auto parsed = json::parse(body.constData(), body.constData() + body.size(),
                              nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        // Unparseable. If the status was fine this is still a failure: silently
        // showing "no matches" for a broken payload is the worst option.
        out.errorMessage = messageForStatus(
            httpStatus >= 400 ? httpStatus : 502, {}, {});
        return out;
    }

    // A Matrix error object is recognised by errcode, not by status, so an
    // error delivered with a 200 (a misbehaving proxy) still reads as an error.
    const QString errcode = str(parsed, "errcode");
    if (!errcode.isEmpty() || httpStatus >= 400) {
        out.errorCode = errcode;
        out.errorMessage = messageForStatus(httpStatus, str(parsed, "error"), errcode);
        return out;
    }

    auto cats = parsed.find("search_categories");
    if (cats == parsed.end() || !cats->is_object()) {
        out.errorMessage = messageForStatus(502, {}, {});
        return out;
    }
    auto roomEvents = cats->find("room_events");
    if (roomEvents == cats->end() || !roomEvents->is_object()) {
        // Well-formed response for a category we didn't ask about. Treat as an
        // empty — but successful — result rather than an error.
        out.ok = true;
        return out;
    }

    out.ok = true;
    if (auto it = roomEvents->find("count");
        it != roomEvents->end() && it->is_number_integer()) {
        out.count = it->get<int>();
    }
    out.nextBatch = str(*roomEvents, "next_batch");
    if (auto it = roomEvents->find("highlights");
        it != roomEvents->end() && it->is_array()) {
        for (const auto& h : *it) {
            if (h.is_string()) out.highlights.append(QString::fromStdString(
                h.get<std::string>()));
        }
    }

    auto results = roomEvents->find("results");
    if (results == roomEvents->end() || !results->is_array()) return out;

    out.hits.reserve(static_cast<int>(results->size()));
    for (const auto& entry : *results) {
        if (!entry.is_object()) continue;
        auto ev = entry.find("result");
        if (ev == entry.end() || !ev->is_object()) continue;

        SearchHit hit;
        hit.eventId = str(*ev, "event_id");
        // Without an event id the row cannot be jumped to, which is the only
        // thing a result is FOR. Drop it rather than render a dead row.
        if (hit.eventId.isEmpty()) continue;
        hit.roomId = str(*ev, "room_id");
        hit.sender = str(*ev, "sender");
        if (auto ts = ev->find("origin_server_ts");
            ts != ev->end() && ts->is_number()) {
            hit.timestamp = ts->get<qint64>();
        }
        if (auto c = ev->find("content"); c != ev->end() && c->is_object()) {
            hit.body = str(*c, "body");
            hit.msgtype = str(*c, "msgtype");
        }
        if (auto r = entry.find("rank"); r != entry.end() && r->is_number()) {
            hit.rank = r->get<double>();
        }
        out.hits.append(std::move(hit));
    }

    return out;
}

} // namespace bsfchat::client
