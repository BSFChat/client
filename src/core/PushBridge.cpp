#include "core/PushBridge.h"

#include "core/PushPlatform.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaMethod>
#include <QPointer>
#include <QSysInfo>

#if defined(Q_OS_ANDROID) && defined(BSFCHAT_PUSH_ENABLED)
#define BSFCHAT_PUSH_ANDROID 1
#include <QJniEnvironment>
#include <QJniObject>
#endif

#if defined(Q_OS_IOS) && defined(BSFCHAT_PUSH_ENABLED)
#define BSFCHAT_PUSH_IOS 1
// src/core/PushBridgeIos.mm
namespace bsfchat::push::ios {
void attach(PushBridge* bridge); // flushes what arrived before Qt was up
void requestAuthorizationAndRegister();
QString deviceModel();          // UIDevice.model, e.g. "iPhone"

}
#endif

namespace {

#ifdef BSFCHAT_PUSH_ANDROID
constexpr const char* kJavaBridge = "com/bsfchat/client/PushBridge";

QString fromJString(JNIEnv* env, jstring s)
{
    if (!s) return {};
    const char* raw = env->GetStringUTFChars(s, nullptr);
    if (!raw) return {};
    QString out = QString::fromUtf8(raw);
    env->ReleaseStringUTFChars(s, raw);
    return out;
}

extern "C" JNIEXPORT void JNICALL
nativeOnPushToken(JNIEnv* env, jclass, jstring token, jstring deviceName)
{
    PushBridge::instance()->deliverToken(fromJString(env, token), fromJString(env, deviceName));
}

extern "C" JNIEXPORT void JNICALL
nativeOnPushUnavailable(JNIEnv* env, jclass, jstring reason)
{
    PushBridge::instance()->deliverUnavailable(fromJString(env, reason));
}

extern "C" JNIEXPORT jboolean JNICALL
nativeOnPushMessage(JNIEnv* env, jclass, jstring roomId, jstring eventId, jint unread)
{
    return PushBridge::instance()->deliverMessage(fromJString(env, roomId),
                                                  fromJString(env, eventId), int(unread))
               ? JNI_TRUE : JNI_FALSE;
}

void registerNatives()
{
    const JNINativeMethod methods[] = {
        { const_cast<char*>("nativeOnPushToken"),
          const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)V"),
          reinterpret_cast<void*>(nativeOnPushToken) },
        { const_cast<char*>("nativeOnPushUnavailable"),
          const_cast<char*>("(Ljava/lang/String;)V"),
          reinterpret_cast<void*>(nativeOnPushUnavailable) },
        { const_cast<char*>("nativeOnPushMessage"),
          const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;I)Z"),
          reinterpret_cast<void*>(nativeOnPushMessage) },
    };
    QJniEnvironment env;
    if (!env.registerNativeMethods(kJavaBridge, methods, 3)) {
        qWarning("[push] could not register the PushBridge natives; push is off");
        return;
    }
    QJniObject::callStaticMethod<void>(kJavaBridge, "setNativeReady", "(Z)V", jboolean(true));
}

QJniObject appContext()
{
    return QJniObject(QNativeInterface::QAndroidApplication::context());
}
#endif

} // namespace

PushBridge* PushBridge::instance()
{
    // Created on the GUI thread by PushManager before any platform callback
    // is wired up to it, so the lazy init is not raced in practice; the
    // static local makes it safe regardless.
    static PushBridge* bridge = [] {
        auto* b = new PushBridge(QCoreApplication::instance());
        return b;
    }();
    return bridge;
}

PushBridge::PushBridge(QObject* parent) : QObject(parent)
{
    m_deviceName = QSysInfo::prettyProductName();
#ifdef BSFCHAT_PUSH_ANDROID
    // Build.MANUFACTURER + MODEL: what the server's device list shows.
    const QString model = QJniObject::callStaticObjectMethod(
        kJavaBridge, "deviceName", "()Ljava/lang/String;").toString();
    if (!model.isEmpty()) m_deviceName = model;
    registerNatives();
#endif
#ifdef BSFCHAT_PUSH_IOS
    const QString model = bsfchat::push::ios::deviceModel();
    if (!model.isEmpty()) m_deviceName = model;
    bsfchat::push::ios::attach(this);
#endif
    if (!bsfchat::push::kCompiledIn) setUnavailable(QStringLiteral("not-built"));
}

