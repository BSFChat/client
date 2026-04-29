#include "AndroidAudioRouting.h"

#include <QtGlobal>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QDebug>
#include <QJniEnvironment>
#include <QJniObject>

namespace {

// AudioManager constants (stable across API levels 14..34, verified
// against Android source).
constexpr int kAudioManagerStreamVoiceCall     = 0;
constexpr int kAudioManagerStreamMusic         = 3;
constexpr int kAudioManagerModeNormal          = 0;
constexpr int kAudioManagerModeInCommunication = 3;
// AudioManager.AUDIOFOCUS_GAIN                 = 1
constexpr int kAudioFocusGain                  = 1;
// AudioManager.AUDIOFOCUS_REQUEST_GRANTED      = 1
constexpr int kAudioFocusRequestGranted        = 1;

bool g_inVoiceMode = false;
int g_savedMode = kAudioManagerModeNormal;

QJniObject appContext()
{
    return QJniObject(QNativeInterface::QAndroidApplication::context());
}

void startVoiceService()
{
    // context.startForegroundService(new Intent(context, VoiceService.class))
    // Pre-O we'd call startService(); post-O startForegroundService() is
    // required or the platform throws ForegroundServiceStartNotAllowedException.
    QJniObject ctx = appContext();
    if (!ctx.isValid()) return;

    QJniObject intent("android/content/Intent");
    if (!intent.isValid()) return;
    QJniEnvironment env;
    jclass svcClass = env->FindClass("com/bsfchat/client/VoiceService");
    if (!svcClass) {
        env->ExceptionClear();
        qWarning("[audio-routing] VoiceService class not found — foreground "
                 "service won't anchor the voice call; backgrounded app "
                 "will be killed. Check android/src wiring.");
        return;
    }
    QJniObject classRef(reinterpret_cast<jobject>(svcClass));
    intent.callObjectMethod("setClass",
        "(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;",
        ctx.object(), classRef.object());
    env->DeleteLocalRef(svcClass);

    if (QNativeInterface::QAndroidApplication::sdkVersion() >= 26) {
        ctx.callObjectMethod("startForegroundService",
            "(Landroid/content/Intent;)Landroid/content/ComponentName;",
            intent.object());
    } else {
        ctx.callObjectMethod("startService",
            "(Landroid/content/Intent;)Landroid/content/ComponentName;",
            intent.object());
    }
    qInfo("[audio-routing] VoiceService started");
}

void stopVoiceService()
{
    QJniObject ctx = appContext();
    if (!ctx.isValid()) return;

    QJniObject intent("android/content/Intent");
    if (!intent.isValid()) return;
    QJniEnvironment env;
    jclass svcClass = env->FindClass("com/bsfchat/client/VoiceService");
    if (!svcClass) {
        env->ExceptionClear();
        return;
    }
    QJniObject classRef(reinterpret_cast<jobject>(svcClass));
    intent.callObjectMethod("setClass",
        "(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;",
        ctx.object(), classRef.object());
    env->DeleteLocalRef(svcClass);

    ctx.callMethod<jboolean>("stopService",
        "(Landroid/content/Intent;)Z", intent.object());
    qInfo("[audio-routing] VoiceService stopped");
}

QJniObject audioManager()
{
    // Context.getSystemService(Context.AUDIO_SERVICE) — we fetch the
    // "audio" system-service name literal rather than the Context
    // static via JNI (which would be another hop); Android's source
    // defines AUDIO_SERVICE = "audio" and that's been stable forever.
    auto ctx = QJniObject(QNativeInterface::QAndroidApplication::context());
    if (!ctx.isValid()) return {};
    QJniObject serviceName = QJniObject::fromString("audio");
    return ctx.callObjectMethod("getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;",
        serviceName.object<jstring>());
}

} // namespace
#endif // Q_OS_ANDROID

namespace bsfchat::audio_routing {

void enterVoiceMode()
{
#ifdef Q_OS_ANDROID
    if (g_inVoiceMode) return;

    QJniObject am = audioManager();
    if (!am.isValid()) {
        qWarning("[audio-routing] AudioManager lookup failed");
        return;
    }

    // Remember the previous mode so exitVoiceMode can restore it —
    // most apps leave MODE_NORMAL, but a call-in-progress case
    // might leave MODE_IN_CALL that we shouldn't stomp.
    g_savedMode = am.callMethod<jint>("getMode", "()I");

    am.callMethod<void>("setMode", "(I)V",
        jint(kAudioManagerModeInCommunication));
    am.callMethod<void>("setSpeakerphoneOn", "(Z)V", jboolean(true));

    // Kick off the foreground service so the process isn't killed
    // the moment the user switches apps. Safe to call before or
    // after audio-focus request.
    startVoiceService();

    // Request audio focus so music ducks. The 3-arg overload (stream,
    // duration hint) is the pre-O API that still works on current
    // Android; AudioFocusRequest objects are nicer but require
    // constructing builder chains over JNI.
    jint res = am.callMethod<jint>("requestAudioFocus",
        "(Landroid/media/AudioManager$OnAudioFocusChangeListener;II)I",
        nullptr, jint(kAudioManagerStreamVoiceCall),
        jint(kAudioFocusGain));
    if (res != kAudioFocusRequestGranted) {
        qWarning("[audio-routing] requestAudioFocus -> %d (not granted)",
                 int(res));
    } else {
        qInfo("[audio-routing] entered voice mode "
              "(speakerphone on, focus granted)");
    }
    g_inVoiceMode = true;
#endif
}

void exitVoiceMode()
{
#ifdef Q_OS_ANDROID
    if (!g_inVoiceMode) return;

    QJniObject am = audioManager();
    if (!am.isValid()) return;

    am.callMethod<void>("setSpeakerphoneOn", "(Z)V", jboolean(false));
    am.callMethod<void>("setMode", "(I)V", jint(g_savedMode));
    am.callMethod<jint>("abandonAudioFocus",
        "(Landroid/media/AudioManager$OnAudioFocusChangeListener;)I",
        nullptr);

    // Drop the foreground notification + release the service anchor.
    stopVoiceService();

    qInfo("[audio-routing] exited voice mode (restored mode=%d)", g_savedMode);
    g_inVoiceMode = false;
#endif
}

} // namespace bsfchat::audio_routing
