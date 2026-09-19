// The recovery rules for a session the homeserver has stopped accepting.
//
// Regression suite for the 2026-09-19 production incident: every row in the
// server's `access_tokens` table was purged after a token-entropy fix, every
// desktop client showed "Your session has expired. Sign in again to
// reconnect." — and then no user action ever reached POST /login. The server
// logged zero login attempts while the user retried. The client was stuck
// until the server was removed from the sidebar and added again.
//
// Each slot below fails against the behaviour that shipped. Read
// src/net/SessionAuth.h for the three defects; the names here map onto them.

#include "net/SessionAuth.h"

#include <QTest>

using bsfchat::client::ReconnectAction;
using bsfchat::client::SessionAuth;
using bsfchat::client::SessionPhase;

class TestSessionAuth : public QObject
{
    Q_OBJECT

private slots:
    // --- baseline -----------------------------------------------------

    void freshSessionIsAuthenticatedAndRetriesNormally()
    {
        SessionAuth a;
        QCOMPARE(a.phase(), SessionPhase::Authenticated);
        QVERIFY(!a.needsReauth());
        QVERIFY(!a.isReauthenticating());
        // "Reconnect" on a healthy-but-sulking connection is still the
        // ordinary redial. Nothing here may break that path.
        QCOMPARE(a.actionForReconnect(), ReconnectAction::RetryWithToken);
        QVERIFY(!a.shouldSuppressSubsystemError());
    }

    // --- defect 1: a dead token must never be retried -----------------

    // The whole incident in one assertion. ServerManager::reconnectServer
    // called rebuildConnection, which copied old->accessToken() onto the
    // replacement connection and went straight back to /sync. The user
    // clicked Reconnect, the client redialled with the same dead bearer
    // token, and /login was never touched — which is exactly what the
    // server's logs showed.
    void rejectedTokenMustNotBeRetried()
    {
        SessionAuth a;
        a.noteTokenRejected();
        QCOMPARE(a.actionForReconnect(), ReconnectAction::Reauthenticate);
        QVERIFY(a.actionForReconnect() != ReconnectAction::RetryWithToken);
    }

    void firstRejectionIsTheOneToSurface()
    {
        SessionAuth a;
        QVERIFY(a.noteTokenRejected());
        // Ten in-flight requests all 401 at once. One banner, not ten.
        QVERIFY(!a.noteTokenRejected());
        QVERIFY(!a.noteTokenRejected());
        QCOMPARE(a.phase(), SessionPhase::Expired);
    }

    // --- defect 2: the state must be clearable ------------------------

    // The shipped handler early-returned on `m_connectionStatus == 3` and the
    // only thing that reset it was a successful /sync, which a dead token can
    // never produce. Nothing the user could do cleared the surface.
    void expiredSessionCanAlwaysStartAReauth()
    {
        SessionAuth a;
        a.noteTokenRejected();
        QVERIFY(a.beginReauth());
        QCOMPARE(a.phase(), SessionPhase::Reauthenticating);
        QVERIFY(a.isReauthenticating());
        QVERIFY(a.needsReauth());
    }

    void secondClickWhileSigningInIsIgnored()
    {
        SessionAuth a;
        a.noteTokenRejected();
        QVERIFY(a.beginReauth());
        // An impatient double-click must not open a second browser tab and
        // a second PKCE exchange.
        QVERIFY(!a.beginReauth());
        QCOMPARE(a.actionForReconnect(), ReconnectAction::Ignore);
    }

    // The failure mode that would have replaced the old one: a login attempt
    // that dies (user closes the browser tab, IdP unreachable, server
    // advertises no flow we can run) must leave the button usable.
    void failedReauthIsRetryable()
    {
        SessionAuth a;
        a.noteTokenRejected();
        QVERIFY(a.beginReauth());
        a.noteReauthFailed();
        QCOMPARE(a.phase(), SessionPhase::Expired);
        QCOMPARE(a.actionForReconnect(), ReconnectAction::Reauthenticate);
        QVERIFY(a.beginReauth());   // and again, as many times as it takes
        a.noteReauthFailed();
        QVERIFY(a.beginReauth());
    }

    void successfulReauthFullyRecovers()
    {
        SessionAuth a;
        a.noteTokenRejected();
        a.beginReauth();
        a.noteAuthenticated();
        QCOMPARE(a.phase(), SessionPhase::Authenticated);
        QVERIFY(!a.needsReauth());
        QCOMPARE(a.actionForReconnect(), ReconnectAction::RetryWithToken);
        QVERIFY(!a.shouldSuppressSubsystemError());
    }

    // A second purge, months later, must raise the banner again. Under the
    // old "already surfaced" guard the flag was sticky for the process.
    void aLaterPurgeSurfacesAgain()
    {
        SessionAuth a;
        QVERIFY(a.noteTokenRejected());
        a.beginReauth();
        a.noteAuthenticated();
        QVERIFY(a.noteTokenRejected());
    }

    // A 401 from a request that was already in flight when the user pressed
    // the button must not strand the phase in Reauthenticating with no login
    // running — that would leave beginReauth() refusing forever.
    void lateRejectionDuringReauthDoesNotStrandTheState()
    {
        SessionAuth a;
        a.noteTokenRejected();
        QVERIFY(a.beginReauth());
        a.noteTokenRejected();          // stale 401 lands
        QCOMPARE(a.phase(), SessionPhase::Expired);
        QVERIFY(a.beginReauth());       // still recoverable
    }

    // --- defect 3: one surface for the cause --------------------------

    // What the user actually saw first was not the auth banner but a raw
    // {"errcode":"M_UNKNOWN_TOKEN",...} toast thrown by the voice join path,
    // because every subsystem kept driving the token the client already knew
    // was dead and reported the 401 on its own surface.
    void subsystemErrorsAreSuppressedOnceTheSessionIsKnownDead()
    {
        SessionAuth a;
        QVERIFY(!a.shouldSuppressSubsystemError());
        a.noteTokenRejected();
        QVERIFY(a.shouldSuppressSubsystemError());
        a.beginReauth();
        QVERIFY(a.shouldSuppressSubsystemError());
        a.noteAuthenticated();
        QVERIFY(!a.shouldSuppressSubsystemError());
    }

    // --- the whole incident, end to end -------------------------------

    void purgeToRecoveryRoundTrip()
    {
        SessionAuth a;

        // Server-side purge. Sync 401s.
        QVERIFY(a.noteTokenRejected());
        QVERIFY(a.needsReauth());

        // The user tries the only affordance the shipped client had.
        QCOMPARE(a.actionForReconnect(), ReconnectAction::Reauthenticate);

        // They click "Sign in again". The browser flow is cancelled.
        QVERIFY(a.beginReauth());
        a.noteReauthFailed();
        QVERIFY(a.needsReauth());

        // They click it again and complete it this time.
        QVERIFY(a.beginReauth());
        a.noteAuthenticated();

        QCOMPARE(a.phase(), SessionPhase::Authenticated);
        QVERIFY(!a.needsReauth());
        QCOMPARE(a.actionForReconnect(), ReconnectAction::RetryWithToken);
    }
};

QTEST_MAIN(TestSessionAuth)
#include "test_session_auth.moc"
