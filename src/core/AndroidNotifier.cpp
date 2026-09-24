#include "AndroidNotifier.h"

#include <QtGlobal>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QPointer>

namespace {

constexpr const char* kChannelId = "bsfchat_chat";
constexpr const char* kChannelName = "Chat messages";

// Notification importance levels (android.app.NotificationManager):
// DEFAULT = 3 (sound, but no heads-up); HIGH = 4 (heads-up).
constexpr jint kImportanceHigh = 4;

// PendingIntent flags — platform constants.
constexpr jint kPiUpdateCurrent = 0x08000000; // FLAG_UPDATE_CURRENT
constexpr jint kPiImmutable     = 0x04000000; // FLAG_IMMUTABLE (API 23+)

QJniObject appContext()
{
    return QJniObject(QNativeInterface::QAndroidApplication::context());
}

// The single notifier, so SyncService's stop callback can reach it.
QPointer<AndroidNotifier> g_instance;

// Android told us background sync is over. The one case that matters is
// the Android 15 dataSync budget: SyncService.onTimeout fires, the service
// stops itself, and nothing else would ever tell the app that its
// background delivery is gone.
extern "C" JNIEXPORT void JNICALL
nativeOnSyncServiceStopped(JNIEnv* env, jclass, jstring reasonJ)
{
    auto* inst = g_instance.data();
    if (!inst) return;
    QString reason;
    if (reasonJ) {
        const char* raw = env->GetStringUTFChars(reasonJ, nullptr);
        if (raw) {
            reason = QString::fromUtf8(raw);
            env->ReleaseStringUTFChars(reasonJ, raw);
        }
    }
    QMetaObject::invokeMethod(inst, [inst, reason]() {
        inst->onSyncServiceStopped(reason);
    }, Qt::QueuedConnection);
}

void registerSyncNatives()
{
    static bool registered = false;
    if (registered) return;
    JNINativeMethod methods[] = {
        { const_cast<char*>("nativeOnSyncServiceStopped"),
          const_cast<char*>("(Ljava/lang/String;)V"),
          reinterpret_cast<void*>(nativeOnSyncServiceStopped) },
    };
    QJniEnvironment env;
    if (env.registerNativeMethods("com/bsfchat/client/SyncService",
                                  methods, 1)) {
        registered = true;
    } else {
        qWarning("[notifier] could not register the SyncService stop bridge; "
                 "an Android 15 dataSync timeout will go unnoticed");
    }
}

QJniObject notificationManager()
{
    QJniObject ctx = appContext();
    if (!ctx.isValid()) return {};
    QJniObject name = QJniObject::fromString("notification");
    return ctx.callObjectMethod("getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;",
        name.object<jstring>());
}

void ensureChatChannel()
{
    if (QNativeInterface::QAndroidApplication::sdkVersion() < 26) return;

    QJniObject nm = notificationManager();
    if (!nm.isValid()) return;

    QJniObject id = QJniObject::fromString(kChannelId);
    QJniObject existing = nm.callObjectMethod(
        "getNotificationChannel",
        "(Ljava/lang/String;)Landroid/app/NotificationChannel;",
        id.object<jstring>());
    if (existing.isValid()) return;

    QJniObject name = QJniObject::fromString(kChannelName);
    QJniObject channel("android/app/NotificationChannel",
        "(Ljava/lang/String;Ljava/lang/CharSequence;I)V",
        id.object<jstring>(),
        name.object<jstring>(),
        kImportanceHigh);
    if (!channel.isValid()) return;

    QJniObject desc = QJniObject::fromString(
        "Notifications for new chat messages.");
    channel.callMethod<void>("setDescription",
        "(Ljava/lang/String;)V", desc.object<jstring>());
    channel.callMethod<void>("setShowBadge", "(Z)V", jboolean(true));

    nm.callMethod<void>("createNotificationChannel",
        "(Landroid/app/NotificationChannel;)V", channel.object());
}

// Construct a PendingIntent that re-launches the main activity with
// the given bsfchat:// URL as its data — UrlHandler / main.cpp
// pick it up via argv or the macOS/Android URL handler path.
QJniObject buildTapIntent(const QString& deepLink)
{
    QJniObject ctx = appContext();
    if (!ctx.isValid()) return {};

    QJniObject intent("android/content/Intent",
        "(Ljava/lang/String;)V",
        QJniObject::fromString("android.intent.action.VIEW")
            .object<jstring>());
    if (!intent.isValid()) return {};

    // Set the BSFChatActivity as target so the intent re-enters the
    // running process (singleTop) and onNewIntent fires.
    QJniEnvironment env;
    jclass act = env->FindClass("com/bsfchat/client/BSFChatActivity");
    if (!act) { env->ExceptionClear(); return {}; }
    QJniObject classRef(reinterpret_cast<jobject>(act));
    intent.callObjectMethod("setClass",
        "(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;",
        ctx.object(), classRef.object());
    env->DeleteLocalRef(act);

    // Attach the URL as EXTRA so the Qt-side handler reads it; we
    // also set the data URI for any OS-level routing.
    if (!deepLink.isEmpty()) {
        QJniObject uriClass("android/net/Uri");
        QJniObject uri = QJniObject::callStaticObjectMethod(
            "android/net/Uri", "parse",
            "(Ljava/lang/String;)Landroid/net/Uri;",
            QJniObject::fromString(deepLink).object<jstring>());
        if (uri.isValid()) {
            intent.callObjectMethod("setData",
                "(Landroid/net/Uri;)Landroid/content/Intent;",
                uri.object());
        }
    }

    // FLAG_ACTIVITY_SINGLE_TOP | FLAG_ACTIVITY_CLEAR_TOP so a tapped
    // notification re-uses the existing task.
    intent.callObjectMethod("setFlags",
        "(I)Landroid/content/Intent;",
        jint(0x20000000 | 0x04000000)); // SINGLE_TOP | CLEAR_TOP

    int flags = kPiUpdateCurrent;
    if (QNativeInterface::QAndroidApplication::sdkVersion() >= 23) {
        flags |= kPiImmutable;
    }

    // PendingIntent.getActivity(context, requestCode, intent, flags)
    // Request code doesn't matter as long as it's unique per tag —
    // we use the deep-link's hash so different rooms don't collapse.
    int reqCode = qHash(deepLink) & 0x7FFFFFFF;

    QJniObject pi = QJniObject::callStaticObjectMethod(
        "android/app/PendingIntent",
        "getActivity",
        "(Landroid/content/Context;ILandroid/content/Intent;I)"
        "Landroid/app/PendingIntent;",
        ctx.object(), jint(reqCode), intent.object(), jint(flags));
    return pi;
}

} // namespace
#endif // Q_OS_ANDROID

