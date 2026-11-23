#pragma once

#include <QLocale>
#include <QString>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <cstdint>

// The client's half of the message size limits. The server publishes what it
// enforces in /versions under `bsfchat.limits`; this reads that block and
// checks an outgoing m.room.message against it. Header-only and free of Qt
// networking so the test can drive it without a server.
namespace bsfchat::net::msglimit {

// One ceiling from the published block. A value that is missing, not an
// unsigned integer, or zero keeps `fallback`: a zero would refuse every
// message, and a server that publishes nonsense should not be obeyed.
inline int readLimit(const nlohmann::json& limits, const char* key, int fallback)
{
    if (!limits.is_object() || !limits.contains(key)) return fallback;
    const auto& v = limits[key];
    // Parsed wire JSON is unsigned; a literal built in code is signed. Both are
    // integers, and anything negative is nonsense.
    std::uint64_t n = 0;
    if (v.is_number_unsigned()) {
        n = v.get<std::uint64_t>();
    } else if (v.is_number_integer()) {
        const auto signed_n = v.get<std::int64_t>();
        if (signed_n <= 0) return fallback;
        n = static_cast<std::uint64_t>(signed_n);
    } else {
        return fallback;
    }
    if (n == 0) return fallback;
    return static_cast<int>(std::min<std::uint64_t>(n, INT_MAX));
}

// Exact bytes, with separators: a rounded "16 KB" would read "16 KB is over
// 16 KB" for a message one byte too long, which is worse than a longer number.
inline QString byteText(qint64 bytes)
{
    return QLocale::system().toString(bytes) + QStringLiteral(" bytes");
}

// Empty when `content` (an m.room.message) fits both ceilings. Otherwise a
// sentence a person can act on: what is too big and what the server takes.
// Bytes, because that is what the server counts (see Constants.h).
inline QString messageSizeProblem(const nlohmann::json& content, qint64 maxBody,
                                  qint64 maxFormatted)
{
    const auto bytesOf = [&content](const char* key) -> qint64 {
        if (!content.is_object() || !content.contains(key)) return 0;
        const auto& v = content[key];
        return v.is_string() ? qint64(v.get_ref<const std::string&>().size()) : 0;
    };

    const qint64 body = bytesOf("body");
    if (body > maxBody) {
        return QStringLiteral("This message is %1; this server accepts up to %2 per message. "
                              "Shorten it, or split it into several messages.")
            .arg(byteText(body), byteText(maxBody));
    }
    const qint64 formatted = bytesOf("formatted_body");
    if (formatted > maxFormatted) {
        return QStringLiteral("The formatting makes this message %1; this server accepts up to %2 "
                              "once formatted. Use less markup, or split it into several messages.")
            .arg(byteText(formatted), byteText(maxFormatted));
    }
    return {};
}

// What the send path does with one outgoing m.room.message.
enum class SendPlan {
    Inline,                 // fits as sent
    InlineWithoutFormatting,// the body fits; only the markup is too big, so send the text and drop the markup
    AsFile,                 // too long for a message: upload the text as a file and send that
    Refuse,                 // cannot be sent at all; `reason` says why
};

struct SendDecision {
    SendPlan plan = SendPlan::Inline;
    QString reason; // set for Refuse only
};

// The decision for one message. Order matters: the cheapest answer first, and
// a file only when the text itself is too long for a message.
//
// Files are for m.text only. An /me emote or a notice cannot become an m.file
// without changing what it is, so those are refused with the size reason.
// A reply or thread message is still m.text, so it keeps its m.relates_to on
// the file event and stays in its thread.
inline SendDecision planSend(const nlohmann::json& content, qint64 maxBody,
                             qint64 maxFormatted, qint64 maxUpload)
{
    const auto bytesOf = [&content](const char* key) -> qint64 {
        if (!content.is_object() || !content.contains(key)) return 0;
        const auto& v = content[key];
        return v.is_string() ? qint64(v.get_ref<const std::string&>().size()) : 0;
    };
    const qint64 body = bytesOf("body");
    const qint64 formatted = bytesOf("formatted_body");

    if (body <= maxBody && formatted <= maxFormatted) return {SendPlan::Inline, {}};
    if (body <= maxBody) return {SendPlan::InlineWithoutFormatting, {}};

    const std::string msgtype = content.is_object() && content.contains("msgtype")
                                    && content["msgtype"].is_string()
        ? content["msgtype"].get<std::string>()
        : std::string();
    if (msgtype != "m.text") {
        return {SendPlan::Refuse, messageSizeProblem(content, maxBody, maxFormatted)};
    }
    if (body > maxUpload) {
        return {SendPlan::Refuse,
                QStringLiteral("This message is %1, which is more than the %2 this server accepts "
                               "as a file. Shorten it, or split it into several messages.")
                    .arg(byteText(body), byteText(maxUpload))};
    }
    return {SendPlan::AsFile, {}};
}

} // namespace bsfchat::net::msglimit
