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
#include <QJsonDocument>

#include "voice/VoiceEncryption.h"

using namespace voice;

namespace {

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
                const int idx = lower.indexOf(QStringLiteral("end-to-end"));
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
