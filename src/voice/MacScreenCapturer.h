#pragma once

#include <QObject>
#include <QImage>
#include <QTimer>

#ifdef __OBJC__
@class SCContentFilter;
@class SCStream;
@class MacPickerObserver;
@class MacStreamSink;
#else
typedef struct objc_object SCContentFilter;
typedef struct objc_object SCStream;
typedef struct objc_object MacPickerObserver;
typedef struct objc_object MacStreamSink;
#endif

// macOS screen capturer on ScreenCaptureKit. Bypasses Qt's
// QScreenCapture entirely because Homebrew's Qt Multimedia is built
// without QT_FEATURE_screen_capture — so QScreenCapture fails with
// "Capturing is not supported on this platform" regardless of TCC
// state.
//
// What gets captured is chosen ONLY through SCContentSharingPicker
// (macOS 14+), and captured with ONE long-lived SCStream built from the
// filter the picker hands back. That filter is the user's consent: no Screen
// Recording TCC grant is requested or needed. See ScreenCapturePolicy.h
// for why the legacy grant request and the display-wide fallback were
// removed, and must stay removed.
//
// It used to poll +[SCScreenshotManager captureImageWithFilter:] once
// per tick. Every call is a kTCCServiceScreenCapture check through
// replayd — ~13,500 of them in 15 minutes at 5 fps (2026-10-16) — which
// froze the owner's Mac for seconds at a time and periodically re-raised
// the screen-recording prompt. A stream checks once, at start.
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

    // Update the capture rate. Safe to call while running — the live
    // stream's minimumFrameInterval is updated in place. Also used by
    // the picker-selection path to pick up the latest preset.
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

public:
    // Plain methods, NOT signals: moc emits a body for every signal, so a
    // declaration under `signals:` with a definition in the .mm is a
    // duplicate symbol at link time.
    // Internal, Qt thread: called by MacStreamSink (after its hop to
    // the main queue and QPointer check).
    void _onStreamFrame(quint64 streamGen, const QImage& frame);
    // The stream with generation `streamGen` failed to start, failed to
    // take a filter/config update, or stopped with an error.
    void _onStreamFailed(quint64 streamGen, quint64 selection,
                         bool isScreenCaptureKitError, long code,
                         const QString& desc);
    void _onFilterApplied(quint64 streamGen, quint64 selection);

private:
    // Build and start the SCStream for m_filter (no-op without one).
    void startStream();
    // Stop the live stream, if any, and forget it. `wait` blocks until
    // stopCaptureWithCompletionHandler has run (bounded) — the
    // destructor's path, so no stream callback outlives a teardown
    // still in progress.
    void teardownStream(bool wait);
    void stopImpl(bool waitForStream);
    void endShare(const QString& message);

    bool m_active = false;
    int m_fps = 5;
    // Live stream + its output/delegate object, or nullptr. Both are
    // retained here; teardownStream() hands them to the stop
    // completion, which releases them once the stream has stopped.
    SCStream* m_stream = nullptr;
    MacStreamSink* m_sink = nullptr;
    // Bumped for every stream started and every teardown. Callbacks
    // carry the generation they were issued under; a stale one (a
    // straggler frame or error from a stream we already dropped) is
    // ignored rather than ending or feeding the current share.
    quint64 m_streamGen = 0;
    // The m_selection whose filter the live stream is known to be
    // running. Differs from m_selection while a picker update is in
    // flight — a "window gone" error then belongs to the old pick.
    quint64 m_appliedSelection = 0;
    // SCStream delivers a frame only when the content changes. A still
    // screen would otherwise send nothing at all, so a viewer who joins
    // (or asks for a keyframe) would wait for the next change; re-send
    // the last frame at a low rate instead.
    QTimer* m_heartbeat = nullptr;
    QImage m_lastFrame;
    qint64 m_lastFrameMs = 0;
    // Consecutive stream failures (start or stop-with-error) since the
    // last delivered frame. A retryable failure restarts the stream —
    // each restart is one more TCC check, so it is bounded: after
    // kMaxConsecutiveFails we emit captureFailed once so the UI can
    // explain instead of showing a black nothing.
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
    // nothing picked: no stream is started.
    SCContentFilter* m_filter = nullptr;
    MacPickerObserver* m_observer = nullptr;
};
