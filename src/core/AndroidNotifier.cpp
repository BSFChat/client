#include "AndroidNotifier.h"

#include <QtGlobal>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QDebug>
#include <QJniEnvironment>
#include <QJniObject>

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

AndroidNotifier::AndroidNotifier(QObject* parent) : QObject(parent) {}

void AndroidNotifier::startSyncService()
{
#ifdef Q_OS_ANDROID
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
    qInfo("[notifier] SyncService started");
#endif
}

void AndroidNotifier::stopSyncService()
{
#ifdef Q_OS_ANDROID
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
