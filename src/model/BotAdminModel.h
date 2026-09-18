#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>

// View-model behind the bot management dialog.
//
// It exists apart from ServerConnection for the reason PermissionMath.h gives
// for living apart from it: the interesting behaviour here is a small state
// machine over replies that arrive out of order, and ServerConnection cannot
// be instantiated in a unit test. Everything that decides what the dialog
// shows — which rows, whether the one-time token banner is up, what the error
// line says, whether the destructive button is armed — is decided here and
// tested in tests/test_bots.cpp.
//
// It reaches the network through `Hooks` (std::function) rather than by
// calling MatrixClient, the same seam VoiceSession uses for its transport, so
// the test substitutes a recording fake and drives the replies by hand.
// ServerConnection installs the real hooks and forwards MatrixClient's
// signals into the on*() slots.
//
// ─────────────────────────── the token rule ───────────────────────────
//
// The server returns a bot's access token EXACTLY ONCE, at creation and at
// each rotation, and can never produce it again. Two consequences run through
// this whole class:
//
//   1. A token that is not shown is lost, and the only recovery is a rotation
//      that invalidates whatever the operator may already have deployed. So
//      the banner is sticky: nothing dismisses it but an explicit click, not
//      a refresh landing underneath it, not another bot being created, not a
//      list reply arriving late.
//
//   2. A token that leaks is a full impersonation of that bot. It is
//      therefore held in exactly one place — m_issuedToken — for exactly as
//      long as the banner is up, and it is never logged, never written to
//      QSettings, never put in a signal argument that anything else stores,
//      and never included in the row data that backs the list. There is
//      deliberately no getter that takes a user id: the only readable token
//      is the one currently on screen.
//
// The client mirrors every qDebug/qWarning line into a rotating file
// (util/FileLogger.h) and verbose mode turns on `bsfchat.*=true` wholesale,
// so "we only log it at debug level" would still put it on disk. Nothing in
// this class or on the path that feeds it logs a response body at all; a test
// scans the sources to keep it that way.
class BotAdminModel : public QObject {
    Q_OBJECT

    // The bots as last listed. Entries are QVariantMaps:
    //   { userId, displayName, description, ownerId, createdAt,
    //     deactivated, lastSeenAt }
    // Never a token — see above.
    Q_PROPERTY(QVariantList bots READ bots NOTIFY botsChanged)
    // A request is in flight. The dialog disables its buttons on this rather
    // than tracking per-row state: every one of these operations rewrites the
    // list, so letting a second one start mid-flight only produces a race
    // between two refreshes.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True once a list reply has ever landed. Distinguishes "no bots on this
    // server" (show the empty state) from "we have not asked yet" (show the
    // spinner) — the two look identical from an empty `bots`.
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    // Last failure, empty when the last operation succeeded. Plain server
    // text; the dialog renders it inline rather than as a toast so it sits
    // next to the control that failed.
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

    // ── the one-time token banner ──────────────────────────────────────────
    // Non-empty only while a freshly issued token is on screen.
    Q_PROPERTY(QString issuedToken READ issuedToken NOTIFY issuedTokenChanged)
    // Who it belongs to, and whether it came from a create or a rotate —
    // the warning copy differs ("save this" vs "the old one just stopped
    // working"), and an operator needs to know which bot they are looking at
    // when they created two in a row.
    Q_PROPERTY(QString issuedTokenUserId READ issuedTokenUserId NOTIFY issuedTokenChanged)
    Q_PROPERTY(bool issuedTokenIsRotation READ issuedTokenIsRotation NOTIFY issuedTokenChanged)
    Q_PROPERTY(bool hasIssuedToken READ hasIssuedToken NOTIFY issuedTokenChanged)

public:
    explicit BotAdminModel(QObject* parent = nullptr);

    // The network seam. ServerConnection fills these in; the test records the
    // calls. All four are optional — an unset hook makes the corresponding
    // action a no-op rather than a crash, which is what a dialog opened
    // against a disconnected server should do.
    struct Hooks {
        std::function<void()> listBots;
        std::function<void(const QString& localpart,
                           const QString& displayName,
                           const QString& description)> createBot;
        std::function<void(const QString& userId)> rotateToken;
        std::function<void(const QString& userId)> deactivateBot;
    };
    Hooks hooks;

