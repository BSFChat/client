#pragma once

#include <QString>
#include <QUrl>
#include <QList>

// "Is this the same server we are already connected to?" — the one rule,
// kept as free functions with no Qt object graph so it can be tested
// without a Settings, a ServerConnection or a network stack. Same shape as
// net/SyncBackoff.h and net/SessionAuth.h, and for the same reason.
//
// THE BUG THIS EXISTS FOR
//
// ServerManager had four places that did `new ServerConnection(url, this)`
// and only one of them — the identity-driven server list — checked whether
// a connection for that URL was already in the roster. Every other route in
// appended unconditionally:
//
//   * "Add server" with BSFChat ID, which is what a user reaches for when
//     their session has gone stale ("log out and add it again" was the
//     documented workaround for exactly that);
//   * "Add server" with a password;
//   * register.
//
// Each duplicate is a whole parallel stack: its own ServerConnection, its
// own MatrixClient, its own SyncLoop, its own 30-second long poll, its own
// device on the server. They are the same account on the same homeserver,
// so they answer the same events into two sets of models, and they hold two
// sockets — and a parked /sync holds an httplib worker for the life of its
// socket, so duplicates eat the server's connection pool at twice the rate
// and push sends into the queue behind them.
//
// Worse, it persists. ServerManager::persistCredentials() writes one saved
// row per roster entry, so the duplicate is written to settings and every
// subsequent launch restores BOTH rows and starts BOTH loops before the
// user has touched anything. Recovered from a real client's settings:
//
//     servers.1.url    = https://chat.bsfchat.com
//     servers.1.userId = @@oidc_a5cdbefe-…:chat.bsfchat.com
//     servers.2.url    = https://chat.bsfchat.com      <-- same account
//     servers.2.userId = @@oidc_a5cdbefe-…:chat.bsfchat.com
//     servers.size     = 2
//
// and its log showed two /sync chains starting 154ms apart at launch,
// polling the same token for the rest of the session.
namespace bsfchat::client {

// What identifies "the same account on the same homeserver". `userId` is
// empty for a connection that has not logged in yet.
struct ServerIdentity {
    QString url;
    QString userId;
};

// Comparison form of a homeserver URL. Scheme and host are case-insensitive
// per RFC 3986 and a trailing slash is not a difference, but the strings
// reaching the roster come from three sources that disagree about all
// three: what the user typed, what .well-known returned, and what the
// identity service has stored. Comparing them raw is why the one dedup
// check that did exist could still miss.
//
// Deliberately NOT a normaliser for requests — bsfchat::normaliseServerUrl
// in net/ServerDiscovery.h is that, and this must never disagree with it
// about which URLs are the same server. This only ever produces a key.
inline QString serverComparisonKey(const QString& url)
{
    QString trimmed = url.trimmed();
    while (trimmed.endsWith(QLatin1Char('/'))) trimmed.chop(1);
    if (trimmed.isEmpty()) return {};

    QUrl parsed(trimmed.contains(QStringLiteral("://"))
                    ? trimmed
                    : QStringLiteral("https://") + trimmed);
    if (!parsed.isValid() || parsed.host().isEmpty()) return trimmed.toLower();

    QString scheme = parsed.scheme().isEmpty() ? QStringLiteral("https")
                                               : parsed.scheme().toLower();
    QString key = scheme + QStringLiteral("://") + parsed.host().toLower();
    // An explicit default port is the same server as no port at all.
    const int port = parsed.port();
    const int defaultPort = (scheme == QStringLiteral("https")) ? 443 : 80;
    if (port != -1 && port != defaultPort) key += QStringLiteral(":") + QString::number(port);
    // The path is part of the homeserver's address (a server can live under
    // a prefix), so it stays — minus the trailing slash already stripped.
    QString path = parsed.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    key += path;
    return key;
}

// True when `a` and `b` would be two connections to one place.
//
// An empty userId on EITHER side means "not logged in yet, so we only know
// the address". That deliberately matches: the add-server flows all start
// with no userId, and treating an unauthenticated add to a homeserver the
// user is already signed into as a distinct server is precisely how the
// duplicates got made. Two genuinely different accounts on one homeserver
// still compare as different once both have logged in — but they can no
// longer be CREATED by the add path, which now re-authenticates the
// existing connection instead. That is the right trade: a second account on
// one server is rare and has a deliberate route, whereas re-adding a server
// you are already on is the common accident.
inline bool isSameServerAccount(const ServerIdentity& a, const ServerIdentity& b)
{
    const QString ka = serverComparisonKey(a.url);
    const QString kb = serverComparisonKey(b.url);
    if (ka.isEmpty() || kb.isEmpty()) return false;
    if (ka != kb) return false;
    if (a.userId.isEmpty() || b.userId.isEmpty()) return true;
    return a.userId == b.userId;
}

// Index of the first entry in `existing` that `candidate` would duplicate,
// or -1. Linear on purpose: a roster is a handful of servers.
inline int indexOfExistingServer(const QList<ServerIdentity>& existing,
                                 const ServerIdentity& candidate)
{
    for (int i = 0; i < existing.size(); ++i) {
        if (isSameServerAccount(existing[i], candidate)) return i;
    }
    return -1;
}

// Indices of `saved` that are duplicates of an EARLIER entry, ascending.
//
// Used to repair settings written before the add paths were deduplicated.
// The first occurrence always wins, which is what keeps activeServerIndex
// meaningful: it points into this list, the user's active server is
// overwhelmingly index 0, and renumbering around a later survivor would
// move the app to a different server on launch for no reason the user
// asked for.
inline QList<int> duplicateServerRows(const QList<ServerIdentity>& saved)
{
    QList<int> dupes;
    QList<ServerIdentity> kept;
    for (int i = 0; i < saved.size(); ++i) {
        if (indexOfExistingServer(kept, saved[i]) >= 0) {
            dupes.append(i);
            continue;
        }
        kept.append(saved[i]);
    }
    return dupes;
}

} // namespace bsfchat::client
