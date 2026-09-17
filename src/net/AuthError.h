#pragma once

#include <QString>

// Recognising "the homeserver has stopped accepting this access token".
//
// Access tokens used to live forever, so every failure from the server was
// worth retrying. They now carry a 90-day expiry and can be revoked (logout
// elsewhere, ban/kick, device removal), which makes a dead token a
// permanent failure that no amount of backoff can fix. Without this
// distinction the sync loop retries the same dead bearer token every 60 s
// for the life of the process while the UI shows "Reconnecting…", so an
// expired session is indistinguishable from bad WiFi and there is nothing
// the user can do about it.
//
// Deliberately separate from SyncBackoff::indicatesRejectedSinceToken, which
// answers a different question ("can dropping the *sync position* help?").
// That one excludes M_UNKNOWN_TOKEN on purpose; this one is its complement.
namespace AuthError {

// True when `errorBody` is a Matrix error object whose errcode says our
// access token is dead: M_UNKNOWN_TOKEN (a token was sent and the server
// does not recognise it) or M_MISSING_TOKEN (we sent none, which for an
// authenticated client means our stored credential was empty).
//
// Matches on the parsed errcode, never a substring: "M_UNKNOWN" is a
// prefix of "M_UNKNOWN_TOKEN" but means something entirely different, and
// a transport failure or an HTML error page from a proxy must never be
// mistaken for the server revoking our session.
bool indicatesDeadAccessToken(const QString& errorBody);

} // namespace AuthError
