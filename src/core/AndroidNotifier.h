// Minimal wrapper around Android's NotificationManager + a bridge to
// start/stop the SyncService that anchors the process lifetime. Used
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
// We post per-event with a stable ID = hash(eventId) so re-posting
// the same event collapses rather than duplicates. Tap action fires
// a bsfchat:// deep-link that UrlHandler already knows how to
// navigate.
#pragma once

#include <QObject>
#include <QString>

class AndroidNotifier : public QObject {
    Q_OBJECT
public:
    explicit AndroidNotifier(QObject* parent = nullptr);

    // Show/hide the foreground sync service. Called at sign-in /
    // sign-out. Idempotent.
    Q_INVOKABLE void startSyncService();
    Q_INVOKABLE void stopSyncService();

    // Post a chat notification. `tapDeepLink` should be a bsfchat://
    // URI (same format UrlHandler handles). `groupKey` groups
    // notifications for the same room so users can swipe them
    // collectively.
    Q_INVOKABLE void postChatNotification(const QString& tag,
                                          const QString& title,
                                          const QString& body,
                                          const QString& tapDeepLink,
                                          const QString& groupKey = QString());

    // Clear by tag (used when a room is marked read).
    Q_INVOKABLE void cancelByTag(const QString& tag);
    // Clear every chat notification this process has posted.
    Q_INVOKABLE void cancelAll();
};
