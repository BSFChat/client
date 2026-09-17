#pragma once

// The m.call.* wire format, in ONE place — V-C3.
//
// Before this header the outbound content was built in VoiceEngine and
// the inbound content was read in ServerConnection, ~800 lines apart in
// different translation units, with nothing tying the two key sets
// together. The two ends could therefore disagree about where a field
// lives (or whether it is written at all) and the only symptom was a
// call that never connected: `call_id` mismatches are DISCARDED, not
// reported, so a peer that failed to learn our call id looped through
// invite → answer → "callId mismatch" → 30 s setup watchdog → re-offer,
// forever, with no audio and no error anywhere in the UI.
//
// Both ends now go through the builders and the parser below, so a
// round-trip test (tests/test_call_signalling.cpp) can assert on the
// REAL wire format rather than on a hand-written copy of it.
//
// Pure logic: no Qt objects beyond value types, no libdatachannel, no
// network. That is what makes it testable at all — VoiceEngine cannot
// be linked into a unit test, because it pulls in an audio device and a
// peer connection.

#include <QByteArray>
#include <QString>

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace voice {

// ---------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------
// `to` is the RECIPIENT of the event. Call signalling rides the shared
// room timeline, so in a mesh of 3+ everyone sees everyone else's
// invites/answers/candidates; without an addressee a receiver can only
// dispatch on `sender`. Receivers that predate the field fall back to
// sender-only dispatch, so it is additive on the wire.
//
// `callId` identifies WHICH call between this pair of users the event
// belongs to, and every receiver validates it. It must never be empty:
// an empty id on the wire is adopted verbatim by the peer (it stores
// whatever the invite carried) and every later event of that call then
// mismatches. The callers take it from PeerConnectionManager::callId(),
// which is owned by the peer object itself and cannot go missing while
// the peer is alive.

inline nlohmann::json buildInvite(const QString& callId, const QString& to,
                                  const std::string& sdp,
                                  const nlohmann::json& caps)
{
    return {
        {"call_id", callId.toStdString()},
        {"to", to.toStdString()},
        {"lifetime", 60000},
        {"offer", {{"type", "offer"}, {"sdp", sdp}}},
        {"bsfchat_caps", caps},
        {"version", 1},
    };
}

inline nlohmann::json buildAnswer(const QString& callId, const QString& to,
                                  const std::string& sdp,
                                  const nlohmann::json& caps)
{
    return {
        {"call_id", callId.toStdString()},
        {"to", to.toStdString()},
        {"answer", {{"type", "answer"}, {"sdp", sdp}}},
        {"bsfchat_caps", caps},
        {"version", 1},
    };
}

// Mid-call renegotiation (bsfchat.call.negotiate). `type` is "offer" or
// "answer".
inline nlohmann::json buildNegotiate(const QString& callId, const QString& to,
                                     const std::string& type,
                                     const std::string& sdp)
{
    return {
        {"call_id", callId.toStdString()},
        {"to", to.toStdString()},
        {"description", {{"type", type}, {"sdp", sdp}}},
        {"version", 1},
    };
}

inline nlohmann::json buildCandidates(
    const QString& callId, const QString& to,
    const std::vector<std::pair<std::string, std::string>>& candidates)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [candidate, mid] : candidates) {
        arr.push_back({
            {"candidate", candidate},
            {"sdpMid", mid},
            {"sdpMLineIndex", 0},
        });
    }
    return {
        {"call_id", callId.toStdString()},
        {"to", to.toStdString()},
        {"candidates", arr},
        {"version", 1},
    };
}

inline nlohmann::json buildHangup(const QString& callId, const QString& to,
                                  const std::string& reason)
{
    return {
        {"call_id", callId.toStdString()},
        {"to", to.toStdString()},
        {"reason", reason},
        {"version", 1},
    };
}

// ---------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------

