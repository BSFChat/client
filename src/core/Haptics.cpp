#include "Haptics.h"

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace {

#ifdef Q_OS_ANDROID
// HapticFeedbackConstants values (from android.view.HapticFeedbackConstants).
// Hard-coded because the constants table is stable and pulling them via JNI
// for every call is wasteful.
constexpr jint kLongPress  = 0;
constexpr jint kVirtualKey = 1;
constexpr jint kKeyboardTap = 3;

void performHapticFeedback(jint constant)
{
    // QtActivity's decorView is the canonical target for haptics on
    // Android. We fetch it once per call — cheap, and keeps us from
    // caching a stale reference across activity restarts.
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid()) return;
    QJniObject window = activity.callObjectMethod(
        "getWindow", "()Landroid/view/Window;");
    if (!window.isValid()) return;
    QJniObject decorView = window.callObjectMethod(
        "getDecorView", "()Landroid/view/View;");
    if (!decorView.isValid()) return;
    decorView.callMethod<jboolean>(
        "performHapticFeedback", "(I)Z", constant);
}
#endif

} // namespace

Haptics::Haptics(QObject* parent) : QObject(parent) {}

void Haptics::longPress()
{
#ifdef Q_OS_ANDROID
    performHapticFeedback(kLongPress);
#endif
}

void Haptics::tick()
{
#ifdef Q_OS_ANDROID
    performHapticFeedback(kKeyboardTap);
#endif
}
