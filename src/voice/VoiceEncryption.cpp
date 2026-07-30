#include "voice/VoiceEncryption.h"

#include <QJsonValue>

namespace voice {

QString protectionBadge(MediaProtection p) {
    switch (p) {
    case MediaProtection::MeshDtls:
        // Unchanged from today, and deliberately so. Accurate: Opus
        // frames ride an SCTP data channel over libdatachannel's DTLS
        // 1.2 handshake, peer to peer. There is NO SRTP on this path —
        // see PeerConnectionManager.cpp:367,559. (server/docs/
        // livekit-migration.md called it "DTLS-SRTP"; that is wrong.)
        return QStringLiteral("DTLS · SCTP");
    case MediaProtection::SfuSharedKey:
        // "shared key" is doing real work here. It signals that one key
        // is shared by the channel, which is exactly the property that
        // makes this not end-to-end. Do not shorten it to "Encrypted".
        return QStringLiteral("Encrypted · shared key");
    case MediaProtection::SfuNone:
        return QStringLiteral("Not encrypted");
    case MediaProtection::Failed:
        return QStringLiteral("Encryption failed");
    }
    return QStringLiteral("Unknown");
}

QString protectionDetail(MediaProtection p) {
    switch (p) {
    case MediaProtection::MeshDtls:
        return QStringLiteral(
            "Audio and video go directly between participants over a "
            "DTLS-encrypted connection. No relay server is involved.\n\n"
            "This is not end-to-end encrypted messaging: there is no "
            "identity verification, and your server coordinates the call.");
    case MediaProtection::SfuSharedKey:
        // Guarantee and limitations in one breath, deliberately. A user
        // who reads only the first sentence should not come away with a
        // stronger belief than the truth.
        return QStringLiteral(
            "Audio and video are encrypted with a key from your BSFChat "
            "server, so the voice relay cannot read them.\n\n"
            "This is not end-to-end encryption. Your server creates and "
            "holds the key, so it can decrypt this call. Anyone who has "
            "been in this channel can also still decrypt it until an "
            "admin rotates the key — including people who have left.");
    case MediaProtection::SfuNone:
        return QStringLiteral(
            "Audio and video pass through the voice relay unencrypted, so "
            "the relay and your server can read them. The connection to "
            "the relay is still protected by TLS in transit.\n\n"
            "An admin can turn on media encryption in the server's voice "
            "settings.");
    case MediaProtection::Failed:
        return QStringLiteral(
            "This call could not be encrypted, so it was not connected.\n\n"
            "Connecting anyway would have sent your audio and video in a "
            "form the relay could read, while this still showed as "
            "encrypted. Ask an admin to check the server's voice "
            "encryption settings.");
    }
    return QString();
}

bool encryptionFailureIsFatal(MediaProtection p) {
    // Only the Failed state. SfuNone is a deliberate server configuration
    // that is labelled honestly, not a failure — the difference matters,
    // because treating "admin chose not to encrypt" as an error would
    // train people to click through the warning that DOES matter.
    return p == MediaProtection::Failed;
}

LiveKitJoinParse parseLiveKitJoin(const QJsonObject& response) {
    LiveKitJoinParse out;
    LiveKitJoinConfig& cfg = out.config;

    cfg.url = response.value(QStringLiteral("url")).toString();
    cfg.token = response.value(QStringLiteral("token")).toString();
    cfg.room = response.value(QStringLiteral("room")).toString();
    cfg.identity = response.value(QStringLiteral("identity")).toString();
    cfg.ttlSeconds = response.value(QStringLiteral("ttl")).toInt();

    if (!cfg.isValid()) {
        out.error = QStringLiteral(
            "The server's voice response was incomplete. Voice can't start.");
        return out;
    }

    const QJsonValue enc = response.value(QStringLiteral("encryption"));
    if (enc.isUndefined() || enc.isNull()) {
        // No encryption block: the server has media encryption switched
        // off. That is a valid configuration and is reported honestly as
        // SfuNone; it is NOT an error.
        return out;
    }
    if (!enc.isObject()) {
        out.error = QStringLiteral(
            "The server sent a malformed encryption setting. Voice can't "
            "start, because it can't be encrypted as expected.");
        return out;
    }

    const QJsonObject e = enc.toObject();
    const QString mode = e.value(QStringLiteral("mode")).toString();
    if (mode != QLatin1String(kEncryptionModeSharedKey)) {
        // Refuse rather than fall back. A server offering a scheme this
        // build does not implement must not be silently downgraded to
        // one it does.
        out.error = QStringLiteral(
            "This server uses a voice encryption mode this version of "
            "BSFChat doesn't support. Update the app to join.");
        return out;
    }

    const QString b64 = e.value(QStringLiteral("key")).toString();
    if (b64.isEmpty()) {
        out.error = QStringLiteral(
            "The server offered an encrypted call but sent no key. Voice "
            "can't start.");
        return out;
    }

    // AbortOnBase64DecodingErrors is REQUIRED, not decoration.
    // fromBase64Encoding's default options are lenient exactly like
    // fromBase64: junk inside the payload is silently skipped, so a
    // corrupted key can decode to a full 32 bytes and sail past the
    // length check below. The result would be a call encrypted with a
    // key the server never issued — every participant mutually
    // undecodable, with the UI still reporting "encrypted".
    // (This was a real bug here, caught by
    // base64JunkThatStillDecodesToTheRightLengthIsRejected.)
    const auto decoded = QByteArray::fromBase64Encoding(
        b64.toUtf8(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        out.error = QStringLiteral(
            "The server's voice encryption key was malformed. Voice can't "
            "start.");
        return out;
    }
    if (decoded.decoded.size() != kSharedKeyBytes) {
        // Wrong length means we would either fail to decrypt or, worse,
        // hand the SDK something it pads or truncates.
        out.error = QStringLiteral(
            "The server's voice encryption key was the wrong size. Voice "
            "can't start.");
        return out;
    }

    cfg.key = decoded.decoded;
    cfg.keyGeneration = static_cast<qint64>(
        e.value(QStringLiteral("key_generation")).toDouble(0));
    return out;
}

} // namespace voice
