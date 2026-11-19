// Keeps one Matrix HTTP pusher per signed-in account pointed at this
// device's platform push token (FCM registration token on Android, APNs
// device token on iOS). See docs/push-notifications.md for the whole
// design; this file is the part that talks to homeservers.
//
// What it does, per account:
//
//   * registers  POST /_matrix/client/v3/pushers/set {kind:"http", ...}
//     whenever a token is known, push is wanted and the pusher body it
//     last had accepted differs from the one it wants now (new token,
//     new gateway, different payload tier, first run of this process);
//   * deletes    the same endpoint with kind:null for a token that has
//     been replaced (rotation), and for the current one when push is no
//     longer wanted (notifications off, or the user chose a background
//     connection instead) or the account is signed out.
//
// One request per account in flight at a time; replies decide the next
// step. A homeserver that refuses the gateway (its
// push.allowed_gateway_prefixes does not list ours — the normal state of a
// self-hosted server that has not opted in) is recorded and NOT retried
// until the gateway URL changes: there is nothing to tell the user beyond a
// line in settings, and nothing hammering the server. A server whose
// push.max_payload is below "names" refuses the names tier; the registrar
// falls back to event_id_only for that account and says so.
//
// No Qt networking and no ServerConnection here: requests go through
// PushTransport, so tests/test_push_registrar.cpp drives the whole state
// machine with a fake.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace bsfchat::client::push {

enum class Platform { Android, Ios };

// What the server is asked to put in a notification. The server default is
// already event_id_only, but it is sent explicitly: an omitted format means
// "the operator's push.default_payload", and an operator may have raised
// that. A client asking for less always wins (server PushService.cpp).
enum class PayloadTier { EventIdOnly, Names };

// `data.format` values the BSFChat server understands (server
// core/Config.h, push_payload). "event_id_only" is the Matrix spec value;
// the names tier is namespaced because the spec defines no such level.
inline constexpr const char* kFormatEventIdOnly = "event_id_only";
inline constexpr const char* kFormatNames = "com.bsfchat.names";

inline constexpr const char* kDefaultGatewayUrl =
    "https://push.bsfchat.com/_matrix/push/v1/notify";
inline constexpr const char* kNotifyPath = "/_matrix/push/v1/notify";

// Fixed for the life of the process.
struct PusherIdentity {
    Platform platform = Platform::Android;
    QString appId;               // com.bsfchat.app[.dev] / com.bsfchat.app.ios[.dev]
    QString appDisplayName = QStringLiteral("BSFChat");
    QString deviceDisplayName;   // "Pixel 8", "iPhone"
    QString lang;                // BCP 47, e.g. "en-GB"
};

// Accepts "https://push.example.com" (adds the notify path) or a full
// notify URL; returns "" for anything that is not an absolute http(s) URL.
QString normaliseGatewayUrl(const QString& input);

// The /pushers/set body that registers `pushkey`. Pure, so the wire format
// is pinned by tests.
QJsonObject pusherBody(const PusherIdentity& id, const QString& pushkey,
                       const QString& gatewayUrl, PayloadTier tier);
// The body that deletes the pusher (app_id, pushkey).
QJsonObject deletionBody(const QString& appId, const QString& pushkey);

// How a /pushers/set reply is read.
enum class ReplyKind {
    Ok,
    GatewayRefused,  // 400: data.url not allowed by this server
    FormatRefused,   // 400: data.format above the server's push.max_payload
    Refused,         // any other 4xx: do not retry (incl. 404: no pushers API)
    AuthGone,        // 401/403: the session is dead; the account will be redone
    Retryable,       // transport error, 5xx, 429
};
ReplyKind classifyReply(int httpStatus, const QByteArray& body, bool transportError);

class PushTransport {
public:
    struct Reply {
        int status = 0;
        QByteArray body;
        bool transportError = false;
    };
    using Done = std::function<void(const Reply&)>;
    virtual ~PushTransport() = default;
    // POST /_matrix/client/v3/pushers/set on `account`. `done` must be called
    // exactly once, unless the account has gone away (then never — the
    // registrar has already forgotten it).
    virtual void setPusher(const QString& account, const QJsonObject& body, Done done) = 0;
};

} // namespace bsfchat::client::push

class PushRegistrar : public QObject {
    Q_OBJECT
public:
    using Platform = bsfchat::client::push::Platform;
    using PayloadTier = bsfchat::client::push::PayloadTier;
    using PusherIdentity = bsfchat::client::push::PusherIdentity;
    using PushTransport = bsfchat::client::push::PushTransport;

    enum class State {
        Idle,            // nothing to do (no token / not wanted / not registered)
        Busy,            // a request is in flight
        Registered,      // the wanted pusher is in place
        GatewayRefused,  // the server will not deliver to our gateway
        Refused,         // the server refused for another reason; see log
        Failed,          // transient failure; retried with backoff
    };
    Q_ENUM(State)

