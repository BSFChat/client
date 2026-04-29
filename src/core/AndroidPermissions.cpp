#include "AndroidPermissions.h"

#include <QtGlobal>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QDebug>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QPointer>
#endif

namespace {
#ifdef Q_OS_ANDROID

// PackageManager.PERMISSION_GRANTED = 0
constexpr jint kPermissionGranted = 0;

QJniObject activity()
{
    auto ctx = QNativeInterface::QAndroidApplication::context();
    return QJniObject(ctx);
}

bool isGranted(const QString& permission)
{
    QJniObject perm = QJniObject::fromString(permission);
    QJniObject act = activity();
    if (!act.isValid()) return false;
    jint res = act.callMethod<jint>("checkSelfPermission",
        "(Ljava/lang/String;)I", perm.object<jstring>());
    return res == kPermissionGranted;
}

// Request codes — the JNI callback ties the result back to the
// right signal. Arbitrary positive values; just unique per request
// shape.
constexpr int kRequestMic           = 1001;
constexpr int kRequestNotifications = 1002;
constexpr int kRequestCamera        = 1003;

// Global, set by Main once the single AndroidPermissions instance is
// created so the JNI bridge can route callbacks back to Qt without
// needing a QObject lookup. The QPointer keeps the raw pointer safe
// during shutdown.
QPointer<AndroidPermissions> g_instance;

// Native-method implementations registered against
// com.bsfchat.client.BSFChatActivity. Called on the Android UI
// thread; we marshal back to the Qt main thread via invokeMethod.

extern "C" JNIEXPORT void JNICALL
nativeOnNewIntent(JNIEnv*, jclass)
{
    auto* inst = g_instance.data();
    if (!inst) return;
    QMetaObject::invokeMethod(inst, [inst]() {
        emit inst->newIntentReceived();
    }, Qt::QueuedConnection);
}

extern "C" JNIEXPORT void JNICALL
nativeOnPermissionResult(JNIEnv* env, jclass,
                          jstring permissionJ, jboolean granted,
                          jint requestCode)
{
    auto* inst = g_instance.data();
    if (!inst) return;
    // Convert jstring outside the lambda — env pointers are thread-local
    // and the lambda runs on a different thread.
    const char* raw = env->GetStringUTFChars(permissionJ, nullptr);
    QString permission = QString::fromUtf8(raw);
    env->ReleaseStringUTFChars(permissionJ, raw);

    bool grantedCopy = (granted == JNI_TRUE);
    int requestCopy = int(requestCode);

    QMetaObject::invokeMethod(inst, [inst, permission, grantedCopy, requestCopy]() {
        inst->onNativePermissionResult(permission, grantedCopy, requestCopy);
    }, Qt::QueuedConnection);
}

// Registered once at first instance construction. Qt's JNI_OnLoad
// handles the initial env for us; we just need to register the
// native methods against our subclass.
void registerNatives()
{
    static bool registered = false;
    if (registered) return;

    JNINativeMethod methods[] = {
        { const_cast<char*>("nativeOnNewIntent"),
          const_cast<char*>("()V"),
          reinterpret_cast<void*>(nativeOnNewIntent) },
        { const_cast<char*>("nativeOnPermissionResult"),
          const_cast<char*>("(Ljava/lang/String;ZI)V"),
          reinterpret_cast<void*>(nativeOnPermissionResult) },
    };

    QJniEnvironment env;
    if (env.registerNativeMethods("com/bsfchat/client/BSFChatActivity",
                                   methods, 2)) {
        registered = true;
        qInfo("[permissions] Registered JNI bridges on BSFChatActivity");
    } else {
        qWarning("[permissions] Failed to register JNI bridges — "
                 "onNewIntent / permission callbacks won't fire");
    }
}

void requestPermission(const QString& permission, int requestCode)
{
    QJniObject act = activity();
    if (!act.isValid()) return;

    QJniEnvironment env;
    jobjectArray perms = env->NewObjectArray(
        1, env->FindClass("java/lang/String"), nullptr);
    env->SetObjectArrayElement(perms, 0,
        QJniObject::fromString(permission).object<jstring>());
    act.callMethod<void>("requestPermissions",
        "([Ljava/lang/String;I)V", perms, jint(requestCode));
    env->DeleteLocalRef(perms);
}

#endif // Q_OS_ANDROID
} // namespace

AndroidPermissions::AndroidPermissions(QObject* parent) : QObject(parent)
{
#ifdef Q_OS_ANDROID
    g_instance = this;
    registerNatives();
#endif
}

bool AndroidPermissions::hasMicrophone() const
{
#ifdef Q_OS_ANDROID
    return isGranted(QStringLiteral("android.permission.RECORD_AUDIO"));
#else
    return true;
#endif
}

bool AndroidPermissions::hasCamera() const
{
#ifdef Q_OS_ANDROID
    return isGranted(QStringLiteral("android.permission.CAMERA"));
#else
    return true;
#endif
}

bool AndroidPermissions::hasNotifications() const
{
#ifdef Q_OS_ANDROID
    // API < 33: POST_NOTIFICATIONS didn't exist and notifications are
    // implicitly allowed — checkSelfPermission returns DENIED on old
    // APIs for unknown permissions which isn't what we want.
    if (QNativeInterface::QAndroidApplication::sdkVersion() < 33) return true;
    return isGranted(
        QStringLiteral("android.permission.POST_NOTIFICATIONS"));
#else
    return true;
#endif
}

void AndroidPermissions::requestMicrophone()
{
#ifdef Q_OS_ANDROID
    if (hasMicrophone()) {
        emit microphoneResult(true);
        return;
    }
    requestPermission(QStringLiteral("android.permission.RECORD_AUDIO"),
                      kRequestMic);
#else
    emit microphoneResult(true);
#endif
}

void AndroidPermissions::requestCamera()
{
#ifdef Q_OS_ANDROID
    if (hasCamera()) {
        emit cameraResult(true);
        return;
    }
    requestPermission(QStringLiteral("android.permission.CAMERA"),
                      kRequestCamera);
#else
    emit cameraResult(true);
#endif
}

void AndroidPermissions::requestNotifications()
{
#ifdef Q_OS_ANDROID
    if (hasNotifications()) {
        emit notificationsResult(true);
        return;
    }
    requestPermission(
        QStringLiteral("android.permission.POST_NOTIFICATIONS"),
        kRequestNotifications);
#else
    emit notificationsResult(true);
#endif
}

void AndroidPermissions::onNativePermissionResult(const QString& permission,
                                                  bool granted,
                                                  int requestCode)
{
#ifdef Q_OS_ANDROID
    Q_UNUSED(requestCode);
    if (permission == QStringLiteral("android.permission.RECORD_AUDIO")) {
        emit microphoneResult(granted);
    } else if (permission ==
               QStringLiteral("android.permission.POST_NOTIFICATIONS")) {
        emit notificationsResult(granted);
    } else if (permission ==
               QStringLiteral("android.permission.CAMERA")) {
        emit cameraResult(granted);
    }
#else
    Q_UNUSED(permission);
    Q_UNUSED(granted);
    Q_UNUSED(requestCode);
#endif
}
