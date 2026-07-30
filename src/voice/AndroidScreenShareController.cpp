#include "AndroidScreenShareController.h"

#include <QtGlobal>

#if defined(Q_OS_ANDROID) && defined(BSFCHAT_VOICE_ENABLED)

#include "core/Settings.h"
#include "net/ServerConnection.h"
#include "net/ServerManager.h"
#include "voice/IVoiceTransport.h"
#include "voice/VoiceEngine.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDebug>
#include <QJniEnvironment>
#include <QJniObject>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>

Q_LOGGING_CATEGORY(logScreenShare, "bsfchat.screenshare", QtWarningMsg)

namespace {

// Only one controller instance is ever in use (main.cpp owns it
// as a stack-allocated object). Stash a weak pointer globally so
// the extern "C" JNI callbacks can route back without plumbing
// a jobject reference through every call.
QPointer<AndroidScreenShareController> g_instance;

QJniObject helper()
{
    return QJniObject::callStaticObjectMethod(
        "com/bsfchat/client/ScreenCaptureHelper",
        "instance",
        "()Lcom/bsfchat/client/ScreenCaptureHelper;");
}

QJniObject currentActivity()
{
    return QJniObject(QNativeInterface::QAndroidApplication::context());
}

extern "C" JNIEXPORT void JNICALL
nativeOnStarted(JNIEnv*, jclass, jint w, jint h)
{
    auto* inst = g_instance.data();
    if (!inst) return;
    int ww = int(w), hh = int(h);
    QMetaObject::invokeMethod(inst, [inst, ww, hh]() {
        inst->onStarted(ww, hh);
    }, Qt::QueuedConnection);
}

extern "C" JNIEXPORT void JNICALL
nativeOnStopped(JNIEnv*, jclass)
{
    auto* inst = g_instance.data();
    if (!inst) return;
    QMetaObject::invokeMethod(inst, [inst]() {
        inst->onStopped();
    }, Qt::QueuedConnection);
}

extern "C" JNIEXPORT void JNICALL
nativeOnPermissionDenied(JNIEnv*, jclass)
{
    auto* inst = g_instance.data();
    if (!inst) return;
    QMetaObject::invokeMethod(inst, [inst]() {
        inst->onPermissionDenied();
    }, Qt::QueuedConnection);
}

extern "C" JNIEXPORT void JNICALL
nativeOnFrame(JNIEnv* env, jclass, jbyteArray jpeg)
{
    auto* inst = g_instance.data();
    if (!inst) return;
    jsize len = env->GetArrayLength(jpeg);
    QByteArray bytes;
    bytes.resize(int(len));
    env->GetByteArrayRegion(jpeg, 0, len,
        reinterpret_cast<jbyte*>(bytes.data()));
    QMetaObject::invokeMethod(inst, [inst, bytes]() {
        inst->onFrame(bytes);
    }, Qt::QueuedConnection);
}

void registerNatives()
{
    static bool registered = false;
    if (registered) return;

    JNINativeMethod methods[] = {
        { const_cast<char*>("nativeOnStarted"),
          const_cast<char*>("(II)V"),
          reinterpret_cast<void*>(nativeOnStarted) },
        { const_cast<char*>("nativeOnStopped"),
          const_cast<char*>("()V"),
          reinterpret_cast<void*>(nativeOnStopped) },
        { const_cast<char*>("nativeOnPermissionDenied"),
          const_cast<char*>("()V"),
          reinterpret_cast<void*>(nativeOnPermissionDenied) },
        { const_cast<char*>("nativeOnFrame"),
          const_cast<char*>("([B)V"),
          reinterpret_cast<void*>(nativeOnFrame) },
    };
    QJniEnvironment env;
    if (env.registerNativeMethods(
            "com/bsfchat/client/ScreenCaptureHelper",
            methods, 4)) {
        registered = true;
        qCInfo(logScreenShare)
            << "Registered JNI bridges on ScreenCaptureHelper";
    } else {
        qCWarning(logScreenShare)
            << "Failed to register JNI bridges on "
               "ScreenCaptureHelper — frames will never arrive";
    }
}

} // namespace

AndroidScreenShareController::AndroidScreenShareController(QObject* parent)
    : QObject(parent)
{
    g_instance = this;
    registerNatives();
}

void AndroidScreenShareController::showPicker()
{
    setLastError(QString());
    // Push the latest user/server-resolved config into the Java
    // helper before we ask for projection consent — the helper
    // bakes those values into the VirtualDisplay it creates,
    // so they need to be in place at that moment.
    pushConfigToHelper();

    QJniObject h = helper();
    if (!h.isValid()) {
        setLastError(QStringLiteral("Screen capture helper unavailable"));
        return;
    }
    QJniObject act = currentActivity();
    if (!act.isValid()) {
        setLastError(QStringLiteral("No activity"));
        return;
    }
    h.callMethod<void>("requestPermission",
        "(Landroid/app/Activity;)V", act.object());
}

