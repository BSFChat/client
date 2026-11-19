#pragma once

// User-facing wording for what the voice endpoints answer.
//
// Several voice paths handed the server's raw error body straight to the
// toast, so users saw {"errcode":"M_FORBIDDEN","error":"..."} (audit 3).
// ServerConnection::setVoiceError passes every message through
// readableVoiceError, so no path can reintroduce it. Header-only and pure,
// for tests/test_voice_lifecycle.cpp.

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace voice {

inline QString vtr(const char* text)
{
    return QCoreApplication::translate("Voice", text);
}

// A Matrix error body becomes its `error` sentence (or a sentence for its
// errcode); anything that is not JSON is passed through unchanged.
inline QString readableVoiceError(const QString& raw)
{
    const QString text = raw.trimmed();
    if (!text.startsWith(QLatin1Char('{'))) return text;
    const QJsonObject obj = QJsonDocument::fromJson(text.toUtf8()).object();
    QString error = obj.value(QStringLiteral("error")).toString().trimmed();
    const QString code = obj.value(QStringLiteral("errcode")).toString();
    if (!error.isEmpty()) {
        error[0] = error[0].toUpper();
        const QChar last = error.back();
        if (last != QLatin1Char('.') && last != QLatin1Char('!') && last != QLatin1Char('?'))
            error += QLatin1Char('.');
        return error;
    }
    if (code == QLatin1String("M_FORBIDDEN"))
        return vtr("You don't have permission to do that in this voice channel.");
    if (code == QLatin1String("M_LIMIT_EXCEEDED"))
        return vtr("Too many requests. Try again in a moment.");
    if (code == QLatin1String("M_NOT_FOUND"))
        return vtr("That voice channel no longer exists.");
    if (!code.isEmpty())
        return vtr("The server refused the request (%1).").arg(code);
    return vtr("The server refused the request.");
}

// The participant-cap refusal. "1 person" / "N people", full stop.
inline QString channelFullText(int maxParticipants)
{
    if (maxParticipants <= 0) return vtr("This voice channel is full.");
    if (maxParticipants == 1) return vtr("This voice channel is full (1 person).");
    return vtr("This voice channel is full (%1 people).").arg(maxParticipants);
}

} // namespace voice
