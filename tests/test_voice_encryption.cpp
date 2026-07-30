// Media-key handling and — just as important — the words used to describe it.
//
// Two failure modes are being guarded against here, and the second is the one
// this project has actually shipped before:
//
//   1. Silently connecting unencrypted when encryption was expected. A client
//      that believes it is encrypted and is not is worse off than one that
//      knows it is in the clear, so every ambiguous case must refuse.
//   2. Describing shared-key relay encryption as end-to-end. The voice badge
//      once read "MLS · 256" against a DTLS+SCTP reality and had to be
//      corrected. The string tests below exist so that cannot recur silently.

#include <QtTest>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

#include "voice/VoiceEncryption.h"

using namespace voice;

namespace {

// A .qml file split into "what runs" and "what it says".
//
// Both halves are needed and they must not be confused. Scanning raw
// source for a banned word would flag the comment that explains the ban;
// scanning it for the binding would be satisfied BY that comment — which
// is not hypothetical, it is what the first version of this file did,
// and the mutation that deleted the binding passed.
//
// So: one pass, comments excluded from both outputs.
struct QmlScan {
    QString code;          // source with comments removed
    QStringList literals;  // string literals appearing in that code
};

QmlScan scanQml(const QString& src) {
    QmlScan out;
    out.code.reserve(src.size());
    const qsizetype n = src.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = src.at(i);
        const QChar next = (i + 1 < n) ? src.at(i + 1) : QChar();

        if (c == u'/' && next == u'/') {                 // line comment
            while (i < n && src.at(i) != u'\n') ++i;
            out.code += u'\n';
            continue;
        }
        if (c == u'/' && next == u'*') {                 // block comment
            i += 2;
            while (i + 1 < n && !(src.at(i) == u'*' && src.at(i + 1) == u'/')) ++i;
            ++i;
            continue;
        }
        if (c == u'"' || c == u'\'' || c == u'`') {      // string literal
            const QChar quote = c;
            QString lit;
            lit += c;
            ++i;
            for (; i < n; ++i) {
                const QChar d = src.at(i);
                lit += d;
                if (d == u'\\') {                        // escape: skip next
                    if (i + 1 < n) lit += src.at(++i);
                    continue;
                }
                if (d == quote) break;
                // An unterminated literal would otherwise swallow the
                // rest of the file. Newlines only end ' and " (a
                // backtick template legitimately spans lines).
                if (d == u'\n' && quote != u'`') break;
            }
            out.literals << lit;
            out.code += lit;
            continue;
        }
        out.code += c;
    }
    return out;
}

QStringList qmlFiles() {
    QStringList out;
    QDirIterator it(QStringLiteral(BSFCHAT_QML_DIR), {QStringLiteral("*.qml")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) out << it.next();
    out.sort();
    return out;
}

QString readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QJsonObject goodResponse(const QByteArray& key = QByteArray(kSharedKeyBytes, 'k'),
                         const QString& mode = QLatin1String(kEncryptionModeSharedKey)) {
    QJsonObject enc;
    enc[QStringLiteral("mode")] = mode;
    enc[QStringLiteral("key")] = QString::fromUtf8(key.toBase64());
    enc[QStringLiteral("key_generation")] = 3;

    QJsonObject o;
    o[QStringLiteral("url")] = QStringLiteral("wss://sfu.example.org");
    o[QStringLiteral("token")] = QStringLiteral("eyJhbG.payload.sig");
    o[QStringLiteral("room")] = QStringLiteral("bsfchat-deadbeef");
    o[QStringLiteral("identity")] = QStringLiteral("@me:example.org|DEV1");
    o[QStringLiteral("ttl")] = 600;
    o[QStringLiteral("encryption")] = enc;
    return o;
}

} // namespace

class TestVoiceEncryption : public QObject {
    Q_OBJECT

private slots:
    // ---- Parsing ---------------------------------------------------