struct InboundCall {
    // False when the payload was not parseable JSON, or not an object.
    bool valid = false;
    QString callId;
    // Empty when the sender did not address the event (older client) —
    // the receiver then falls back to sender-only dispatch.
    QString to;
    // m.call.invite / m.call.answer: the offer/answer SDP.
    // bsfchat.call.negotiate: `description`.
    std::string sdpType;
    std::string sdp;
    nlohmann::json caps = nlohmann::json::object();
    std::vector<std::pair<std::string, std::string>> candidates;
    std::string reason;
};

// `payload` is the event's `content` object, serialised. Keys absent
// from a given event type are simply left at their defaults; the caller
// knows which ones its event carries.
inline InboundCall parseCallContent(const QByteArray& payload)
{
    InboundCall out;
    const auto c = nlohmann::json::parse(payload.constData(),
                                         payload.constData() + payload.size(),
                                         nullptr, /*allow_exceptions=*/false);
    if (c.is_discarded() || !c.is_object()) return out;
    out.valid = true;

    // Every field is read defensively: this is data from another build,
    // and a wrong TYPE must not throw out of the sync handler.
    if (auto it = c.find("call_id"); it != c.end() && it->is_string())
        out.callId = QString::fromStdString(it->get<std::string>());
    if (auto it = c.find("to"); it != c.end() && it->is_string())
        out.to = QString::fromStdString(it->get<std::string>());
    if (auto it = c.find("reason"); it != c.end() && it->is_string())
        out.reason = it->get<std::string>();

    // offer / answer / description all carry the same {type, sdp} shape.
    for (const char* key : {"offer", "answer", "description"}) {
        auto it = c.find(key);
        if (it == c.end() || !it->is_object()) continue;
        if (auto t = it->find("type"); t != it->end() && t->is_string())
            out.sdpType = t->get<std::string>();
        if (auto s = it->find("sdp"); s != it->end() && s->is_string())
            out.sdp = s->get<std::string>();
        break;
    }

    if (auto it = c.find("bsfchat_caps"); it != c.end() && it->is_object())
        out.caps = *it;

    if (auto it = c.find("candidates"); it != c.end() && it->is_array()) {
        for (const auto& ic : *it) {
            if (!ic.is_object()) continue;
            std::string cand, mid;
            if (auto f = ic.find("candidate"); f != ic.end() && f->is_string())
                cand = f->get<std::string>();
            if (auto f = ic.find("sdpMid"); f != ic.end() && f->is_string())
                mid = f->get<std::string>();
            out.candidates.emplace_back(std::move(cand), std::move(mid));
        }
    }
    return out;
}

// True when this event is for us: either it names us, or it names
// nobody (an older sender) and we fall back to dispatching on `sender`.
inline bool addressedToUs(const QString& to, const QString& localUserId)
{
    return to.isEmpty() || to == localUserId;
}

// ---------------------------------------------------------------------
// The call-id rule
// ---------------------------------------------------------------------
// `inbound` is the id on the event, `held` the id we minted for the peer
// it came from.
//
// The strict form of this rule (inbound != held ⇒ drop) is what turned
// a peer that never learned our call id into a permanently dead call:
// nothing recovers, because every subsequent event of that call carries
// the same wrong id, and the mesh reconciler re-offers into the same
// wall every 30 s. An UNIDENTIFIED event — one carrying no id at all —
// is a different case from a MISMATCHED one: it can only have come from
// a peer that has no id to echo, which for a per-pair call means it can
// only be about the one call we have with that peer. Accepting it (and
// saying so in the log) recovers the call with no risk of applying a
// stale session's SDP, because a stale event from a previous call
// carries that call's id and is still rejected.
enum class CallIdMatch {
    Accept,
    // No id on the event; accepted as belonging to our outstanding call
    // with that peer. Worth a warning: the peer is misbehaving.
    AcceptUnidentified,
    Reject,
};

inline CallIdMatch matchCallId(const QString& inbound, const QString& held)
{
    if (inbound == held) return CallIdMatch::Accept;
    if (inbound.isEmpty() && !held.isEmpty())
        return CallIdMatch::AcceptUnidentified;
    return CallIdMatch::Reject;
}

} // namespace voice
