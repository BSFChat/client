#pragma once

#include <QString>

// When the camera permission prompt is allowed to appear, and what to do
// with each answer.
//
// The sibling of voice::micPermissionAction in VoiceStartPolicy.h, and
// the same reason for existing: a unit test cannot answer an OS prompt,
// so the RULE is separated from the query and tested on its own.
//
// It matters more here than it does for the microphone because of WHEN
// the prompt fires. The camera prompt must appear at first video use and
// never at launch:
//
//   * Apple review guideline 5.1.1 and Google's equivalent both read a
//     permission request with no user-visible reason as a violation, and
//     a prompt on the splash screen has no reason attached to it.
//   * A worker hit exactly this on Android on 2026-09-22 — a camera
//     prompt fired at startup and would have failed store review.
//   * It is also just wrong: most sessions never turn video on.
//
// The structural guarantee is that this policy is only ever consulted
// from CameraController::startForCamera(), which is only ever reached
// from the dock's camera button. Nothing on the launch path constructs a
// capture session, queries a permission, or enumerates a device — see
// CameraController::ensureCaptureSession(), which is what makes that
// true rather than merely intended.

namespace camperm {

// The OS permission state, flattened away from QPermission /
// AVAuthorizationStatus so the rule below is testable.
enum class Status {
    Granted,
    // Never asked. This is the state the prompt exists for.
    Undetermined,
    // The user said no. The OS will not ask again; only the Settings
    // app can undo it.
    Denied,
    // Parental controls / MDM. Indistinguishable from Denied for our
    // purposes, but kept separate because the message differs — there
    // is no switch for the user to flip.
    Restricted,
    // No permission backend in this build (a Qt without the platform
    // plugin, or a platform with no such concept — desktop Linux).
    // Proceed and let the capture path report its own failure.
    Unsupported,
};

enum class Action {
    // Permission is in hand (or not a concept here): start capture.
    Proceed,
    // Ask, and start capture only if the answer is yes.
    //
    // NOT "request then proceed", which is what the microphone does.
    // The microphone can start the join optimistically because a denial
    // unwinds it a moment later and the cost is a wasted join. A camera
    // that starts optimistically hands QCamera a device it cannot open,
    // which on iOS surfaces as a silent black preview with no error —
    // so the start waits for the answer.
    RequestThenStart,
    // Refuse and say where the switch is.
    Refuse,
};

inline Action action(Status status)
{
    switch (status) {
    case Status::Denied:
    case Status::Restricted:    return Action::Refuse;
    case Status::Undetermined:  return Action::RequestThenStart;
    case Status::Granted:
    case Status::Unsupported:   break;
    }
    return Action::Proceed;
}

// The message shown when `action()` says Refuse, or when the user
// answers no at the prompt. Platform-specific because the remedy is:
// macOS has a Privacy pane per capture device, iOS has a per-app page,
// and "Restricted" has no remedy the user can reach at all.
inline QString refusalMessage(Status status)
{
    if (status == Status::Restricted) {
        return QStringLiteral(
            "Camera access is restricted on this device, so BSFChat cannot "
            "turn your video on. This is set by a device-management or "
            "parental-controls profile, not by BSFChat.");
    }
#if defined(Q_OS_IOS)
    return QStringLiteral(
        "Camera access is off for BSFChat. Turn it on in Settings > "
        "BSFChat > Camera, then try again.");
#elif defined(Q_OS_MACOS)
    return QStringLiteral(
        "Camera access is denied. Grant it in System Settings > Privacy & "
        "Security > Camera, then restart BSFChat.");
#else
    return QStringLiteral(
        "Camera access is denied. Grant it in your system privacy "
        "settings, then try again.");
#endif
}

} // namespace camperm
