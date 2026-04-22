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

// macOS-specific screen capturer using CGDisplayCreateImage. Bypasses
// Qt's QScreenCapture entirely because Homebrew's Qt Multimedia is
// built without QT_FEATURE_screen_capture — so QScreenCapture fails
// with "Capturing is not supported on this platform" regardless of
// TCC state. CGDisplayCreateImage is lower-level and works as long
// as the app has Screen Recording permission.
//
// macOS 14.4 deprecated CGDisplayCreateImage in favour of
// ScreenCaptureKit, but it still functions. Swap later if needed.
class MacScreenCapturer : public QObject {
    Q_OBJECT
public:
    explicit MacScreenCapturer(QObject* parent = nullptr);
    ~MacScreenCapturer();

    // Start periodic grabs at ~`fps`. Each tick fires frameReady.
    // `displayID` 0 ⇒ main display. Used when the user hasn't picked
    // a specific source via showPicker().
    void start(uint32_t displayID = 0, int fps = 5);
    void stop();
    bool isActive() const { return m_active; }

    // Open macOS's native window/screen picker (SCContentSharingPicker,
    // macOS 14+). Once the user selects something, the capturer stores
    // the SCContentFilter and auto-starts capture using it. Same native
    // UI Zoom / Discord / Teams use.
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

private:
    void grabWithFilter();

    QTimer* m_timer = nullptr;
    uint32_t m_displayID = 0;
    bool m_active = false;
    int m_fps = 5;
    // Currently-selected filter (opaque to the header). Nullptr ⇒
    // use legacy "primary display" path.
    SCContentFilter* m_filter = nullptr;
    MacPickerObserver* m_observer = nullptr;
};
