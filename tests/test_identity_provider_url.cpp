// "Manage Account" after a restart (self-hosted identity providers).
//
// The persisted per-server `identityProviderUrl` was written by the login
// path and read by nobody. The one feature that should consume it — the
// "Manage Account" item in ChannelList.qml — asked
// ServerConnection::identityProviderUrl(), which only ever answered from
// the LIVE IdentityClient. That object exists only in a process that ran
// the OIDC flow itself, so on every launch after the first the answer was
// empty and the QML fell through to a hard-coded https://id.bsfchat.com.
// On the hosted deployment that is invisible; a self-hoster running their
// own provider was sent to somebody else's portal after every restart.
//
// What's pinned here is the accessor's fallback and the two shapes in
// which ServerManager feeds it: restoring an entry from settings, and
// rebuilding a connection from the live one. ServerConnection reaches the
// network only once it is given credentials, which nothing here does, so
// this needs no server and no event loop of its own — and deliberately no
// GUI, since launching one on a dev Mac sets off macOS permission dialogs.
//
// The second half of the file is the same kind of defect one accessor
// along: displayName(). The "signed in as" block in ChannelList.qml and the
// locked panel in ServerSettings.qml both bind it, and both rendered the
// MXID where the name belongs — elided to "@oidc_a5cdbefe-9003-4f0…" in the
// menu, twice over in the panel — while the member list in the same window
// at the same moment rendered "josh" for that account. Same reason as the
// provider url: the value was only ever written by a path a restarted client
// never takes (a /profile reply for our own account), so the surfaces sat on
// the sentinel the login path seeds. Everything else about the account was
// learned from sync; the name was not.

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QStandardPaths>

#include <bsfchat/MatrixTypes.h>

#include "model/BotAdminModel.h"  // ServerConnection exposes a BotAdminModel*
                                   // Q_PROPERTY; moc needs the complete type,
                                   // and a forward declaration is not enough
                                   // once this TU instantiates the meta-type.
#include "net/ServerConnection.h"
#include "net/SyncLoop.h"

namespace {
const auto kServer = QStringLiteral("https://chat.selfhosted.example");
const auto kProvider = QStringLiteral("https://id.selfhosted.example");
// What ChannelList.qml substitutes when it is told nothing.
const auto kHostedFallback = QStringLiteral("https://id.bsfchat.com");

// The production account, spelled out: two OIDC accounts on one homeserver
// whose ids diverge only well past the width a name line gets.
const auto kUser = QStringLiteral(
    "@oidc_a5cdbefe-9003-4f09-b87e-238ba8df3264:chat.bsfchat.com");
const auto kLocalpart = QStringLiteral("oidc_a5cdbefe-9003-4f09-b87e-238ba8df3264");
const auto kName = QStringLiteral("josh");
const auto kDevice = QStringLiteral("DEV");
const auto kRoom = std::string("!general:chat.bsfchat.com");

// One /sync carrying our own m.room.member — the ONLY place a launch learns
// this account's name, and the event the member list builds its row from.
bsfchat::SyncResponse syncNaming(const QString& userId, const QString& name)
{
    bsfchat::RoomEvent ev;
    ev.type = "m.room.member";
    ev.state_key = userId.toStdString();
    ev.sender = userId.toStdString();
    ev.content.data["membership"] = "join";
    ev.content.data["displayname"] = name.toStdString();

    bsfchat::SyncResponse resp;
    resp.next_batch = "s1";
    resp.rooms.join[kRoom].state.events.push_back(ev);
    return resp;
}

// Hands a response to the real ingestion path. SyncLoop is a child of the
// connection and its completion signal is what processSyncResponse is
// connected to, so this is the live route with the socket left out.
void deliver(ServerConnection& conn, const bsfchat::SyncResponse& resp)
{
    auto* loop = conn.findChild<SyncLoop*>();
    QVERIFY2(loop, "the connection has no SyncLoop to deliver through");
    emit loop->syncCompleted(resp);
}

// The restored-entry shape, byte for byte what ServerManager's restore loop
// passes: an account whose stored display name is its own mxid, because
// nothing has ever folded a real one in. An empty token keeps every byte off
// the wire (setCredentials treats it as already-rejected and returns).
void restore(ServerConnection& conn, const QString& userId)
{
    conn.setCredentials(userId, QString(), kDevice, userId);
}
} // namespace

