// m.call.* signalling, end to end through the real wire format — V-C3.
//
// The field failure this pins down (2026-09-17, first two-Mac voice test,
// 0.0.44-rc.7): every inbound m.call.answer arrived with an EMPTY call
// id, so `callId != m_callIds.value(sender)` discarded all of them, ICE
// never completed, the 30 s setup watchdog reaped the peer and the mesh
// reconciler re-offered — forever, with no audio and nothing in the UI
// to say why. The two halves of the wire format lived ~800 lines apart
// in two files (VoiceEngine built the content, ServerConnection read it)
// with no test tying them together and no way to notice a dropped field.
//
// Both halves now go through src/voice/CallSignalCodec.h, and this
// exercises them together: build an event the way VoiceEngine does, take
// it through the protocol's sync parser exactly as MatrixClient does,
// and read it back the way ServerConnection::dispatchCallSignal does.
//
// No server, no libdatachannel, no audio device, no GUI.

#include "voice/CallSignalCodec.h"

#include <bsfchat/MatrixTypes.h>

#include <QObject>
#include <QTest>

using nlohmann::json;

namespace {

constexpr const char* kRoom = "!zjszJ_6upIHm-MG39k:chat.bsfchat.com";
constexpr const char* kUs = "@josh:chat.bsfchat.com";
constexpr const char* kPeer = "@oidc_19f18682:chat.bsfchat.com";

// A realistic answer SDP — the field case was 1144 bytes.
std::string sampleSdp()
{
    return "v=0\r\no=rtc 809468162 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
           "a=group:BUNDLE 0\r\na=setup:active\r\na=ice-ufrag:abcd\r\n"
           + std::string(1000, 'x') + "\r\n";
}

json capsJson()
{
    return {{"video_rtp", 1}, {"control_dc", 1},
            {"video_codecs", json::array({"h264"})},
            {"h264_profiles_decode", json::array({"cb", "high"})},
            {"h264_profiles_encode", json::array({"high"})},
            {"lossless", json::array()}};
}

// Everything between VoiceEngine::sendCallEvent and
// ServerConnection::dispatchCallSignal: the PUT body is stored verbatim
// by the server and handed back inside a /sync timeline, which
// MatrixClient feeds to bsfchat::from_json, and ServerConnection then
// re-serialises as CallSignal::payload.
QByteArray throughSync(const json& content, const char* type)
{
    const json sync = {
        {"next_batch", "s1"},
        {"rooms", {{"join", {{kRoom, {
            {"timeline", {{"events", json::array({json{
                {"event_id", "$1"},
                {"room_id", kRoom},
                {"sender", kPeer},
                {"type", type},
                {"content", json::parse(content.dump())},
                {"origin_server_ts", 1789607924437}}})}}}}}}}}}};

    bsfchat::SyncResponse resp;
    bsfchat::from_json(sync, resp);
    const auto& events = resp.rooms.join.at(kRoom).timeline.events;
    if (events.empty()) return {};
    return QByteArray::fromStdString(events.front().content.data.dump());
}

} // namespace

class TestCallSignalling : public QObject {
    Q_OBJECT

private slots:
    // ---- the regression: a call id survives the whole round trip ----

    // V-C3. This is the test the field failure would have needed: the
    // answer VoiceEngine sends, read back by the code that dispatches
    // it, must carry the SAME call id. Before the codec the two ends
    // were independent literal key sets in different files.
    void answerRoundTripKeepsCallId()
    {
        const QString callId = QStringLiteral("call-1789607924187-6ec9e4fc");
        const auto payload = throughSync(
            voice::buildAnswer(callId, QString::fromUtf8(kUs), sampleSdp(),
                               capsJson()),
            "m.call.answer");

        const voice::InboundCall in = voice::parseCallContent(payload);
        QVERIFY(in.valid);
        QCOMPARE(in.callId, callId);
        QCOMPARE(in.to, QString(kUs));
        QCOMPARE(in.sdp, sampleSdp());
        QCOMPARE(in.sdpType, std::string("answer"));
        QCOMPARE(in.caps.value("video_rtp", 0), 1);
    }

    void inviteRoundTripKeepsCallId()
    {
        const QString callId = QStringLiteral("call-1789607924187-6ec9e4fc");
        const auto payload = throughSync(
            voice::buildInvite(callId, QString::fromUtf8(kUs), sampleSdp(),
                               capsJson()),
            "m.call.invite");

        const voice::InboundCall in = voice::parseCallContent(payload);
        QVERIFY(in.valid);
        QCOMPARE(in.callId, callId);
        QCOMPARE(in.sdpType, std::string("offer"));
        QCOMPARE(in.sdp, sampleSdp());
    }

    void candidatesRoundTripKeepsCallIdAndBatch()
    {
        const QString callId = QStringLiteral("call-7");
        const std::vector<std::pair<std::string, std::string>> cands{
            {"a=candidate:1 1 UDP 2122317823 10.0.141.142 61436 typ host", "0"},
            {"a=candidate:2 1 UDP 1686110207 82.12.1.2 61436 typ srflx", "0"}};
        const auto payload = throughSync(
            voice::buildCandidates(callId, QString::fromUtf8(kUs), cands),
            "m.call.candidates");

        const voice::InboundCall in = voice::parseCallContent(payload);
        QVERIFY(in.valid);
        QCOMPARE(in.callId, callId);
        QCOMPARE(int(in.candidates.size()), 2);
        QCOMPARE(in.candidates[1].first, cands[1].first);
        QCOMPARE(in.candidates[0].second, std::string("0"));
    }

