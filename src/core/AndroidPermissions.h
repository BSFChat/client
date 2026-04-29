// Minimal runtime-permission helper for Android. Mirrors Qt 6's
// (deprecated) QtAndroidExtras::QtAndroid::requestPermission API
// using QJniObject so we don't depend on the private Qt Android
// extras headers that move between Qt versions.
//
// Why our own: we only need one permission (RECORD_AUDIO) at first
// voice-join time, and the call site is a simple "is it granted?
// if not, ask, and call this callback when the user answers" —
// which the public QPermission API doesn't quite serve on Qt 6.5
// without pulling in QtAndroidExtras compat.
#pragma once

#include <QObject>
#include <functional>

class AndroidPermissions : public QObject {
    Q_OBJECT
public:
    explicit AndroidPermissions(QObject* parent = nullptr);

    // True if RECORD_AUDIO is currently granted. Always true on non-
    // Android so desktop callers don't have to branch.
    Q_INVOKABLE bool hasMicrophone() const;
    // True if POST_NOTIFICATIONS is granted (or predates API 33).
    Q_INVOKABLE bool hasNotifications() const;
    Q_INVOKABLE bool hasCamera() const;

    // Fire-and-forget requests; the corresponding `*Result(granted)`
    // signal fires once the user answers. On non-Android both
    // short-circuit to granted=true.
    Q_INVOKABLE void requestMicrophone();
    Q_INVOKABLE void requestNotifications();
    Q_INVOKABLE void requestCamera();

    // Called from the JNI bridge (see BSFChatActivity). Public so the
    // extern "C" callback in the .cpp can route through it.
    void onNativePermissionResult(const QString& permission,
                                  bool granted, int requestCode);

signals:
    void microphoneResult(bool granted);
    void notificationsResult(bool granted);
    void cameraResult(bool granted);
    // New SEND intent arrived while the app was already running.
    // UrlHandler wires onto this and re-runs checkAndroidShareIntent.
    void newIntentReceived();
};
