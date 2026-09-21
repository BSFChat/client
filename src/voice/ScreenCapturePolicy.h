#pragma once

// Decision logic for MacScreenCapturer, kept free of ScreenCaptureKit,
// Objective-C and Qt so it can be unit-tested headlessly. The .mm file
// feeds it `@available(macOS 14.0, *)` and its own state; the answers
// here decide which capture path runs at all.
//
// The rule it encodes: on macOS 14+ the SCContentSharingPicker is the
// ONLY way we obtain something to capture. The picker's filter is the
// user's consent — macOS grants access to exactly that selection with
// no Screen Recording TCC grant involved — so we never ask for the
// legacy blanket grant and never build a filter of our own.
//
// Incident (0.0.44, macOS 26.3): the owner was prompted for Screen
// Recording again and again although BSFChat was already allowed.
// MacScreenCapturer had two ways of inviting that, both removed:
//   * showPicker() called CGRequestScreenCaptureAccess() whenever
//     CGPreflightScreenCaptureAccess() was false. For a picker-based
//     app that preflight is allowed to be false — the picker needs no
//     grant — so the request could fire on the first share of every
//     launch. This was the reachable one: the dock button on macOS
//     only ever calls showPicker().
//   * a "no filter" fallback built SCContentFilter initWithDisplay:
//     from SCShareableContent, i.e. captured a whole display WITHOUT
//     the picker. Since macOS 15 that is the "bypass the private
//     window picker" access the OS re-confirms periodically even after
//     the user allowed it; replayd's exemption from that alert for an
//     ordinary app is a filter that came from the picker (its log
//     line: "skip alert for contentFilter from picker"). Latent in
//     0.0.44 — only start() reached it, and no macOS UI calls start()
//     — but one caller away from being the periodic prompt.
// Neither may come back on the picker path. If there is no picker
// filter, the answer is to show the picker again, never to capture
// something the user did not pick.
//
// How we know the picker needs no grant: replayd (macOS 26.3) decides
// capture access in -[RPClient hasScreenCaptureAccessWithAuditToken:
// fetchCurrentProcess:currentProcessShareableContentFilter:
// contentPickerFilter:error:]. Disassembled, it preflights TCC without
// prompting, then returns YES for a picker filter whatever that says
// (bar one TCC result it turns into an error); only a NON-picker
// filter goes on to a TCC check with kTCCAccessCheckOptionPrompt set —
// the prompt.

namespace screencap {

// What a share request (the dock button, or the legacy start() entry
// point) should do.
enum class StartRoute {
    PresentPicker,  // macOS 14+: let the user choose via the system UI.
    Unsupported,    // No picker: there is no capture path, fail clearly.
};

// Pre-14 has no SCContentSharingPicker and no SCScreenshotManager (both
// API_AVAILABLE(macos(14.0)) in the SDK headers), so there is nothing
// to capture WITH — the old "fallback" built its display filter and
// then handed it to SCScreenshotManager inside an @available(14) block,
// i.e. it never ran below 14 either. The shipped binary's LC_BUILD_VERSION
// minos is 14.0, so this branch exists for honesty, not for users.
constexpr StartRoute routeStart(bool pickerAvailable)
{
    return pickerAvailable ? StartRoute::PresentPicker
                           : StartRoute::Unsupported;
}

// What one capture tick should do.
enum class TickRoute {
    CaptureWithPickerFilter,  // The only way a frame is ever grabbed.
    Skip,                     // No selection yet/any more — grab nothing.
    Unsupported,
};

constexpr TickRoute routeTick(bool pickerAvailable, bool haveFilter)
{
    if (!pickerAvailable) return TickRoute::Unsupported;
    return haveFilter ? TickRoute::CaptureWithPickerFilter
                      : TickRoute::Skip;
}

// SCStreamErrorCode values (SCError.h). Mirrored as plain ints so this
// header needs no ScreenCaptureKit import.
namespace sc_error {
constexpr long UserDeclined     = -3801;
constexpr long NoWindowList     = -3813;
constexpr long NoDisplayList    = -3814;
constexpr long NoCaptureSource  = -3815;
constexpr long UserStopped      = -3817;
constexpr long SystemStopped    = -3821;  // macOS 15+
}

// How a failed picker-filter capture is handled.
enum class FailureKind {
    // The selection is gone for good: the user stopped sharing from the
    // menu-bar control, the system stopped it, or the picked window /
    // display no longer exists. Retrying the same filter cannot succeed;
    // end the share now. The user re-picks by sharing again.
    SelectionEnded,
    // Anything else (including UserDeclined, whose meaning on the picker
    // path is not documented): retry, and after a run of consecutive
    // failures end the share and say why. Never escalates to a legacy
    // grant request or a non-picker capture.
    Retry,
};

constexpr FailureKind classifyFailure(bool isScreenCaptureKitError, long code)
{
    if (!isScreenCaptureKitError) return FailureKind::Retry;
    switch (code) {
    case sc_error::UserStopped:
    case sc_error::SystemStopped:
    case sc_error::NoCaptureSource:
    case sc_error::NoWindowList:
    case sc_error::NoDisplayList:
        return FailureKind::SelectionEnded;
    default:
        return FailureKind::Retry;
    }
}

enum class FailureAction {
    KeepTrying,     // Next tick grabs again with the same filter.
    EndShare,       // Stop capture and tell the controller the share ended.
    ReportFailure,  // Threshold reached: surface captureFailed once.
};

// `consecutive` counts this failure. `alreadyReported` latches per
// share attempt so ReportFailure fires once, not every tick after.
constexpr FailureAction onCaptureFailure(FailureKind kind, int consecutive,
                                         int threshold, bool alreadyReported)
{
    if (kind == FailureKind::SelectionEnded) return FailureAction::EndShare;
    if (consecutive >= threshold && !alreadyReported)
        return FailureAction::ReportFailure;
    return FailureAction::KeepTrying;
}

// The toast for a SelectionEnded failure; empty means end silently.
// A stop the user made from the menu bar is not news to them. The
// others point at the one recovery that works: share again, i.e. pick.
constexpr const char* selectionEndedMessage(long code)
{
    switch (code) {
    case sc_error::UserStopped:
        return "";
    case sc_error::SystemStopped:
        return "macOS stopped the screen share. Share again to pick "
               "what to show.";
    default:
        return "The shared window or display is no longer available. "
               "Share again to pick something else.";
    }
}

}  // namespace screencap
