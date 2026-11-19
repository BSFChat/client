#include "core/SystemBars.h"

#include "core/Settings.h"

#include <QGuiApplication>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

namespace {

#ifdef Q_OS_ANDROID
// Theme.qml's header (bg1) and window (bg0) colours, as ARGB. Kept in step
// by hand; they only matter on API < 35, where the bars are painted rather
// than drawn over.
constexpr jint kDarkStatus  = jint(0xFF121417u);
constexpr jint kDarkNav     = jint(0xFF0B0D11u);
constexpr jint kLightStatus = jint(0xFFFBFCFEu);
constexpr jint kLightNav    = jint(0xFFF3F5F8u);
#endif

// Theme.qml: isDark = AppSettings.theme !== "light".
bool themeIsDark(const Settings* settings)
{
    return !settings || settings->theme() != QLatin1String("light");
}

} // namespace

SystemBars::SystemBars(Settings* settings, QObject* parent)
    : QObject(parent), m_settings(settings)
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    if (m_settings)
        connect(m_settings, &Settings::themeChanged, this, &SystemBars::apply);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state == Qt::ApplicationActive) apply();
            });
#endif
}

void SystemBars::apply()
{
    const bool dark = themeIsDark(m_settings);
#if defined(Q_OS_ANDROID)
    if (!QNativeInterface::QAndroidApplication::isActivityContext()) return;
    const QJniObject activity(QNativeInterface::QAndroidApplication::context());
    if (!activity.isValid()) return;
    QJniObject::callStaticMethod<void>(
        "com/bsfchat/client/SystemBars", "apply", "(Landroid/app/Activity;ZII)V",
        activity.object(), jboolean(dark),
        dark ? kDarkStatus : kLightStatus, dark ? kDarkNav : kLightNav);
#elif defined(Q_OS_IOS)
    bsfchat::iosSetInterfaceStyle(dark);
#else
    Q_UNUSED(dark);
#endif
}
