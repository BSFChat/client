// The platform end of mobile push: where the FCM / APNs token comes from,
// where pushed messages and notification taps come in. One per process,
// because the JNI and Objective-C callbacks that feed it have no object to
// hand. Compiled everywhere; off Android/iOS (and in builds without
// BSFCHAT_PUSH_ENABLED) every request is a no-op and nothing is emitted.
//
//   Android: android/src/com/bsfchat/client/PushBridge.java (+ FcmPush,
//            PushMessagingService staged by cmake/AndroidPush.cmake).
//   iOS:     src/core/PushBridgeIos.mm.
//
// PushManager is the only consumer. See docs/push-notifications.md.
#pragma once

#include <QObject>
#include <QString>

class PushBridge : public QObject {
    Q_OBJECT
public:
    static PushBridge* instance();

    // Android: replay the stored FCM token and ask Firebase for a current
    // one (wanted=true), or stop Firebase and delete the token
    // (wanted=false: notifications off, or a background connection instead).
    // iOS: wanted=true asks for notification permission (alert, sound,
    // badge; the system prompts only the first time) and registers for
    // remote notifications. wanted=false does nothing — the pushers are
    // deleted server-side, which is what stops delivery.
    void start(bool wanted);
    // Android: mirror the two settings the FCM service needs when Qt is not
    // running. No-op elsewhere.
    void setPreferences(bool notificationsEnabled, bool showNames);
    // Android: tell the FCM service whether this process's /sync delivered
    // a pushed event (it then posts nothing itself).
    void answerPushedEvent(const QString& eventId, bool handledBySync);

    QString token() const { return m_token; }
    QString deviceName() const { return m_deviceName; }
    // The platform said push cannot work here (no Google Play services,
    // APNs registration failed, permission denied, not configured).
    bool unavailable() const { return m_unavailable; }
    QString unavailableReason() const { return m_unavailableReason; }
    // Android: getting a token failed transiently and is being retried
    // with backoff (FcmPush). Not "unavailable": a token may yet come.
    bool retrying() const { return m_retrying; }
    // The OS has notifications for this app switched off.
    bool notificationsBlocked() const { return m_notificationsBlocked; }
    // Re-read the permission (on return to the foreground: the user may
    // have just been to system settings).
    void refreshPermission();
    // System settings, at this app's notification page where possible.
    void openNotificationSettings();
    // iOS: the app icon badge. A push sets it from the server's count while
    // BSFChat is not running; the client puts it back to its own total
    // whenever that changes, 0 included. No-op elsewhere (desktop docks
    // and Android launchers are NotificationManager's).
    void setBadgeCount(int count);

    // A tap that arrived before anyone was listening (cold start).
    bool takePendingTap(QString* roomId, QString* eventId);

    // Fed by the platform code. Any thread.
    void deliverToken(const QString& token, const QString& deviceName);
    void deliverUnavailable(const QString& reason);
    void deliverRetrying(const QString& reason);
    void deliverPermission(bool blocked);
    // Returns whether the event was taken (Android: C++ will answer with
    // answerPushedEvent).
    bool deliverMessage(const QString& roomId, const QString& eventId, int unread);
    void deliverTap(const QString& roomId, const QString& eventId);

signals:
    void tokenChanged();
    void unavailableChanged();
    void retryingChanged();
    void permissionChanged();
    void messageReceived(const QString& roomId, const QString& eventId, int unread);
    void notificationTapped(const QString& roomId, const QString& eventId);

private:
    explicit PushBridge(QObject* parent = nullptr);
    void setToken(const QString& token, const QString& deviceName);
    void setUnavailable(const QString& reason);

    QString m_token;
    QString m_deviceName;
    bool m_unavailable = false;
    bool m_retrying = false;
    bool m_notificationsBlocked = false;
    QString m_unavailableReason;
    QString m_pendingTapRoom;
    QString m_pendingTapEvent;
};
