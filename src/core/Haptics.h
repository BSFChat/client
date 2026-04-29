// Lightweight haptic-feedback bridge. Exposed to QML as a
// `haptics` context property. On Android we route to
// View.performHapticFeedback(); on all other platforms the methods
// are no-ops so callers don't need to gate on Theme.isMobile in QML.
//
// Why this is its own QObject instead of inlined JNI: QML can't call
// static Java methods directly, and the JNI handle path is fiddly
// enough to want a single place to maintain it.
#pragma once

#include <QObject>

class Haptics : public QObject {
    Q_OBJECT
public:
    explicit Haptics(QObject* parent = nullptr);

    // Short confirmation buzz — the "I heard you" tap used for
    // long-press activation, swipe-commit, etc. Roughly 20ms on most
    // devices via HapticFeedbackConstants.LONG_PRESS.
    Q_INVOKABLE void longPress();

    // Even shorter tap — for incremental events like each step of a
    // swipe gesture crossing a detent. Mapped to KEYBOARD_TAP which
    // is tuned to be very quiet on Android.
    Q_INVOKABLE void tick();
};
