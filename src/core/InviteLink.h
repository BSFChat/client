#pragma once

// bsfchat://invite links: "join this server". Format:
//
//   bsfchat://invite/<server>
//   bsfchat://invite?server=<server>
//
// <server> is what a person would type into Add server: a host name with an
// optional port ("chat.example.com", "chat.example.com:8448"), or an https://
// URL, percent-encoded or not. Nothing else is accepted, because the link can
// come from anywhere: no other scheme (http included), no user info, and only
// a plain DNS name or IP literal as the host.
//
// The link only ever PREFILLS the Add server address; joining still takes the
// user's own tap, and the probe goes out only then.
//
// Header-only so tests/test_invite_link.cpp needs nothing but QtCore.

#include <QString>
#include <QUrl>
#include <QUrlQuery>

namespace bsfchat::invite {

inline bool isInviteLink(const QString& link)
{
    return link.startsWith(QLatin1String("bsfchat://invite/"), Qt::CaseInsensitive)
        || link.startsWith(QLatin1String("bsfchat://invite?"), Qt::CaseInsensitive);
}

// The server address from an invite link, ready to prefill the Add server
// field, or "" when the link is not an invite or names no acceptable server.
// Without a path the result is host[:port]; with one, the full https URL.
inline QString serverFromLink(const QString& link)
{
    if (!isInviteLink(link)) return {};
    QString raw;
    const QString rest = link.mid(int(sizeof("bsfchat://invite") - 1));
    if (rest.startsWith(QLatin1Char('?'))) {
        raw = QUrlQuery(rest.mid(1)).queryItemValue(QStringLiteral("server"),
                                                    QUrl::FullyDecoded);
    } else {
        raw = rest.mid(1);
        // Ignore a trailing query or fragment on the path form.
        for (const QChar stop : {QLatin1Char('?'), QLatin1Char('#')}) {
            const int cut = raw.indexOf(stop);
            if (cut >= 0) raw.truncate(cut);
        }
        raw = QUrl::fromPercentEncoding(raw.toUtf8());
    }
    raw = raw.trimmed();
    if (raw.isEmpty() || raw.size() > 512) return {};
    for (const QChar c : raw)
        if (c.isSpace() || c.unicode() < 0x20) return {};

    const bool hasScheme = raw.contains(QLatin1String("://"));
    if (hasScheme && !raw.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        return {};
    const QUrl url(hasScheme ? raw : QStringLiteral("https://") + raw, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) return {};
    if (!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return {};

    const QString host = url.host(QUrl::FullyEncoded);
    if (host.isEmpty() || host.size() > 253) return {};
    // A DNS name (IDNs arrive here as punycode) or an IP literal; QUrl has
    // already validated IPv6 inside brackets.
    if (!host.contains(QLatin1Char(':'))) {
        for (const QChar c : host) {
            if (!(c.isLetterOrNumber() && c.unicode() < 0x80)
                && c != QLatin1Char('.') && c != QLatin1Char('-'))
                return {};
        }
        if (host.startsWith(QLatin1Char('.')) || host.startsWith(QLatin1Char('-')))
            return {};
    }

    const QString path = url.path(QUrl::FullyEncoded);
    if (!path.isEmpty() && path != QLatin1String("/"))
        return url.toString(QUrl::FullyEncoded);
    QString out = host.contains(QLatin1Char(':'))
                      ? QLatin1Char('[') + host + QLatin1Char(']') : host;
    if (url.port() > 0) out += QLatin1Char(':') + QString::number(url.port());
    return out;
}

} // namespace bsfchat::invite
