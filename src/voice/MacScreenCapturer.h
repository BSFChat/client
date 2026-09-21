#pragma once

#include <QObject>
#include <QImage>
#include <QTimer>

#ifdef __OBJC__
@class SCContentFilter;
@class MacPickerObserver;
#else
typedef struct objc_object SCContentFilter;
typedef struct objc_object MacPickerObserver;
#endif

// macOS screen capturer on ScreenCaptureKit. Bypasses Qt's
// QScreenCapture entirely because Homebrew's Qt Multimedia is built
// without QT_FEATURE_screen_capture — so QScreenCapture fails with
// "Capturing is not supported on this platform" regardless of TCC
// state.
//
// What gets captured is chosen ONLY through SCContentSharingPicker
// (macOS 14+), and captured with SCScreenshotManager using the filter
// the picker hands back. That filter is the user's consent: no Screen
// Recording TCC grant is requested or needed. See ScreenCapturePolicy.h
// for why the legacy grant request and the display-wide fallback were
// removed, and must stay removed.
class MacScreenCapturer : public QObject {
    Q_OBJECT
public:
    explicit MacScreenCapturer(QObject* parent = nullptr);
    ~MacScreenCapturer();

    // Kept for ScreenShareController::startForScreen. There is no
    // "capture this display" path any more: this records `fps` and
    // presents the picker, exactly like showPicker(). `displayID` is
    // ignored — capturing a display the user did not pick is the
    // private-window-picker bypass macOS keeps re-confirming.
    void start(uint32_t displayID = 0, int fps = 5);
    void stop();
    bool isActive() const { return m_active; }

    // Open macOS's native window/screen picker (SCContentSharingPicker,
    // macOS 14+). Once the user selects something, the capturer stores
    // the SCContentFilter and auto-starts capture using it. Same native
    // UI Zoom / Discord / Teams use. Never asks for a TCC grant.
    void showPicker();

    // Update the capture rate. Safe to call while running — the
    // QTimer's interval is adjusted in place. Also used by the
    // picker-selection path to pick up the latest preset.
    void setFps(int fps);

    // Internal: called by MacPickerObserver when the picker returns a
    // selection. `filter` is an SCContentFilter* (retained by caller).
    void _onPickerSelection(SCContentFilter* filter);
    void _onPickerCancel();

signals:
    void frameReady(const QImage& frame);
    void captureFailed(const QString& description);
    // Fired when the user picks a source via showPicker(). QML can
    // reflect the source in a "Sharing: <name>" label, etc.
    void sourceSelected();
    void pickerCancelled();
    // The share is over and capture has already stopped: the picked
    // window/display went away, the user stopped sharing from the
    // menu-bar control, or there is no picker on this OS. `message` is
    // for the user (empty ⇒ nothing worth saying). Recovery is to share
    // again, which re-offers the picker — never a wider capture.
    void shareEnded(const QString& message);

private:
    void grabWithFilter();
    // Picker-path failure handling, on the Qt thread. `selection` is the
    // m_selection value the failed capture was issued under.
    void handleCaptureFailure(quint64 selection, bool isScreenCaptureKitError,
                              long code, const QString& desc);
    void endShare(const QString& message);

    QTimer* m_timer = nullptr;
    bool m_active = false;
    int m_fps = 5;
    // Consecutive captureImage failures. SCScreenshotManager fails
    // per-call (e.g. "user declined TCC" when a stale grant no longer
    // matches the binary's signature) rather than erroring the stream,
    // so without a threshold the capturer retries silently forever.
    // After kMaxConsecutiveFails we emit captureFailed once so the UI
    // can explain instead of showing a black nothing.
    static constexpr int kMaxConsecutiveFails = 5;
    int m_consecutiveFails = 0;
    bool m_failureNotified = false;
    // Bumped whenever the filter is replaced or dropped. A capture still
    // in flight from the previous selection must not end the new one —
    // e.g. the user re-picks because the old window is closing.
    quint64 m_selection = 0;
    // True between showPicker() and the resulting selection/cancel —
    // outside a live capture, the only window in which a picker
    // filter may start one.
    bool m_pickerPending = false;
    // Currently-selected filter (opaque to the header). Nullptr ⇒
    // nothing picked: capture ticks grab nothing.
    SCContentFilter* m_filter = nullptr;
    MacPickerObserver* m_observer = nullptr;
};