class TestIdentityProviderUrl : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The constructor hydrates its DM map from QSettings. Keep that
        // out of the developer's real preferences file.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("BSFChatTest"));
        QCoreApplication::setApplicationName(QStringLiteral("test_identity_provider_url"));
    }

    // The restart case. A connection restored from a saved entry has no
    // live IdentityClient — this process never ran the OIDC flow — and
    // must still report the provider that entry was saved with.
    void restoredConnectionReportsStoredProvider()
    {
        ServerConnection conn(kServer);
        // Exactly what ServerManager's restore loop does with
        // Settings::ServerEntry::identityProviderUrl.
        conn.setIdentityProviderUrl(kProvider);

        QCOMPARE(conn.identityProviderUrl(), kProvider);
        // The point of the whole change: no fallback, so "Manage Account"
        // opens the self-hoster's own portal rather than the hosted one.
        QVERIFY(conn.identityProviderUrl() != kHostedFallback);
    }

    // Nothing stored still means "we don't know", so the QML fallback
    // keeps applying to entries that predate the field being written.
    void unknownProviderStaysEmpty()
    {
        ServerConnection conn(kServer);
        QVERIFY(conn.identityProviderUrl().isEmpty());

        conn.setIdentityProviderUrl(QString());
        QVERIFY(conn.identityProviderUrl().isEmpty());
    }

    // rebuildConnection() copies from the live object rather than from
    // settings, so the restored value has to survive that hop — otherwise
    // a Reconnect, or an edit to the server URL, silently reintroduces
    // the fallback mid-session.
    void providerSurvivesRebuild()
    {
        ServerConnection restored(kServer);
        restored.setIdentityProviderUrl(kProvider);

        ServerConnection rebuilt(kServer);
        rebuilt.setIdentityProviderUrl(restored.identityProviderUrl());

        QCOMPARE(rebuilt.identityProviderUrl(), kProvider);
    }

    // ── displayName(): which account, in words ────────────────────────────

    // The observed bug. The name is in the sync the member list is drawing
    // its row from; the identity surfaces have to be reading the same fold,
    // or they say something the rest of the window contradicts.
    void aSyncedNameReachesTheIdentitySurfaces()
    {
        ServerConnection conn(kServer);
        restore(conn, kUser);

        // Before: the stored sentinel. Not the raw mxid even here — see the
        // next test — but certainly not a name.
        QVERIFY(conn.displayName() != kName);

        QSignalSpy renamed(&conn, &ServerConnection::displayNameChanged);
        deliver(conn, syncNaming(kUser, kName));

        QCOMPARE(conn.displayName(), kName);
        // The mxid is the line BELOW the name in both blocks. A name line
        // that repeats it is the whole defect, at any length.
        QVERIFY2(conn.displayName() != conn.userId(),
                 "the name line is a second copy of the mxid");
        QVERIFY2(!conn.displayName().startsWith(QLatin1Char('@')),
                 "the name line is showing an mxid");
        // Without the notify nothing rebinds and the fix is invisible: QML
        // reads this property once and never again.
        QVERIFY2(renamed.count() >= 1,
                 "the name changed without telling QML, so the block keeps "
                 "rendering the old one");
    }

    // An account that genuinely has no display name — the throwaway local
    // server. The block must still not print one string on both lines: the
    // localpart is the identifying half of the id and says something at any
    // width, which the shared "@" and homeserver do not. It is also exactly
    // what the member list prints for such an account
    // (MemberListModel::resolveName), so the two agree here as well.
    void anAccountWithNoNameGetsItsLocalpartNotItsMxid()
    {
        ServerConnection conn(kServer);
        restore(conn, kUser);

        QCOMPARE(conn.displayName(), kLocalpart);
        QVERIFY2(conn.displayName() != conn.userId(),
                 "with no display name the block prints the same string twice");
    }

    // A real name is never second-guessed into a localpart, in either
    // direction: what the server said is what is shown.
    void aStoredNameIsLeftAlone()
    {
        ServerConnection conn(kServer);
        conn.setCredentials(kUser, QString(), kDevice, kName);
        QCOMPARE(conn.displayName(), kName);
    }
};

// GUILESS, not QTEST_MAIN: the target links Qt6::Gui only because
// IdentityClient opens the browser through QDesktopServices, and a
// QGuiApplication here would want a window system — the thing every test
// in this directory exists to avoid on a dev Mac.
QTEST_GUILESS_MAIN(TestIdentityProviderUrl)
#include "test_identity_provider_url.moc"