AndroidNotifier::AndroidNotifier(QObject* parent) : QObject(parent)
{
#ifdef Q_OS_ANDROID
    g_instance = this;
    registerSyncNatives();

    // The only moment at which starting a foreground service is
    // unconditionally legal is while the app is in the foreground, so every
    // deferred or failed start is retried from here.
    if (auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(gui, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive) return;
                // One attempt per foreground after a budget stop. If the
                // 24h window has not rolled over yet the start is refused,
                // SyncService reports it, and we go quiet again until the
                // next time the user opens the app.
                m_budgetExhausted = false;
                tryStartSyncService();
            });
    }
#endif
}

void AndroidNotifier::startSyncService()
{
    m_syncWanted = true;
    tryStartSyncService();
}

void AndroidNotifier::tryStartSyncService()
{
#ifdef Q_OS_ANDROID
    if (!m_syncWanted || m_syncRunning || m_budgetExhausted) return;

    // Android 12+ throws ForegroundServiceStartNotAllowedException out of
    // startForegroundService() when the app is not in the foreground, and
    // that exception is fatal to the process. The call site for this is a
    // ServerManager::serverAdded, which fires while connections are being
    // restored — before the activity is resumed on a cold start, and from
    // a reconnect at any time. So the state is checked, not assumed.
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
        qInfo("[notifier] deferring SyncService start: app is not in the "
              "foreground (state=%d)",
              int(QGuiApplication::applicationState()));
        emit backgroundSyncUnavailable(QStringLiteral("deferred-background"));
        return;
    }

    QJniObject ctx = appContext();
    if (!ctx.isValid()) return;

    QJniObject intent("android/content/Intent");
    if (!intent.isValid()) return;
    QJniEnvironment env;
    jclass svc = env->FindClass("com/bsfchat/client/SyncService");
    if (!svc) { env->ExceptionClear(); return; }
    QJniObject classRef(reinterpret_cast<jobject>(svc));
    intent.callObjectMethod("setClass",
        "(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;",
        ctx.object(), classRef.object());
    env->DeleteLocalRef(svc);

    if (QNativeInterface::QAndroidApplication::sdkVersion() >= 26) {
        ctx.callObjectMethod("startForegroundService",
            "(Landroid/content/Intent;)Landroid/content/ComponentName;",
            intent.object());
    } else {
        ctx.callObjectMethod("startService",
            "(Landroid/content/Intent;)Landroid/content/ComponentName;",
            intent.object());
    }
    // Belt as well as braces: QJniObject clears a pending exception after
    // each call, but it does not tell the caller, so the start would
    // otherwise look like it succeeded. This asks explicitly.
    if (env.checkAndClearExceptions()) {
        qWarning("[notifier] SyncService start was refused by the platform; "
                 "background sync stays off until the app is foregrounded "
                 "again");
        emit backgroundSyncUnavailable(QStringLiteral("start-threw"));
        return;
    }

    setSyncRunning(true);
    qInfo("[notifier] SyncService started");
