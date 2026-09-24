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

// ---------------------------------------------------------------------
// Refusal messages
// ---------------------------------------------------------------------
// A refusal must tell the user how to undo it. A dead end is not
// hypothetical: the macOS prompt-denied path used to say, in full,
// "Camera access denied."
//
// The remedy differs per platform, so the message does — and that is
// exactly why the platform is a PARAMETER here rather than a #if inside
// each function. Every build can then check every platform's wording,
// instead of only its own. That is not theoretical tidiness: the first
// version of this hard-coded the iOS remedy everywhere, passed on macOS,
// and failed the Linux and Windows CI jobs, which is the only reason
// anyone found out.
enum class Platform {
    IOS,
    MacOS,
    Android,
    Windows,
    // Desktop Linux and anything else with no per-app camera permission.
    OtherDesktop,
};

constexpr Platform hostPlatform()
{
#if defined(Q_OS_IOS)
    return Platform::IOS;
#elif defined(Q_OS_MACOS)
    return Platform::MacOS;
#elif defined(Q_OS_ANDROID)
    return Platform::Android;
#elif defined(Q_OS_WIN)
    return Platform::Windows;
#else
    return Platform::OtherDesktop;
#endif
}

// Where `p` lets a user change a camera decision, spelled the way that
// platform spells it — or empty where no such place exists.
inline QString settingsPathFor(Platform p)
{
    switch (p) {
    case Platform::IOS:
        return QStringLiteral("Settings > BSFChat > Camera");
    case Platform::MacOS:
        return QStringLiteral("System Settings > Privacy & Security > Camera");
    case Platform::Android:
        // Not reached today — Android answers Unsupported here and asks
        // through androidPerms in VoiceDock — but if it ever is, this is
        // where the user goes.
        return QStringLiteral("Settings > Apps > BSFChat > Permissions");
    case Platform::Windows:
        // Windows has a real per-app camera switch and it is worth
        // naming. It used to fall into the generic branch and say "your
        // system privacy settings", which describes a place instead of
        // being one.
        return QStringLiteral("Settings > Privacy & security > Camera");
    case Platform::OtherDesktop:
        // Honest, not a gap: desktop Linux has no per-app camera
        // permission at all, which is also why cameraPermission()
        // reports Unsupported there and this is unreachable via
        // action(). Inventing a settings pane would send the user after
        // a switch that does not exist.
        break;
    }
    return QString();
}

inline QString settingsPath() { return settingsPathFor(hostPlatform()); }

inline QString refusalMessageFor(Status status, Platform p)
{
    if (status == Status::Restricted) {
        return QStringLiteral(
            "Camera access is restricted on this device, so BSFChat cannot "
            "turn your video on. This is set by a device-management or "
            "parental-controls profile, not by BSFChat.");
    }
    const QString where = settingsPathFor(p);
    if (where.isEmpty()) {
        return QStringLiteral(
            "BSFChat cannot open the camera. Check that no other "
            "application is using it, and that your user account has "
            "permission to access the camera device.");
    }
    // macOS-specific and not padding: flipping a TCC switch for an
    // already-running process does not take effect until it restarts —
    // macOS itself offers to quit the app when you do it. "Try again"
    // would send the user straight back to the same refusal.
    const QString then = p == Platform::MacOS
        ? QStringLiteral("then restart BSFChat")
        : QStringLiteral("then try again");
    return QStringLiteral("Camera access is off for BSFChat. Turn it on in "
                          "%1, %2.").arg(where, then);
}

// The message shown when `action()` says Refuse, or when the user
// answers no at the prompt.
inline QString refusalMessage(Status status)
{
    return refusalMessageFor(status, hostPlatform());
}

} // namespace camperm
