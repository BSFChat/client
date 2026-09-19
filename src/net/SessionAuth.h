#pragma once

// Where a connection stands with the homeserver's opinion of our access
// token, and — the part that matters — what a user-initiated "reconnect"
// is allowed to do about it.
//
// This exists because of a production incident (2026-09-19): every row in
// the server's `access_tokens` table was deleted as a one-off purge after a
// token-entropy fix. Every client correctly got 401 M_UNKNOWN_TOKEN and
// correctly showed "Your session has expired. Sign in again to reconnect."
// Then nothing the user could do from the UI ever reached POST /login — the
// server logged ZERO login attempts while they retried — and the only
// recovery was to remove the server from the sidebar and add it again.
//
// Three separate defects produced that, and all three are decisions about
// state rather than about networking, so they live here where a test can
// pin them without a server, an event loop or a window:
//
//   1. The only "try again" affordance in the UI (server context menu >
//      Reconnect) rebuilt the connection carrying the SAME dead bearer
//      token over to the new object and went straight back to /sync. A
//      token the server has already rejected can never be retried into
//      working, so once a rejection has been seen, a plain retry is the
//      wrong action and must be refused in favour of a fresh login.
//      That is actionForReconnect().
//
//   2. The expired state latched. The sync-error handler early-returned on
//      "already surfaced", and the only thing that cleared it was a
//      successful /sync — which cannot happen with a dead token. Restarting
//      the app did not help either, because the dead token is what gets
//      restored from settings. So the error surface was unclearable for the
//      life of the install. Every transition here is therefore reversible:
//      beginReauth() works from Expired, noteReauthFailed() returns to
//      Expired rather than to a terminal state, and a rejection after a
//      recovery surfaces again instead of being swallowed as a duplicate.
//
//   3. Every other subsystem kept driving the dead token and reporting the
//      401 on its own surface — what the user actually saw first was a raw
//      {"errcode":"M_UNKNOWN_TOKEN",...} toast from the voice join path,
//      not an auth prompt. Once the session is known dead those are noise
//      about a cause already on screen: shouldSuppressSubsystemError().
//
// Deliberately free of Qt beyond nothing at all: no QObject, no signals, no
// strings. ServerConnection owns one of these and does the I/O.

namespace bsfchat::client {

enum class SessionPhase {
    // We hold a token that the homeserver has not rejected. Not a claim
    // that it is good — only that nothing has said otherwise yet.
    Authenticated,
    // The homeserver has rejected it (401 M_UNKNOWN_TOKEN / M_MISSING_TOKEN).
    // Permanent for this credential; only a fresh login changes it.
    Expired,
    // A login round trip is in flight. Distinct from Expired so a second
    // click on "Sign in again" does not open a second browser tab.
    Reauthenticating,
};

// What a "reconnect / try again" request should actually do.
enum class ReconnectAction {
    // Ordinary kick: tear down and redial carrying the current credential.
    RetryWithToken,
    // The credential is known dead. Discard it and run the login flow.
    Reauthenticate,
    // A login is already in flight; do nothing and let it finish.
    Ignore,
};

class SessionAuth {
public:
    SessionPhase phase() const { return m_phase; }

    // True whenever the user needs to sign in again — including while the
    // attempt is running, so the UI does not flicker back to "connected"
    // between clicking the button and the browser flow completing.
    bool needsReauth() const { return m_phase != SessionPhase::Authenticated; }
    bool isReauthenticating() const { return m_phase == SessionPhase::Reauthenticating; }

    // The homeserver rejected our token. Returns true only on the edge into
    // Expired, which is the caller's cue to stop sync and raise the banner;
    // the tenth 401 from ten different in-flight requests returns false.
    //
    // Note what it does NOT do: it never refuses to move a Reauthenticating
    // session back to Expired. A 401 arriving from a request that was still
    // in flight when the user hit the button would otherwise strand the
    // phase in Reauthenticating with no login running.
    bool noteTokenRejected()
    {
        const bool isNew = (m_phase == SessionPhase::Authenticated);
        m_phase = SessionPhase::Expired;
        return isNew;
    }

    ReconnectAction actionForReconnect() const
    {
        switch (m_phase) {
        case SessionPhase::Authenticated:    return ReconnectAction::RetryWithToken;
        case SessionPhase::Expired:          return ReconnectAction::Reauthenticate;
        case SessionPhase::Reauthenticating: return ReconnectAction::Ignore;
        }
        return ReconnectAction::RetryWithToken;
    }

    // Take the session into a login attempt. False means "already running" —
    // the caller must not start a second flow.
    //
    // Callable from Authenticated on purpose: "sign out and sign in again"
    // is a legitimate thing to offer someone whose session is merely
    // misbehaving, and refusing it here would be a second way to get stuck.
    bool beginReauth()
    {
        if (m_phase == SessionPhase::Reauthenticating) return false;
        m_phase = SessionPhase::Reauthenticating;
        return true;
    }

    // The login attempt failed (user closed the browser, IdP down, server
    // advertises no flow we can run). Back to Expired — NOT to a terminal
    // state, so the button works again. This is defect 2 above.
    void noteReauthFailed() { m_phase = SessionPhase::Expired; }

    // A login succeeded, or a /sync came back clean. Resets the "already
    // surfaced" memory too, so a later purge is reported rather than
    // silently swallowed as a repeat of the one we recovered from.
    void noteAuthenticated() { m_phase = SessionPhase::Authenticated; }

    // Whether a subsystem that just got a 401 should keep quiet about it.
    // True from the moment the session is known dead: the banner is already
    // saying the true thing, and a raw Matrix error object in a toast is
    // worse than silence. This is defect 3 above.
    bool shouldSuppressSubsystemError() const { return needsReauth(); }

private:
    SessionPhase m_phase = SessionPhase::Authenticated;
};

} // namespace bsfchat::client
