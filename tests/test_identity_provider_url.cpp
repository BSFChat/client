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

#include <QtTest/QtTest>
#include <QStandardPaths>

#include "model/BotAdminModel.h"  // ServerConnection exposes a BotAdminModel*
                                   // Q_PROPERTY; moc needs the complete type,
                                   // and a forward declaration is not enough
                                   // once this TU instantiates the meta-type.
#include "net/ServerConnection.h"

namespace {
const auto kServer = QStringLiteral("https://chat.selfhosted.example");
const auto kProvider = QStringLiteral("https://id.selfhosted.example");
// What ChannelList.qml substitutes when it is told nothing.
const auto kHostedFallback = QStringLiteral("https://id.bsfchat.com");
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
};

// GUILESS, not QTEST_MAIN: the target links Qt6::Gui only because
// IdentityClient opens the browser through QDesktopServices, and a
// QGuiApplication here would want a window system — the thing every test
// in this directory exists to avoid on a dev Mac.
QTEST_GUILESS_MAIN(TestIdentityProviderUrl)
#include "test_identity_provider_url.moc"