#endif
}

void AndroidNotifier::stopSyncService()
{
    m_syncWanted = false;
#ifdef Q_OS_ANDROID
    setSyncRunning(false);

    QJniObject ctx = appContext();
    if (!ctx.isValid()) return;

    QJniObject intent("android/content/Intent");
    if (!intent.isValid()) return;
    QJniEnvironment env;
    jclass svc = env->FindClass("com/bsfchat/client/SyncService");
    if (!svc) { env->ExceptionClear(); return; }
    QJniObject classRef(reinterpret_cast<jobject>(svc));
    intent.callObjectMethod("setClass",
        "(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;",
        ctx.object(), classRef.object());
    env->DeleteLocalRef(svc);

    ctx.callMethod<jboolean>("stopService",
        "(Landroid/content/Intent;)Z", intent.object());
    qInfo("[notifier] SyncService stopped");
#endif
}

void AndroidNotifier::onSyncServiceStopped(const QString& reason)
{
    setSyncRunning(false);
    if (reason == QLatin1String("dataSync-budget-exhausted")) {
        // Android 15 spent our six hours. Do NOT restart: the platform
        // refuses another dataSync FGS until the 24h window rolls over, and
        // an app that keeps asking just burns battery being told no. We
        // degrade to foreground-only sync — the /sync loop is ours and keeps
        // running for as long as the process does — and try once more the
        // next time the user brings the app up. Firebase is not the answer
        // here and never will be; see the note in SyncService.java.
        m_budgetExhausted = true;
        qWarning("[notifier] Android stopped background sync: the dataSync "
                 "foreground-service budget for this 24h window is spent. "
                 "Messages will now arrive only while BSFChat is open.");
    } else {
        qWarning("[notifier] background sync stopped (%s)",
                 qUtf8Printable(reason));
    }
    emit backgroundSyncUnavailable(reason);
}

void AndroidNotifier::setSyncRunning(bool running)
{
    if (m_syncRunning == running) return;
    m_syncRunning = running;
    emit backgroundSyncActiveChanged();
}