    void parsesAWellFormedResponse() {
        const auto r = parseLiveKitJoin(goodResponse());
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.config.url, QStringLiteral("wss://sfu.example.org"));
        QCOMPARE(r.config.room, QStringLiteral("bsfchat-deadbeef"));
        QCOMPARE(r.config.ttlSeconds, 600);
        QVERIFY(r.config.hasKey());
        QCOMPARE(r.config.key.size(), kSharedKeyBytes);
        QCOMPARE(r.config.keyGeneration, 3);
    }

    // A server with encryption switched off sends no block. That is a valid
    // configuration reported honestly as SfuNone, not an error — conflating
    // the two would train people to click through the warning that matters.
    void anAbsentEncryptionBlockIsNotAnError() {
        auto o = goodResponse();
        o.remove(QStringLiteral("encryption"));
        const auto r = parseLiveKitJoin(o);
        QVERIFY(r.ok());
        QVERIFY(!r.config.hasKey());
    }

    void missingConnectionFieldsAreRejected() {
        for (const QString& field : {QStringLiteral("url"), QStringLiteral("token"),
                                     QStringLiteral("room")}) {
            auto o = goodResponse();
            o.remove(field);
            const auto r = parseLiveKitJoin(o);
            QVERIFY2(!r.ok(), qPrintable(QStringLiteral("missing %1 was accepted").arg(field)));
            QVERIFY(!r.error.isEmpty());
        }
    }

    // The core refusal: anything wrong with the key must stop the join, never
    // degrade it to an unencrypted connection.
    void aShortKeyIsRejectedRatherThanUsed() {
        const auto r = parseLiveKitJoin(goodResponse(QByteArray(16, 'k')));
        QVERIFY(!r.ok());
        QVERIFY(!r.config.hasKey());
    }

    void anOversizedKeyIsRejected() {
        QVERIFY(!parseLiveKitJoin(goodResponse(QByteArray(64, 'k'))).ok());
    }

    // Junk INSIDE otherwise-valid base64 is the case that makes the strict
    // decoder load-bearing. QByteArray::fromBase64's lenient mode skips the
    // invalid characters and still yields a full 32 bytes, so the length
    // check alone would wave this through and the call would then be
    // encrypted with a key the server never issued — every participant
    // silently undecodable to the others.
    void base64JunkThatStillDecodesToTheRightLengthIsRejected() {
        const QByteArray valid = QByteArray(kSharedKeyBytes, 'k').toBase64();
        QByteArray corrupted = valid;
        corrupted.insert(8, " \n*");   // characters lenient mode drops
        // Precondition: lenient decoding really does yield a full-size key,
        // so this test is exercising the strict check and not the length one.
        QCOMPARE(QByteArray::fromBase64(corrupted).size(), kSharedKeyBytes);

        auto o = goodResponse();
        auto enc = o.value(QStringLiteral("encryption")).toObject();
        enc[QStringLiteral("key")] = QString::fromUtf8(corrupted);
        o[QStringLiteral("encryption")] = enc;
        const auto r = parseLiveKitJoin(o);
        QVERIFY2(!r.ok(), "corrupted base64 that decodes to 32 bytes was accepted");
        QVERIFY(r.config.key.isEmpty());
    }

    void aMalformedBase64KeyIsRejected() {
        auto o = goodResponse();
        auto enc = o.value(QStringLiteral("encryption")).toObject();
        enc[QStringLiteral("key")] = QStringLiteral("!!!! not base64 !!!!");
        o[QStringLiteral("encryption")] = enc;
        const auto r = parseLiveKitJoin(o);
        QVERIFY2(!r.ok(),
                 "QByteArray::fromBase64's lenient mode would have silently "
                 "decoded this to something short.");
    }

    void anEmptyKeyIsRejected() {
        auto o = goodResponse();
        auto enc = o.value(QStringLiteral("encryption")).toObject();
        enc[QStringLiteral("key")] = QString();
        o[QStringLiteral("encryption")] = enc;
        QVERIFY(!parseLiveKitJoin(o).ok());
    }

    // A future server offering a scheme this build cannot do must not be
    // downgraded to one it can.
    void anUnknownEncryptionModeIsRefusedNotDowngraded() {
        const auto r = parseLiveKitJoin(
            goodResponse(QByteArray(kSharedKeyBytes, 'k'), QStringLiteral("mls_v2")));
        QVERIFY(!r.ok());
        QVERIFY(!r.config.hasKey());
    }

    void aMalformedEncryptionBlockIsRejected() {
        auto o = goodResponse();
        o[QStringLiteral("encryption")] = QStringLiteral("yes please");
        QVERIFY(!parseLiveKitJoin(o).ok());
    }

    // Whenever parsing fails, no key survives into the config — so a caller
    // that ignored the error flag still cannot connect with partial material.
    void aFailedParseNeverYieldsAKey() {
        auto o = goodResponse(QByteArray(16, 'k'));
        const auto r = parseLiveKitJoin(o);
        QVERIFY(!r.ok());
        QVERIFY(r.config.key.isEmpty());
    }

    void theWireModeStringIsStable() {
        QCOMPARE(QLatin1String(kEncryptionModeSharedKey), QLatin1String("shared_key"));
        QCOMPARE(kSharedKeyBytes, 32); // AES-GCM-256
    }

    // ---- Labelling -------------------------------------------------
    //
    // These assert the ABSENCE of a claim, which is unusual but is exactly
    // what went wrong last time.

    void noLabelEverClaimsEndToEndEncryption() {
        for (auto p : {MediaProtection::MeshDtls, MediaProtection::SfuSharedKey,
                       MediaProtection::SfuNone, MediaProtection::Failed}) {
            for (const QString& s : {protectionBadge(p), protectionDetail(p)}) {
                const QString lower = s.toLower();
                // "not end-to-end encryption" is the one legitimate use, so
                // check for the claim rather than the substring.
                const qsizetype idx = lower.indexOf(QStringLiteral("end-to-end"));
                if (idx >= 0) {
                    const QString before = lower.left(idx);
                    QVERIFY2(before.endsWith(QStringLiteral("not ")) ||
                                 before.endsWith(QStringLiteral("not an ")),
                             qPrintable(QStringLiteral("'end-to-end' used as a claim in: %1").arg(s)));
                }
                QVERIFY2(!lower.contains(QStringLiteral("e2e")),
                         qPrintable(QStringLiteral("E2EE claimed in: %1").arg(s)));
                QVERIFY2(!lower.contains(QStringLiteral("mls")),
                         qPrintable(QStringLiteral("MLS claimed in: %1 — we do not "
                                                   "implement MLS").arg(s)));
            }
        }
    }

    // The guarantee and the limitation must appear together. A user who reads
    // only the shared-key description must not come away believing the server
    // cannot read their call.
    void theSharedKeyDetailStatesBothLimitations() {
        const QString d = protectionDetail(MediaProtection::SfuSharedKey).toLower();
        QVERIFY2(d.contains(QStringLiteral("not end-to-end")),
                 "must say plainly that this is not end-to-end encryption");
        QVERIFY2(d.contains(QStringLiteral("server")) && d.contains(QStringLiteral("key")),
                 "must say the server holds the key");
        QVERIFY2(d.contains(QStringLiteral("left")) || d.contains(QStringLiteral("rotate")),
                 "must say a departed member can still decrypt until rotation");
    }

    // The badge must not read as a bare "Encrypted" — that is the word people
    // round up to "nobody can read this".
    void theSharedKeyBadgeQualifiesItself() {
        const QString b = protectionBadge(MediaProtection::SfuSharedKey);
        QVERIFY(b.toLower().contains(QStringLiteral("shared")));
        QCOMPARE(b.compare(QStringLiteral("encrypted"), Qt::CaseInsensitive) == 0, false);
    }

    void theMeshBadgeIsUnchanged() {
        // Corrected from "MLS · 256" once already. Pin it.
        QCOMPARE(protectionBadge(MediaProtection::MeshDtls), QStringLiteral("DTLS · SCTP"));
    }

    void anUnencryptedRelayIsLabelledPlainly() {
        const QString b = protectionBadge(MediaProtection::SfuNone).toLower();
        QVERIFY(b.contains(QStringLiteral("not encrypted")));
        QVERIFY(protectionDetail(MediaProtection::SfuNone).toLower()
                    .contains(QStringLiteral("unencrypted")));
    }

    void everyStateHasBadgeAndDetailText() {
        for (auto p : {MediaProtection::MeshDtls, MediaProtection::SfuSharedKey,
                       MediaProtection::SfuNone, MediaProtection::Failed}) {
            QVERIFY(!protectionBadge(p).isEmpty());
            QVERIFY(!protectionDetail(p).isEmpty());
        }
    }

    // ---- Session → protection mapping ------------------------------
    //
    // The one place "what did we actually connect with" turns into "what may
    // we say about it". A caller that picks a label by hand is how the QML
    // badge ended up hard-coding the mesh protocol.

    void meshIsMeshRegardlessOfAnySharedKey() {
        // A mesh session has no shared key at all, so the flag must not
        // reach the answer in either direction.
        QCOMPARE(protectionForSession(SessionTransport::Mesh, false),
                 MediaProtection::MeshDtls);
        QCOMPARE(protectionForSession(SessionTransport::Mesh, true),
                 MediaProtection::MeshDtls);
    }

    // An SFU without a key relays media it can read. That must NOT round up
    // to "encrypted" on the strength of the TLS hop to the relay.
    void anSfuWithoutAKeyIsLabelledUnencrypted() {
        QCOMPARE(protectionForSession(SessionTransport::Sfu, false),
                 MediaProtection::SfuNone);
    }

    void anSfuWithAKeyIsSharedKeyNotEndToEnd() {
        QCOMPARE(protectionForSession(SessionTransport::Sfu, true),
                 MediaProtection::SfuSharedKey);
    }

    // Failed means "encryption was asked for and could not be set up" — a
    // decision made before a session exists. Deriving it from a session that
    // DID start would let a live call display the refusal text.
    void aStartedSessionIsNeverLabelledFailed() {
        for (auto t : {SessionTransport::Mesh, SessionTransport::Sfu}) {
            for (bool key : {false, true}) {
                QVERIFY(protectionForSession(t, key) != MediaProtection::Failed);
                QVERIFY(!encryptionFailureIsFatal(protectionForSession(t, key)));
            }
        }
    }

    // ---- The QML-facing surface ------------------------------------
    //
    // ServerConnection::voiceProtectionBadge/Detail are one-line wrappers
    // around the two functions above plus an empty string when no session is
    // live. Instantiating ServerConnection needs the whole app, so what is
    // pinned here is (a) the exact strings that surface can produce and (b)
    // that the QML actually reads it instead of writing its own.

    // The regression check for the property swap: a mesh call must display
    // exactly what the hard-coded literal used to display.
    void aMeshCallStillReadsDtlsSctp() {
        QCOMPARE(protectionBadge(protectionForSession(SessionTransport::Mesh, false)),
                 QStringLiteral("DTLS · SCTP"));
    }

    // Same absence rule as noLabelEverClaimsEndToEndEncryption, applied to
    // the set of strings reachable through the property rather than to the
    // enum — so a new session state cannot slip a claim through by being
    // reachable from protectionForSession but forgotten in the loop above.
    void noStringReachableFromASessionClaimsEndToEnd() {
        QStringList reachable;
        for (auto t : {SessionTransport::Mesh, SessionTransport::Sfu}) {
            for (bool key : {false, true}) {
                const auto p = protectionForSession(t, key);
                reachable << protectionBadge(p) << protectionDetail(p);
            }
        }
        // The idle state: no session, no claim. QML hides the badge on this.
        reachable << QString();

        for (const QString& s : reachable) {
            const QString lower = s.toLower();
            const qsizetype idx = lower.indexOf(QStringLiteral("end-to-end"));
            if (idx >= 0) {
                const QString before = lower.left(idx);
                QVERIFY2(before.endsWith(QStringLiteral("not ")) ||
                             before.endsWith(QStringLiteral("not an ")),
                         qPrintable(QStringLiteral("'end-to-end' used as a claim in: %1").arg(s)));
            }
            QVERIFY2(!lower.contains(QStringLiteral("e2e")), qPrintable(s));
            QVERIFY2(!lower.contains(QStringLiteral("mls")), qPrintable(s));
        }
    }

    // The badge is bound, not written. Without this, deleting the badge
    // entirely would leave the scan below trivially green — a test passing
    // because there is nothing left to check.
    //
    // Checked against the COMMENT-STRIPPED source. Checking the raw file
    // was the first version of this test and it did not work: the comment
    // above the badge names the property, so the assertion held even with
    // the binding removed.
    void theVoiceRoomBadgeBindsToTheCppProperty() {
        const QString src = readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/VoiceRoom.qml"));
        QVERIFY2(!src.isEmpty(), "VoiceRoom.qml not readable — has it moved?");
        const QString code = scanQml(src).code;
        QVERIFY2(code.contains(QStringLiteral("voiceProtectionBadge")),
                 "VoiceRoom must render ServerConnection.voiceProtectionBadge, "
                 "not a label of its own");
        QVERIFY2(code.contains(QStringLiteral("voiceProtectionDetail")),
                 "the badge's tooltip must render voiceProtectionDetail — the "
                 "badge alone states a guarantee without its limitation");
    }

    // SPEC §3.10: the only honest security surface in Settings is a row in
    // Voice & Activation showing the LIVE call's protection state. Settings has
    // no "Security & Keys" pane and must never grow one — there are no keys to
    // manage, so its content could only be invented, and invented content in a
    // security pane is read as a guarantee.
    //
    // Same comment-stripped check as the badge above, for the same reason: the
    // comment sitting over that row names the property, so scanning the raw
    // file would hold with the binding deleted.
    void theSettingsProtectionRowBindsToTheCppProperty() {
        const QString src = readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ClientSettings.qml"));
        QVERIFY2(!src.isEmpty(), "ClientSettings.qml not readable — has it moved?");
        const QString code = scanQml(src).code;
        QVERIFY2(code.contains(QStringLiteral("voiceProtectionDetail")),
                 "the Voice & Activation protection row must render "
                 "ServerConnection.voiceProtectionDetail verbatim — it is the "
                 "only security statement Settings is allowed to make, and "
                 "writing it here instead of binding it is how it goes stale");
    }

    // No security claim may be written in QML, in any form, ever. The words
    // live in VoiceEncryption.cpp and reach the UI through a Q_PROPERTY —
    // that indirection is the only thing that keeps the label honest when the
    // transport changes underneath it, and it is exactly what was missing
    // when VoiceRoom.qml hard-coded the mesh protocol.
    //
    // Comments are not scanned: they are not shown to anyone, and the ban
    // would make it impossible to explain the rule where it is needed.
    void qmlHoldsNoHardCodedSecurityLabel() {
        const QStringList files = qmlFiles();
        QVERIFY2(files.size() > 10,
                 qPrintable(QStringLiteral("only %1 .qml files found under %2 — the "
                                           "scan is not looking at the real tree")
                                .arg(files.size()).arg(QStringLiteral(BSFCHAT_QML_DIR))));

        // Tokens that can only be a claim about how media is protected. If a
        // new user-facing string genuinely needs one of these, add it to
        // VoiceEncryption.cpp and bind it — do not add it here.
        static const QStringList banned = {
            QStringLiteral("mls"),        QStringLiteral("e2e"),
            QStringLiteral("end-to-end"), QStringLiteral("end to end"),
            QStringLiteral("srtp"),       QStringLiteral("dtls"),
            QStringLiteral("sctp"),       QStringLiteral("encrypt"),
            QStringLiteral("zero-knowledge"),
        };

        int scanned = 0;
        for (const QString& path : files) {
            const QString src = readAll(path);
            QVERIFY2(!src.isEmpty(), qPrintable(path));
            for (const QString& lit : scanQml(src).literals) {
                ++scanned;
                const QString lower = lit.toLower();
                for (const QString& bad : banned) {
                    QVERIFY2(!lower.contains(bad),
                             qPrintable(QStringLiteral("%1 contains the string %2 — "
                                                       "security wording belongs in "
                                                       "voice/VoiceEncryption.cpp, bound "
                                                       "through a Q_PROPERTY")
                                            .arg(path, lit)));
                }
            }
        }
        QVERIFY2(scanned > 100, "string-literal extraction found almost nothing — "
                                "the regex is broken, not the QML");
    }

    // Only a genuine failure interrupts. A deliberately-unencrypted server is
    // labelled, not alarmed about.
    void onlyAFailureIsFatal() {
        QVERIFY(encryptionFailureIsFatal(MediaProtection::Failed));
        QVERIFY(!encryptionFailureIsFatal(MediaProtection::SfuNone));
        QVERIFY(!encryptionFailureIsFatal(MediaProtection::SfuSharedKey));
        QVERIFY(!encryptionFailureIsFatal(MediaProtection::MeshDtls));
    }
};

QTEST_MAIN(TestVoiceEncryption)
#include "test_voice_encryption.moc"