    PushRegistrar(PushTransport* transport, PusherIdentity identity,
                  QObject* parent = nullptr);

    const PusherIdentity& identity() const { return m_identity; }

    // Inputs. Each one re-evaluates every account.
    void setToken(const QString& pushkey);
    QString token() const { return m_token; }
    void setWanted(bool wanted);
    bool wanted() const { return m_wanted; }
    void setTier(PayloadTier tier);
    // Normalised with normaliseGatewayUrl; an invalid value disables
    // registration (and deletes what was registered) rather than sending
    // garbage a server would refuse anyway.
    void setGatewayUrl(const QString& url);
    QString gatewayUrl() const { return m_gatewayUrl; }

    // An account is signed in and reachable. `registeredPushkey` is what a
    // previous run left registered (persisted by the caller from
    // registeredPushkeyChanged), so a token that rotated while the app was
    // not running still has its old pusher deleted.
    void addAccount(const QString& account, const QString& registeredPushkey = QString());
    bool hasAccount(const QString& account) const { return m_accounts.contains(account); }
    // The account is being signed out. Returns the deletion bodies still
    // owed for it (the registered pusher and any stale ones) so the caller
    // can send them on the session before it is revoked, and forgets it.
    QList<QJsonObject> takeAccount(const QString& account);
    // The account went away without a sign-out (connection rebuilt). Keeps
    // nothing; the server-side pusher stays and a later addAccount resumes.
    void dropAccount(const QString& account);

    State state(const QString& account) const;
    // The server refused the names tier and this account is on
    // event_id_only instead.
    bool namesRefused(const QString& account) const;
    QString lastError(const QString& account) const;
    QStringList accounts() const { return m_accounts.keys(); }

    // Retry every account in Failed now (on reconnect, on foreground).
    void retryFailed();
    // Off in tests that want to step retries by hand.
    void setAutoRetry(bool on) { m_autoRetry = on; }
    static int backoffMs(int failures);

signals:
    // Persist this: it is what addAccount wants back next run. "" = none.
    void registeredPushkeyChanged(const QString& account, const QString& pushkey);
    void stateChanged(const QString& account);

private:
    struct Account {
        QString registeredKey;        // pushkey believed registered on the server
        QJsonObject registeredBody;   // body last accepted; empty = unknown
        QStringList stale;            // pushkeys still to delete
        State state = State::Idle;
        bool busy = false;
        bool namesRefused = false;
        QString refusedGateway;       // gateway the server refused
        QJsonObject refusedBody;      // body the server refused for good
        QString lastError;
        int failures = 0;
        quint64 generation = 0;       // bumps on drop/take so late replies are ignored
    };

    QJsonObject desiredBody(const Account& a) const;
    void pump(const QString& account);
    void pumpAll();
    void setState(const QString& account, Account& a, State s);
    void scheduleRetry(const QString& account);
    void setRegistered(const QString& account, Account& a, const QString& key,
                       const QJsonObject& body);

    PushTransport* m_transport;
    PusherIdentity m_identity;
    QString m_token;
    bool m_wanted = false;
    PayloadTier m_tier = PayloadTier::EventIdOnly;
    QString m_gatewayUrl;
    bool m_autoRetry = true;
    QHash<QString, Account> m_accounts;
    quint64 m_nextGeneration = 1;
};

namespace bsfchat::client::push {

// ---- The settings status line and the Android background decision ----
// Pure, so both are pinned by tests/test_push_registrar.cpp; PushManager
// gathers the inputs from the bridge, the settings and the registrar.

struct AccountStatus {
    QString host;
    PushRegistrar::State state = PushRegistrar::State::Idle;
    bool namesRefused = false;
    QString lastError;
};

struct StatusInputs {
    bool compiledIn = false;
    bool ios = false;
    bool notificationsOn = false;
    bool backgroundChosen = false;   // Android: "background connection instead"
    bool bridgeUnavailable = false;  // the platform said push cannot work here
    bool tokenRetrying = false;      // a transient token failure, retrying
    bool hasToken = false;
    bool permissionBlocked = false;  // OS notification permission denied
    bool gatewayValid = true;
    bool showNames = false;
    // Signed-in accounts the registrar knows (i.e. connected at least once).
    QList<AccountStatus> accounts;
};

struct StatusSummary {
    QString text;
    // Android: whether SyncService should run. True whenever some
    // signed-in server cannot reach this device through push (audit 3 #9).
    bool backgroundSyncWanted = false;
    // Hosts that rely on the background connection rather than push.
    QStringList onBackground;
    int registered = 0;
    int refused = 0;   // GatewayRefused + Refused
    int failed = 0;
};

StatusSummary summariseStatus(const StatusInputs& in);

} // namespace bsfchat::client::push
