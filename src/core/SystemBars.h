// Status and navigation bar appearance from the app's own theme.
//
// The app picks dark or light itself (Settings::theme, default dark) and
// ignores the system appearance, so the system bars, which follow the
// SYSTEM appearance by default, could end up dark icons on the dark header
// or light icons on a light one. This keeps them readable:
//
//   * Android: the bar icon appearance (WindowInsetsController on API 30+,
//     the View light-bar flags before that) and, where the platform still
//     paints them (API < 35, i.e. not edge-to-edge), the bar colours, set
//     from the theme in android/src/com/bsfchat/client/SystemBars.java.
//   * iOS: overrideUserInterfaceStyle on the app's windows, which the
//     default status bar style follows (SystemBarsIos.mm). It also makes
//     the keyboard and other system UI match the theme.
//
// Applied at startup, on every theme change, and again whenever the app
// becomes active, since a recreated window or Qt itself can reset them.
// No-op elsewhere.
#pragma once

#include <QObject>

class Settings;

class SystemBars : public QObject {
    Q_OBJECT
public:
    explicit SystemBars(Settings* settings, QObject* parent = nullptr);

    // Re-read the theme and apply it to the bars. Safe to call any time
    // after the QGuiApplication exists.
    void apply();

private:
    Settings* m_settings = nullptr;
};

#ifdef Q_OS_IOS
namespace bsfchat {
// SystemBarsIos.mm. dark=true asks for light status bar content.
void iosSetInterfaceStyle(bool dark);
}
#endif