    QVariantList bots() const { return m_bots; }
    bool busy() const { return m_busy; }
    bool loaded() const { return m_loaded; }
    QString errorText() const { return m_errorText; }
    QString issuedToken() const { return m_issuedToken; }
    QString issuedTokenUserId() const { return m_issuedTokenUserId; }
    bool issuedTokenIsRotation() const { return m_issuedTokenIsRotation; }
    bool hasIssuedToken() const { return !m_issuedToken.isEmpty(); }

    // ── actions (called from QML) ──────────────────────────────────────────

    Q_INVOKABLE void refresh();

    // `localpart` is the bit before the colon in the bot's user id; the
    // server owns the final id and returns it. Validated here as well as
    // server-side so a typo is refused without a round trip and without the
    // dialog having to guess what the server's 400 meant — see
    // localpartError().
    Q_INVOKABLE void createBot(const QString& localpart,
                               const QString& displayName,
                               const QString& description);
    Q_INVOKABLE void rotateToken(const QString& userId);
    Q_INVOKABLE void deactivateBot(const QString& userId);

    // Empty string when `localpart` is acceptable, otherwise the reason to
    // show under the field. Called on every keystroke so Create can be
    // disabled rather than attempted.
    //
    // The rule matches Matrix's own localpart grammar as the server applies
    // it: lowercase a–z, digits, and `.`, `_`, `=`, `-`, `/`, `+`, at least
    // one character. It is a courtesy check, not a security boundary — the
    // server validates independently and its answer wins.
    //
    // Two spellings of one function: the static one is the rule, and the
    // invokable instance method is what QML calls. `Q_INVOKABLE static` does
    // meta-register, but calling a static through an instance from QML is a
    // corner of the binding that differs between Qt versions and cannot be
    // checked here without launching the app — so the QML-facing entry point
    // is an ordinary const method and there is nothing to find out at
    // runtime.
    static QString localpartError(const QString& localpart);
    Q_INVOKABLE QString validateLocalpart(const QString& localpart) const
    {
        return localpartError(localpart);
    }

    // Drop the one-time token from the UI and from this object. The ONLY way
    // the banner comes down. Called by the dialog's "I've saved it" button
    // and by reset() when the dialog closes.
    Q_INVOKABLE void dismissToken();

    // Find a listed bot's display name for confirmation copy ("Deactivate
    // Build Bot?"). Falls back to the user id, which is always meaningful.
    Q_INVOKABLE QString displayNameFor(const QString& userId) const;

    // Whole-dialog reset, on close. Clears the token as well as the list:
    // reopening the dialog must not resurrect a token the operator walked
    // away from, and holding it beyond the dialog's life buys nothing.
    Q_INVOKABLE void reset();

    // ── replies (wired from ServerConnection) ──────────────────────────────

    // `bots` is the raw `bots` array from GET /bsfchat/bots.
    void onBotsListed(const QJsonArray& bots);
    // A create landed. `token` is the one-and-only copy; it goes to the
    // banner and nowhere else.
    void onBotCreated(const QString& userId, const QString& displayName,
                      const QString& token);
    void onTokenRotated(const QString& userId, const QString& token);
    void onBotDeactivated(const QString& userId);
    // `operation` is one of "list"/"create"/"rotate"/"deactivate", used only
    // to phrase the message; `message` is the server's text.
    void onFailed(const QString& operation, const QString& message);

    // The list of user ids currently known to be bots, for seeding
    // BotRegistry from the admin view. Deactivated bots are INCLUDED: a
    // deactivated bot's messages are still in the timeline and were still
    // written by a bot, so the badge stays.
    QStringList listedBotUserIds() const;

signals:
    void botsChanged();
    void busyChanged();
    void loadedChanged();
    void errorTextChanged();
    void issuedTokenChanged();

    // Raised when a create or rotate produced a token. The token is NOT an
    // argument: anything connected to this reads it from `issuedToken` while
    // the banner is up. Passing it through a signal would put a copy in every
    // queued-connection event and in any QML handler's arguments object, for
    // no gain — the one consumer is the dialog, which is already bound to the
    // property.
    void tokenIssued(const QString& userId, bool rotation);

    // A bot was created or deactivated. ServerConnection listens so it can
    // keep BotRegistry (and therefore every badge) in step without waiting
    // for the profile probe to come round again.
    void botSetChanged();

private:
    void setBusy(bool busy);
    void setErrorText(const QString& text);
    void presentToken(const QString& userId, const QString& token, bool rotation);

    QVariantList m_bots;
    bool m_busy = false;
    bool m_loaded = false;
    QString m_errorText;

    QString m_issuedToken;
    QString m_issuedTokenUserId;
    bool m_issuedTokenIsRotation = false;
};