void AndroidScreenShareController::setSettings(Settings* s)
{
    if (m_settings == s) return;
    m_settings = s;
    if (!s) return;
    auto reapply = [this]() { pushConfigToHelper(); };
    QObject::connect(s, &Settings::screenShareFpsChanged, this, reapply);
    QObject::connect(s, &Settings::screenShareMaxWidthChanged, this, reapply);
    QObject::connect(s, &Settings::screenShareJpegQualityChanged, this, reapply);
}

void AndroidScreenShareController::pushConfigToHelper()
{
    if (!m_settings) return;

    int userFps   = m_settings->screenShareFps();
    int userW     = m_settings->screenShareMaxWidth();
    int userJ     = m_settings->screenShareJpegQuality();

    int srvFps = -1, srvW = -1, srvJ = -1;
    if (m_serverManager) {
        ServerConnection* conn = m_serverManager->activeServer();
        if (conn) {
            srvFps = conn->maxScreenShareFps();
            srvW   = conn->maxScreenShareWidth();
            srvJ   = conn->maxScreenShareJpeg();
        }
    }
    int fps = (srvFps >= 0) ? std::min(userFps, srvFps) : userFps;
    int maxW = (srvW >= 0) ? std::min(userW, srvW) : userW;
    int jpeg = (srvJ >= 0) ? std::min(userJ, srvJ) : userJ;

    QJniEnvironment env;
    QJniObject::callStaticMethod<void>(
        "com/bsfchat/client/ScreenCaptureHelper",
        "configure", "(III)V",
        jint(maxW), jint(fps), jint(jpeg));
    qCInfo(logScreenShare,
        "config pushed → fps=%d maxW=%d Q=%d (user fps=%d maxW=%d Q=%d, "
        "server caps fps=%d maxW=%d Q=%d)",
        fps, maxW, jpeg, userFps, userW, userJ, srvFps, srvW, srvJ);
}

void AndroidScreenShareController::stop()
{
    QJniObject h = helper();
    if (!h.isValid()) return;
    h.callMethod<void>("stopCapture", "()V");
    // The helper's callback fires onStopped() via JNI; we clear
    // active there. But if stop was invoked before any callback
    // ever ran, clear now so the UI snaps back immediately.
    setActive(false);
}

void AndroidScreenShareController::onStarted(int w, int h)
{
    m_width = w;
    m_height = h;
    qCInfo(logScreenShare, "capture started %dx%d", w, h);
    setActive(true);
}

void AndroidScreenShareController::onStopped()
{
    qCInfo(logScreenShare, "capture stopped");
    setActive(false);
}

void AndroidScreenShareController::onPermissionDenied()
{
    qCInfo(logScreenShare, "user denied projection consent");
    setLastError(QStringLiteral("Screen share permission denied"));
    setActive(false);
}

void AndroidScreenShareController::onFrame(const QByteArray& jpeg)
{
    if (!m_active) return;
    broadcast(jpeg);
}

void AndroidScreenShareController::broadcast(const QByteArray& jpeg)
{
    if (!m_serverManager) return;
    ServerConnection* conn = m_serverManager->activeServer();
    if (!conn) return;
    // Android is mesh-only (no LiveKit SDK for mobile) but this only
    // uses the transport-neutral JPEG sink, so it takes the interface.
    IVoiceTransport* eng = conn->voiceEngine();
    if (!eng) return;
    eng->broadcastScreenFrame(jpeg);
}

void AndroidScreenShareController::setActive(bool a)
{
    if (m_active == a) return;
    m_active = a;
    emit activeChanged();
}

void AndroidScreenShareController::setLastError(const QString& e)
{
    if (m_lastError == e) return;
    m_lastError = e;
    emit lastErrorChanged();
}

#else // Non-Android or no voice build: stub out the class so
      // main.cpp can still link + instantiate without ifdefs.

AndroidScreenShareController::AndroidScreenShareController(QObject* parent)
    : QObject(parent) {}
void AndroidScreenShareController::showPicker() {}
void AndroidScreenShareController::stop() {}
void AndroidScreenShareController::onStarted(int, int) {}
void AndroidScreenShareController::onStopped() {}
void AndroidScreenShareController::onPermissionDenied() {}
void AndroidScreenShareController::onFrame(const QByteArray&) {}
void AndroidScreenShareController::broadcast(const QByteArray&) {}
void AndroidScreenShareController::setActive(bool) {}
void AndroidScreenShareController::setLastError(const QString&) {}

#endif
