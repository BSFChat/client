#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

// The account-deletion handshake, as a state machine with no network in it.
//
// POST /_matrix/client/v3/account/deactivate is Matrix user-interactive auth:
//
//   1. POST with no `auth`.
//   2. The server answers 401 with { flows: [{ stages: ["m.login.password"] }],
//      params, completed, session } — the SAME shape /account/password emits.
//      The client collects a password and repeats the request with
//      { auth: { type: "m.login.password", password: "…" } }.
//   3. 200. The account is gone.
//
// ── Step 1 can delete the account on its own ─────────────────────────────
//
// An account signed in through the identity provider has NO password on this
// server — the row's hash is empty — and the server admits it on the bearer
// token alone (server/src/api/AuthHandler.cpp, handle_deactivate_account). For
// those accounts step 1 answers 200 and there is no step 2.
//
// So the first request is not a probe and must never be treated as one. The
// user's confirmation has to be complete BEFORE begin() is called, and the
// password prompt is a SECOND gate that only some accounts see — not the
// confirmation. A UI that posts first to find out whether it needs a password
// has already deleted half its users' accounts.
//
// ── Telling the two 401s apart ───────────────────────────────────────────
//
// A 401 here means one of two opposite things: "prove it is you" (the UIA
// challenge) or "your session is dead" (M_UNKNOWN_TOKEN, which MatrixClient
// watches for globally and turns into the sign-in banner). They are told apart
// by the `flows` array, not by the errcode — the challenge carries no errcode
// at all. Getting this backwards either eats the password prompt or claims the
// user has been signed out while they are staring at a working client.
//
// And a wrong password is 403 M_FORBIDDEN, not another 401, so it must leave
// the flow in PasswordRequired with the field still on screen rather than
// unwinding it: the server counts that attempt against the same lockout
// /login uses, and a user who has to reopen the dialog to retype a typo will
// spend that budget fast.
//
// Free of Qt beyond QJson, of MatrixClient and of QObject, so
// tests/test_deactivate_flow.cpp can drive every branch without a socket.
namespace bsfchat::net {

enum class DeactivatePhase {
    // Nothing in flight. Also where a failed attempt lands, so the dialog's
    // button works again.
    Idle,
    // The first request is out. May come back 200 (an account with no
    // password) or 401-with-flows (one with).
    Confirming,
    // The server asked for the password and nothing is in flight. The UI owns
    // the field; this phase is what says to show it.
    PasswordRequired,
    // A request carrying the password is out.
    Authenticating,
    // 200. Terminal — the account no longer exists, so there is nothing to
    // retry and no way back.
    Deactivated,
};

// What the caller should do with a reply.
enum class DeactivateOutcome {
    // 200: the account is gone. Tear the connection down.
    Succeeded,
    // 401 with an m.login.password flow. Show the password field.
    PasswordRequired,
    // 403 after we sent a password: wrong password. Say so, keep the field.
    PasswordRejected,
    // Anything else — including a 401 with no usable flow, which is a server
    // asking for a stage this client cannot complete. Surface the message.
    Failed,
    // A reply that does not belong to the request we are waiting for (a late
    // answer to a cancelled attempt). Ignore it.
    Ignored,
};

class DeactivateFlow {
public:
    DeactivatePhase phase() const { return m_phase; }

    // True while a request is out — the dialog disables its buttons on this
    // so a double click cannot post twice.
    bool busy() const
    {
        return m_phase == DeactivatePhase::Confirming
            || m_phase == DeactivatePhase::Authenticating;
    }

    // True when the UI must show a password field. Distinct from busy(): the
    // field is on screen both while idle-awaiting-input and while the password
    // is being checked, and it must not vanish between the two.
    bool needsPassword() const
    {
        return m_phase == DeactivatePhase::PasswordRequired
            || m_phase == DeactivatePhase::Authenticating;
    }

    // The server's UIA session token, echoed back with the password so the
    // server can match the two halves. Empty until a challenge has arrived.
    QString session() const { return m_session; }

    // Last refusal, for the inline error line. Cleared by every new attempt.
    QString errorText() const { return m_errorText; }

    // Send the first request. False when one is already running, or when the
    // account is already deactivated — either way the caller must not POST.
    //
    // THE CALLER HAS ALREADY CONFIRMED by the time this runs. See the header.
    bool begin()
    {
        if (m_phase != DeactivatePhase::Idle) return false;
        m_phase = DeactivatePhase::Confirming;
        m_errorText.clear();
        return true;
    }