    void negotiateRoundTripKeepsTypeAndCallId()
    {
        const QString callId = QStringLiteral("call-9");
        const auto payload = throughSync(
            voice::buildNegotiate(callId, QString::fromUtf8(kUs), "offer",
                                  sampleSdp()),
            "bsfchat.call.negotiate");

        const voice::InboundCall in = voice::parseCallContent(payload);
        QCOMPARE(in.callId, callId);
        QCOMPARE(in.sdpType, std::string("offer"));
        QCOMPARE(in.sdp, sampleSdp());
    }

    void hangupRoundTripKeepsReason()
    {
        const auto payload = throughSync(
            voice::buildHangup(QStringLiteral("call-3"), QString::fromUtf8(kUs),
                               "user_hangup"),
            "m.call.hangup");
        const voice::InboundCall in = voice::parseCallContent(payload);
        QCOMPARE(in.callId, QStringLiteral("call-3"));
        QCOMPARE(in.reason, std::string("user_hangup"));
    }

    // ---- the call-id rule ----

    // THE FIELD CASE. An answer that carries no call id at all can only
    // have come from a peer that never learned ours, and it can only be
    // about the one call we hold with that peer. The shipped rule was a
    // bare `inbound != held`, which rejected it — and since every later
    // event of that call carried the same nothing, the call could never
    // recover.
    void unidentifiedEventIsAcceptedForOurOutstandingCall()
    {
        QCOMPARE(voice::matchCallId(QString(),
                                    QStringLiteral("call-1789607924187-6ec9")),
                 voice::CallIdMatch::AcceptUnidentified);
    }

    // What the strict check exists for, and what must keep working: a
    // stale event from a call we have already replaced.
    void mismatchedCallIdIsStillRejected()
    {
        QCOMPARE(voice::matchCallId(QStringLiteral("call-old"),
                                    QStringLiteral("call-new")),
                 voice::CallIdMatch::Reject);
    }

    void matchingCallIdIsAccepted()
    {
        QCOMPARE(voice::matchCallId(QStringLiteral("call-1"),
                                    QStringLiteral("call-1")),
                 voice::CallIdMatch::Accept);
    }

    // We hold no id for this peer (no outstanding call), but the event
    // names one: not ours to apply.
    void identifiedEventWithNoHeldCallIsRejected()
    {
        QCOMPARE(voice::matchCallId(QStringLiteral("call-1"), QString()),
                 voice::CallIdMatch::Reject);
    }

    // ---- parsing is defensive ----

    // An answer from a build that drops the field is exactly the field
    // payload: sdp present, call id absent. It must parse, not throw,
    // and report an empty id rather than a wrong one.
    void answerWithoutCallIdStillParses()
    {
        json content = {
            {"to", kUs},
            {"answer", {{"type", "answer"}, {"sdp", sampleSdp()}}},
            {"version", 1}};
        const voice::InboundCall in =
            voice::parseCallContent(throughSync(content, "m.call.answer"));
        QVERIFY(in.valid);
        QVERIFY(in.callId.isEmpty());
        QCOMPARE(in.sdp.size(), sampleSdp().size());
    }

    void wrongTypesDoNotThrow()
    {
        const voice::InboundCall in = voice::parseCallContent(
            QByteArray(R"({"call_id":42,"to":[],"answer":"nope",)"
                       R"("candidates":{},"bsfchat_caps":7})"));
        QVERIFY(in.valid);
        QVERIFY(in.callId.isEmpty());
        QVERIFY(in.sdp.empty());
        QVERIFY(in.candidates.empty());
        QVERIFY(in.caps.is_object());
    }

    void malformedPayloadIsRejected()
    {
        QVERIFY(!voice::parseCallContent(QByteArray("{not json")).valid);
        QVERIFY(!voice::parseCallContent(QByteArray("[1,2,3]")).valid);
        QVERIFY(!voice::parseCallContent(QByteArray()).valid);
    }

    // ---- addressing ----

    void addressingAcceptsOursAndUnaddressed()
    {
        // Ours.
        QVERIFY(voice::addressedToUs(QString::fromUtf8(kUs), QString::fromUtf8(kUs)));
        // Older sender, no `to` at all: fall back to sender-only
        // dispatch rather than dropping a mixed-fleet peer's events.
        QVERIFY(voice::addressedToUs(QString(), QString::fromUtf8(kUs)));
        // Somebody else's, in a mesh of 3+.
        QVERIFY(!voice::addressedToUs(QString::fromUtf8(kPeer),
                                      QString::fromUtf8(kUs)));
    }

    // The addressee has to survive the round trip too — a `to` that came
    // back empty would make every client in a 3+ mesh apply everyone
    // else's offers (the bug the field addressing was added for).
    void addresseeSurvivesRoundTrip()
    {
        const auto payload = throughSync(
            voice::buildAnswer(QStringLiteral("call-1"), QString::fromUtf8(kUs),
                               sampleSdp(), capsJson()),
            "m.call.answer");
        QCOMPARE(voice::parseCallContent(payload).to, QString(kUs));
    }
};

QTEST_MAIN(TestCallSignalling)
#include "test_call_signalling.moc"
