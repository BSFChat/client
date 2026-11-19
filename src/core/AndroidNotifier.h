// Minimal wrapper around Android's NotificationManager + a bridge to
// start/stop the SyncService that anchors the process lifetime — the
// persistent background connection, which since push is only used when
// push is not built in, not available on the device, or the user chose it
// (NotificationManager::setBackgroundSyncPolicy). Used
// from the desktop/ mobile shared NotificationManager so inbound
// messages continue to surface when the app is backgrounded.
//
// Safe to call on all platforms: methods no-op off Android so the
// call sites (src/core/NotificationManager.cpp) stay unbranched.
//
// Posting an Android notification requires:
//   * a notification channel (created once)
//   * the POST_NOTIFICATIONS runtime permission on 33+ (asked via
//     AndroidPermissions before first post)
//   * a PendingIntent for the tap action
//
// We post one notification per room (roomTag), so a newer message
// replaces the previous one rather than stacking. Tap action fires
// a bsfchat:// deep-link that UrlHandler already knows how to
// navigate.
#pragma once

#include <QObject>
#include <QString>

class AndroidNotifier : public QObject {
    Q_OBJECT
    // False whenever /sync is not anchored by a foreground service: either
    // it has not been started yet, or Android refused/stopped it. In that
    // state the app still syncs, but only while it is in the foreground.
    Q_PROPERTY(bool backgroundSyncActive READ backgroundSyncActive
               NOTIFY backgroundSyncActiveChanged)
public:
    explicit AndroidNotifier(QObject* parent = nullptr);

    // Show/hide the foreground sync service. Called at sign-in /
    // sign-out. Idempotent.
    //
    // startSyncService() records the INTENT to run background sync; the
    // service itself is only started when Android will actually allow it.
    // Two rules make that necessary:
    //
    //   * Android 12+ throws ForegroundServiceStartNotAllowedException
    //     from Context.startForegroundService() if the app is not in the
    //     foreground at the time. The call site here is a ServerManager
    //     signal that can fire during a restore, before the activity is
    //     resumed, so the start is deferred to the next transition to
    //     Qt::ApplicationActive rather than thrown.
    //   * Android 15 caps a `dataSync` service at ~6h per 24h and then
    //     stops it (SyncService.onTimeout). We do not fight that: the
    //     service stays down, backgroundSyncActive goes false, and one
    //     further attempt is made the next time the app is foregrounded.
    Q_INVOKABLE void startSyncService();
    Q_INVOKABLE void stopSyncService();

    bool backgroundSyncActive() const { return m_syncRunning; }

    // Called from the JNI bridge (SyncService.nativeOnSyncServiceStopped).
    // Public so the extern "C" shim can route through it.
    void onSyncServiceStopped(const QString& reason);

    // Post a chat notification. `tapDeepLink` should be a bsfchat://
    // URI (same format UrlHandler handles). Posting again with the same
    // tag replaces the earlier notification.
    Q_INVOKABLE void postChatNotification(const QString& tag,
                                          const QString& title,
                                          const QString& body,
                                          const QString& tapDeepLink);

    // The tag for a room's chat notification: "push|<roomId>", notification
    // id 1. PushBridge.java posts its generic push notifications under the
    // same tag and id, so the two replace each other and one cancelByTag
    // clears whichever is showing. Room ids carry their server name, so
    // the server URL is not needed to keep rooms apart.
    static QString roomTag(const QString& roomId)
    {
        return QStringLiteral("push|") + roomId;
    }

    // Clear by tag (used when a room is marked read).
    Q_INVOKABLE void cancelByTag(const QString& tag);
    // Clear every chat notification this process has posted.
    Q_INVOKABLE void cancelAll();

signals:
    void backgroundSyncActiveChanged();
    // Background sync is down and will not come back on its own until the
    // app is next foregrounded. `reason` is the raw tag from SyncService
    // ("dataSync-budget-exhausted", "start-refused", "deferred-background",
    // "start-threw") — meant for the log and for a UI that wants to say
    // "messages will only arrive while BSFChat is open".
    void backgroundSyncUnavailable(const QString& reason);

private:
    void tryStartSyncService();
    void setSyncRunning(bool running);

    // What the app wants, as opposed to what Android is allowing.
    bool m_syncWanted = false;
    bool m_syncRunning = false;
    // Set when Android 15 stops us for exceeding the dataSync budget.
    // Cleared on the next foreground transition, which then makes exactly
    // one more attempt — enough to recover once the 24h window rolls over
    // without turning a spent budget into a restart loop.
    bool m_budgetExhausted = false;
};