void AndroidNotifier::postChatNotification(const QString& tag,
                                           const QString& title,
                                           const QString& body,
                                           const QString& tapDeepLink,
                                           const QString& groupKey)
{
#ifdef Q_OS_ANDROID
    ensureChatChannel();

    QJniObject ctx = appContext();
    if (!ctx.isValid()) return;
    QJniObject nm = notificationManager();
    if (!nm.isValid()) return;

    QJniObject builder;
    if (QNativeInterface::QAndroidApplication::sdkVersion() >= 26) {
        builder = QJniObject("android/app/Notification$Builder",
            "(Landroid/content/Context;Ljava/lang/String;)V",
            ctx.object(),
            QJniObject::fromString(kChannelId).object<jstring>());
    } else {
        builder = QJniObject("android/app/Notification$Builder",
            "(Landroid/content/Context;)V", ctx.object());
    }
    if (!builder.isValid()) return;

    // Chainable setters — each returns the builder; we ignore the
    // returned value and reuse `builder` (which is the same ref).
    builder.callObjectMethod("setContentTitle",
        "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;",
        QJniObject::fromString(title).object<jstring>());
    builder.callObjectMethod("setContentText",
        "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;",
        QJniObject::fromString(body).object<jstring>());
    builder.callObjectMethod("setAutoCancel",
        "(Z)Landroid/app/Notification$Builder;", jboolean(true));
    builder.callObjectMethod("setSmallIcon",
        "(I)Landroid/app/Notification$Builder;",
        jint(0x01080088)); // android.R.drawable.sym_action_chat

    QJniObject pi = buildTapIntent(tapDeepLink);
    if (pi.isValid()) {
        builder.callObjectMethod("setContentIntent",
            "(Landroid/app/PendingIntent;)Landroid/app/Notification$Builder;",
            pi.object());
    }

    if (!groupKey.isEmpty()) {
        builder.callObjectMethod("setGroup",
            "(Ljava/lang/String;)Landroid/app/Notification$Builder;",
            QJniObject::fromString(groupKey).object<jstring>());
    }

    QJniObject notification = builder.callObjectMethod(
        "build", "()Landroid/app/Notification;");
    if (!notification.isValid()) return;

    // notify(tag, id, notification) — tag differentiates per-room
    // streams; id=1 across the board so a single notification per
    // tag replaces previous.
    nm.callMethod<void>("notify",
        "(Ljava/lang/String;ILandroid/app/Notification;)V",
        QJniObject::fromString(tag).object<jstring>(),
        jint(1), notification.object());

    // Summary notification — required by Android for grouped
    // notifications to actually render as a group in the shade.
    // Without this, grouped notifications show flat. Setting a
    // summary (any Notification.Builder with setGroup + setGroupSummary)
    // tells the platform "these belong together"; the user sees
    // a collapsible stack. We re-post the summary on every
    // post — cheap, and ensures it stays alive across system
    // restarts.
    if (!groupKey.isEmpty()) {
        QJniObject sb;
        if (QNativeInterface::QAndroidApplication::sdkVersion() >= 26) {
            sb = QJniObject("android/app/Notification$Builder",
                "(Landroid/content/Context;Ljava/lang/String;)V",
                ctx.object(),
                QJniObject::fromString(kChannelId).object<jstring>());
        } else {
            sb = QJniObject("android/app/Notification$Builder",
                "(Landroid/content/Context;)V", ctx.object());
        }
        if (sb.isValid()) {
            sb.callObjectMethod("setContentTitle",
                "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;",
                QJniObject::fromString(title).object<jstring>());
            sb.callObjectMethod("setSmallIcon",
                "(I)Landroid/app/Notification$Builder;",
                jint(0x01080088));
            sb.callObjectMethod("setGroup",
                "(Ljava/lang/String;)Landroid/app/Notification$Builder;",
                QJniObject::fromString(groupKey).object<jstring>());
            sb.callObjectMethod("setGroupSummary",
                "(Z)Landroid/app/Notification$Builder;",
                jboolean(true));
            QJniObject summary = sb.callObjectMethod(
                "build", "()Landroid/app/Notification;");
            if (summary.isValid()) {
                // Summary uses the same tag as the group so
                // cancelByTag sweeps both. IDs must be unique
                // within a tag — use 2 for summary, 1 for leaf.
                nm.callMethod<void>("notify",
                    "(Ljava/lang/String;ILandroid/app/Notification;)V",
                    QJniObject::fromString(groupKey).object<jstring>(),
                    jint(2), summary.object());
            }
        }
    }
#else
    Q_UNUSED(tag);
    Q_UNUSED(title);
    Q_UNUSED(body);
    Q_UNUSED(tapDeepLink);
    Q_UNUSED(groupKey);
#endif
}

void AndroidNotifier::cancelByTag(const QString& tag)
{
#ifdef Q_OS_ANDROID
    QJniObject nm = notificationManager();
    if (!nm.isValid()) return;
    nm.callMethod<void>("cancel",
        "(Ljava/lang/String;I)V",
        QJniObject::fromString(tag).object<jstring>(), jint(1));
#else
    Q_UNUSED(tag);
#endif
}

void AndroidNotifier::cancelAll()
{
#ifdef Q_OS_ANDROID
    QJniObject nm = notificationManager();
    if (!nm.isValid()) return;
    nm.callMethod<void>("cancelAll", "()V");
#endif
}