void PushBridge::start(bool wanted)
{
#ifdef BSFCHAT_PUSH_ANDROID
    QJniObject ctx = appContext();
    if (!ctx.isValid()) return;
    QJniObject::callStaticMethod<void>(kJavaBridge, "start", "(Landroid/content/Context;Z)V",
                                       ctx.object(), jboolean(wanted));
#elif defined(BSFCHAT_PUSH_IOS)
    if (wanted) bsfchat::push::ios::requestAuthorizationAndRegister();
#else
    Q_UNUSED(wanted);
#endif
}

void PushBridge::setPreferences(bool notificationsEnabled, bool showNames)
{
#ifdef BSFCHAT_PUSH_ANDROID
    QJniObject ctx = appContext();
    if (!ctx.isValid()) return;
    QJniObject::callStaticMethod<void>(kJavaBridge, "setPrefs", "(Landroid/content/Context;ZZ)V",
                                       ctx.object(), jboolean(notificationsEnabled),
                                       jboolean(showNames));
#else
    Q_UNUSED(notificationsEnabled);
    Q_UNUSED(showNames);
#endif
}

void PushBridge::answerPushedEvent(const QString& eventId, bool handledBySync)
{
#ifdef BSFCHAT_PUSH_ANDROID
    QJniObject::callStaticMethod<void>(kJavaBridge, "onNativeDone", "(Ljava/lang/String;Z)V",
                                       QJniObject::fromString(eventId).object<jstring>(),
                                       jboolean(handledBySync));
#else
    Q_UNUSED(eventId);
    Q_UNUSED(handledBySync);
#endif
}

bool PushBridge::takePendingTap(QString* roomId, QString* eventId)
{
    if (m_pendingTapRoom.isEmpty()) return false;
    if (roomId) *roomId = m_pendingTapRoom;
    if (eventId) *eventId = m_pendingTapEvent;
    m_pendingTapRoom.clear();
    m_pendingTapEvent.clear();
    return true;
}

void PushBridge::deliverToken(const QString& token, const QString& deviceName)
{
    QPointer<PushBridge> self(this);
    QMetaObject::invokeMethod(this, [self, token, deviceName]() {
        if (self) self->setToken(token, deviceName);
    }, Qt::QueuedConnection);
}

void PushBridge::deliverUnavailable(const QString& reason)
{
    QPointer<PushBridge> self(this);
    QMetaObject::invokeMethod(this, [self, reason]() {
        if (self) self->setUnavailable(reason);
    }, Qt::QueuedConnection);
}

bool PushBridge::deliverMessage(const QString& roomId, const QString& eventId, int unread)
{
    if (roomId.isEmpty() || eventId.isEmpty()) return false;
    if (!isSignalConnected(QMetaMethod::fromSignal(&PushBridge::messageReceived))) return false;
    QPointer<PushBridge> self(this);
    QMetaObject::invokeMethod(this, [self, roomId, eventId, unread]() {
        if (self) emit self->messageReceived(roomId, eventId, unread);
    }, Qt::QueuedConnection);
    return true;
}

void PushBridge::deliverTap(const QString& roomId, const QString& eventId)
{
    if (roomId.isEmpty()) return;
    QPointer<PushBridge> self(this);
    QMetaObject::invokeMethod(this, [self, roomId, eventId]() {
        if (!self) return;
        if (self->isSignalConnected(QMetaMethod::fromSignal(&PushBridge::notificationTapped))) {
            emit self->notificationTapped(roomId, eventId);
        } else {
            self->m_pendingTapRoom = roomId;
            self->m_pendingTapEvent = eventId;
        }
    }, Qt::QueuedConnection);
}

void PushBridge::setToken(const QString& token, const QString& deviceName)
{
    if (!deviceName.isEmpty()) m_deviceName = deviceName;
    if (!token.isEmpty() && m_unavailable && bsfchat::push::kCompiledIn) {
        m_unavailable = false;
        m_unavailableReason.clear();
        emit unavailableChanged();
    }
    if (m_token == token) return;
    m_token = token;
    emit tokenChanged();
}

void PushBridge::setUnavailable(const QString& reason)
{
    m_unavailableReason = reason;
    if (m_unavailable) return;
    m_unavailable = true;
    qInfo().noquote() << "[push] unavailable on this device:" << reason;
    emit unavailableChanged();
}