    // Send the password. False unless the server has actually asked for one
    // and nothing is in flight.
    bool submitPassword()
    {
        if (m_phase != DeactivatePhase::PasswordRequired) return false;
        m_phase = DeactivatePhase::Authenticating;
        m_errorText.clear();
        return true;
    }

    // The `auth` object to send with the second request. Built here so the
    // session token cannot be forgotten at a call site.
    //
    // No `identifier`: the server takes the account from the bearer token and
    // refuses a named identifier that disagrees with it, so sending one adds a
    // way to get a 403 and no way to succeed.
    QJsonObject authObject(const QString& password) const
    {
        QJsonObject auth;
        auth.insert(QStringLiteral("type"), QStringLiteral("m.login.password"));
        auth.insert(QStringLiteral("password"), password);
        if (!m_session.isEmpty()) auth.insert(QStringLiteral("session"), m_session);
        return auth;
    }

    // Fold a reply in and say what the caller should do about it.
    //
    // `body` is the parsed response (an empty object when it did not parse —
    // an HTML error page from a proxy must not read as a UIA challenge).
    DeactivateOutcome onReply(int status, const QJsonObject& body)
    {
        if (!busy()) return DeactivateOutcome::Ignored;
        const bool sentPassword = (m_phase == DeactivatePhase::Authenticating);

        if (status >= 200 && status < 300) {
            m_phase = DeactivatePhase::Deactivated;
            m_errorText.clear();
            return DeactivateOutcome::Succeeded;
        }

        if (status == 401 && offersPasswordStage(body)) {
            m_session = body.value(QStringLiteral("session")).toString();
            m_phase = DeactivatePhase::PasswordRequired;
            // Not an error: this is the server doing its job. A sentence here
            // would put "something went wrong" above a field the user has not
            // filled in yet.
            m_errorText.clear();
            return DeactivateOutcome::PasswordRequired;
        }

        if (status == 403 && sentPassword) {
            // Back to PasswordRequired, NOT to Idle: the field stays, with the
            // reason above it. The server has already counted this against the
            // account's lockout, so making the user reopen the dialog to try
            // again would spend that budget on the UI's behalf.
            m_phase = DeactivatePhase::PasswordRequired;
            m_errorText = messageIn(body, QStringLiteral("That password is not correct."));
            return DeactivateOutcome::PasswordRejected;
        }

        m_phase = DeactivatePhase::Idle;
        m_errorText = messageIn(body, QStringLiteral("The server refused the request."));
        return DeactivateOutcome::Failed;
    }

    // A transport failure — no status, no body. Same landing as any other
    // failure, with the network's own words.
    void onTransportFailure(const QString& error)
    {
        if (!busy()) return;
        m_phase = (m_phase == DeactivatePhase::Authenticating)
                      ? DeactivatePhase::PasswordRequired
                      : DeactivatePhase::Idle;
        m_errorText = error.isEmpty()
                          ? QStringLiteral("Could not reach the server.")
                          : error;
    }

    // The user closed the dialog, or the connection went away. Everything
    // except Deactivated is abandonable; that one is terminal because the
    // account really is gone and there is nothing to return to.
    void reset()
    {
        if (m_phase == DeactivatePhase::Deactivated) return;
        m_phase = DeactivatePhase::Idle;
        m_session.clear();
        m_errorText.clear();
    }

private:
    // A challenge is usable only if some flow's stage list contains
    // m.login.password. A server offering only stages this client cannot
    // complete (a captcha, an email) must not be answered with a password
    // field that can never succeed.
    static bool offersPasswordStage(const QJsonObject& body)
    {
        const QJsonValue flows = body.value(QStringLiteral("flows"));
        if (!flows.isArray()) return false;
        for (const QJsonValue& flow : flows.toArray()) {
            const QJsonValue stages = flow.toObject().value(QStringLiteral("stages"));
            if (!stages.isArray()) continue;
            for (const QJsonValue& stage : stages.toArray()) {
                if (stage.toString() == QLatin1String("m.login.password")) return true;
            }
        }
        return false;
    }

    static QString messageIn(const QJsonObject& body, const QString& fallback)
    {
        QString msg = body.value(QStringLiteral("error")).toString();
        if (msg.isEmpty()) msg = body.value(QStringLiteral("errcode")).toString();
        return msg.isEmpty() ? fallback : msg;
    }

    DeactivatePhase m_phase = DeactivatePhase::Idle;
    QString m_session;
    QString m_errorText;
};

} // namespace bsfchat::net
