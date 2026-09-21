#include "voice/MacScreenCapturer.h"
#include "voice/ScreenCapturePolicy.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#include <QDebug>

// ScreenCaptureKit (macOS 12.3+) is Apple's sanctioned replacement
// for the now-obsolete CGDisplayCreateImage path. We use
// SCScreenshotManager for one-shot polling captures rather than
// SCStream because (a) we already have a 5 fps throttle, (b) no
// need to juggle a persistent stream lifecycle, and (c) the
// screenshot API is synchronous-ish from the caller's POV.
//
// For source selection we hook SCContentSharingPicker (macOS 14+) —
// the same native UI Zoom / Discord use. The picker delivers an
// SCContentFilter we retain and use as the capture target, and it is
// the ONLY capture target: this file never calls
// CGRequestScreenCaptureAccess and never builds a filter from
// SCShareableContent. Both used to be here and both invite the
// Screen Recording prompts macOS shows picker-less apps — see
// ScreenCapturePolicy.h (tests/test_screen_capture_policy.cpp guards
// the source against their return).

@interface MacPickerObserver : NSObject <SCContentSharingPickerObserver>
@property (nonatomic, assign) MacScreenCapturer *owner;
@end

@implementation MacPickerObserver
// SCContentSharingPicker invokes these delegate methods on an
// arbitrary background queue. Anything that touches Qt objects
// (QTimer, signals, QObject state) must run on the Qt/main
// thread — hop via dispatch_get_main_queue before dispatching.
- (void)contentSharingPicker:(SCContentSharingPicker *)picker
          didUpdateWithFilter:(SCContentFilter *)filter
                    forStream:(SCStream *)stream API_AVAILABLE(macos(14.0)) {
    SCContentFilter* retained = [filter retain];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (_owner) _owner->_onPickerSelection(retained);
        [retained release];
    });
}
- (void)contentSharingPicker:(SCContentSharingPicker *)picker
           didCancelForStream:(SCStream *)stream API_AVAILABLE(macos(14.0)) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (_owner) _owner->_onPickerCancel();
    });
}
- (void)contentSharingPickerStartDidFailWithError:(NSError *)error
    API_AVAILABLE(macos(14.0)) {
    NSLog(@"[mac-capture] picker start failed: %@", error);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (_owner) _owner->_onPickerCancel();
    });
}
@end

MacScreenCapturer::MacScreenCapturer(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    connect(m_timer, &QTimer::timeout, this, [this]() { grabWithFilter(); });

    if (@available(macOS 14.0, *)) {
        MacPickerObserver* obs = [[MacPickerObserver alloc] init];
        obs.owner = this;
        m_observer = obs;
        [[SCContentSharingPicker sharedPicker] addObserver:obs];
    }
}

MacScreenCapturer::~MacScreenCapturer() {
    stop();
    if (m_filter) {
        [(id)m_filter release];
        m_filter = nullptr;
    }
    if (@available(macOS 14.0, *)) {
        if (m_observer) {
            [[SCContentSharingPicker sharedPicker]
                removeObserver:(MacPickerObserver*)m_observer];
            [(id)m_observer release];
            m_observer = nullptr;
        }
    }
}

// There used to be an ensureScreenAccess() here, called from
// showPicker() and start(): CGPreflightScreenCaptureAccess(), and when
// that was false, CGRequestScreenCaptureAccess() "once per app run".
// It was added because SCScreenshotManager never prompts, on the
// theory that capture needs the legacy grant. On the picker path it
// does not: the picker's filter carries access to the selection, and a
// false preflight is a NORMAL state for an app that only uses the
// picker — so the request prompted for a permission the app never
// needed, on the first share of a launch, for an owner who had already
// allowed BSFChat (0.0.44, macOS 26.3). Do not re-add it. A picker
// capture that fails is handled in handleCaptureFailure(): end the
// share and let the user pick again.
void MacScreenCapturer::showPicker()
{
    bool pickerAvailable = false;
    if (@available(macOS 14.0, *)) pickerAvailable = true;
    switch (screencap::routeStart(pickerAvailable)) {
    case screencap::StartRoute::PresentPicker:
        if (@available(macOS 14.0, *)) {
            m_pickerPending = true;
            SCContentSharingPicker* picker = [SCContentSharingPicker sharedPicker];
            picker.active = YES;
            // Allow all source kinds (display / window / application).
            SCContentSharingPickerConfiguration* cfg =
                [[SCContentSharingPickerConfiguration alloc] init];
            cfg.allowedPickerModes = SCContentSharingPickerModeSingleDisplay
                                   | SCContentSharingPickerModeSingleWindow
                                   | SCContentSharingPickerModeMultipleWindows
                                   | SCContentSharingPickerModeSingleApplication;
            picker.defaultConfiguration = cfg;
            [cfg release];
            [picker present];
        }
        return;
    case screencap::StartRoute::Unsupported:
        // Unreachable in shipped builds (minos 14.0), and there is no
        // pre-picker capture path to fall back to: SCScreenshotManager
        // is 14+ too. Say so instead of ticking a timer that grabs
        // nothing, which is what the old "primary display" fallback did.
        qWarning("[mac-capture] screen sharing requires macOS 14+");
        emit shareEnded(QStringLiteral(
            "Screen sharing needs macOS 14 (Sonoma) or later."));
        return;
    }
}

void MacScreenCapturer::_onPickerSelection(SCContentFilter* filter)
{
    // Accept a filter only when we're capturing (user changed the
    // shared window mid-share) or expecting one (showPicker pending).
    // The shared system picker re-delivers filters at other times —
    // e.g. a stale delivery racing a stop() — and acting on those
    // would silently restart a share the user just ended.
    if (!m_active && !m_pickerPending) {
        qInfo("[mac-capture] ignoring unsolicited picker filter");
        return;
    }
    m_pickerPending = false;
    // Replace prior filter (if any) with the newly-chosen one.
    if (m_filter) {
        [(id)m_filter release];
        m_filter = nullptr;
    }
    m_filter = (SCContentFilter*)[(id)filter retain];
    ++m_selection;
    qInfo("[mac-capture] picker returned a filter; starting capture");
    // Fresh share attempt — re-arm the failure notifier so a capture
    // problem surfaces again (it latches per attempt, not per app run).
    m_consecutiveFails = 0;
    m_failureNotified = false;
    // Start polling immediately.
    if (!m_active) {
        m_timer->setInterval(m_fps > 0 ? (1000 / m_fps) : 200);
        m_timer->start();
        m_active = true;
    } else {
        m_timer->setInterval(m_fps > 0 ? (1000 / m_fps) : 200);
    }
    emit sourceSelected();
    grabWithFilter();
}

void MacScreenCapturer::_onPickerCancel()
{
    qInfo("[mac-capture] picker cancelled");
    m_pickerPending = false;
    emit pickerCancelled();
}

void MacScreenCapturer::start(uint32_t /*displayID*/, int fps)
{
    if (m_active) return;
    // No direct display capture: this entry point used to start a timer
    // with no filter, which made every tick build a display-wide filter
    // from SCShareableContent — capture outside the picker, the access
    // macOS 15+ periodically makes the user re-approve. Picking is the
    // only way in.
    setFps(fps);
    qInfo("[mac-capture] start(fps=%d) → picker", m_fps);
    showPicker();
}

void MacScreenCapturer::setFps(int fps)
{
    m_fps = fps > 0 ? fps : 5;
    if (m_timer) m_timer->setInterval(1000 / m_fps);
}

void MacScreenCapturer::stop()
{
    if (!m_active) return;
    m_timer->stop();
    m_active = false;
    if (@available(macOS 14.0, *)) {
        // End the system share session too. While the shared picker
        // stays active, macOS keeps the "sharing" indicator on and
        // RE-DELIVERS didUpdateWithFilter — which used to restart
        // capture right after a stop (the "can't stop streaming" bug).
        [SCContentSharingPicker sharedPicker].active = NO;
    }
    if (m_filter) {
        [(id)m_filter release];
        m_filter = nullptr;
    }
    ++m_selection;
    qInfo("[mac-capture] stop");
}


void MacScreenCapturer::endShare(const QString& message)
{
    stop();
    emit shareEnded(message);
}

void MacScreenCapturer::handleCaptureFailure(quint64 selection,
                                             bool isScreenCaptureKitError,
                                             long code, const QString& desc)
{
    if (!m_active) return;
    // A straggler from a selection the user has since replaced (or a
    // share that has since stopped) says nothing about the current one.
    if (selection != m_selection) return;
    const auto kind = screencap::classifyFailure(isScreenCaptureKitError, code);
    switch (screencap::onCaptureFailure(kind, ++m_consecutiveFails,
                                        kMaxConsecutiveFails,
                                        m_failureNotified)) {
    case screencap::FailureAction::KeepTrying:
        return;
    case screencap::FailureAction::EndShare:
        qInfo("[mac-capture] selection ended (code %ld) — ending share", code);
        endShare(QString::fromUtf8(screencap::selectionEndedMessage(code)));
        return;
    case screencap::FailureAction::ReportFailure:
        // Per-call failures (e.g. "user declined") must surface after a
        // few consecutive misses rather than retrying silently forever.
        // This reports; it does NOT go looking for a wider permission —
        // the controller ends the share and the user can pick again.
        m_failureNotified = true;
        emit captureFailed(desc);
        return;
    }
}

// The actual SCScreenshotManager capture, always with the filter the
// user picked. With no filter there is nothing to capture: the
// display-wide SCShareableContent fallback that used to live here was
// capture outside the picker (see ScreenCapturePolicy.h).
void MacScreenCapturer::grabWithFilter()
{
    static int s_tick = 0;
    ++s_tick;
    if (s_tick % 25 == 1) {
        qInfo("[mac-capture] tick #%d filter=%p active=%d",
              s_tick, (void*)m_filter, int(m_active));
    }
    bool pickerAvailable = false;
    if (@available(macOS 14.0, *)) pickerAvailable = true;
    switch (screencap::routeTick(pickerAvailable, m_filter != nullptr)) {
    case screencap::TickRoute::CaptureWithPickerFilter:
        break;
    case screencap::TickRoute::Skip:
    case screencap::TickRoute::Unsupported:
        return;
    }

    if (@available(macOS 14.0, *)) {
        const quint64 selection = m_selection;
        auto completion = ^(CGImageRef image, NSError *err) {
            if (err || !image) {
                static int s_failCount = 0;
                ++s_failCount;
                if (s_failCount <= 3 || s_failCount % 25 == 0) {
                    qWarning("[mac-capture] captureImage failed #%d: %s (%ld)",
                             s_failCount,
                             err ? err.localizedDescription.UTF8String
                                 : "nil image",
                             err ? long(err.code) : 0L);
                }
                // The completion runs off-thread; count and decide on
                // the Qt thread.
                const bool isSCK = err
                    && [err.domain isEqualToString:SCStreamErrorDomain];
                const long code = err ? long(err.code) : 0L;
                QString desc = err
                    ? QString::fromNSString(err.localizedDescription)
                    : QStringLiteral("capture returned no image");
                QMetaObject::invokeMethod(this,
                    [this, selection, isSCK, code, desc]() {
                        handleCaptureFailure(selection, isSCK, code, desc);
                    }, Qt::QueuedConnection);
                return;
            }
            static int s_okCount = 0;
            ++s_okCount;
            if (s_okCount <= 3 || s_okCount % 25 == 0) {
                qInfo("[mac-capture] captureImage ok #%d (%zux%zu)",
                      s_okCount, CGImageGetWidth(image),
                      CGImageGetHeight(image));
            }
            const size_t w = CGImageGetWidth(image);
            const size_t h = CGImageGetHeight(image);
            const size_t bpr = CGImageGetBytesPerRow(image);

            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            std::vector<uint8_t> buf(bpr * h);
            CGBitmapInfo binfo =
                (CGBitmapInfo)kCGImageAlphaPremultipliedFirst
                | (CGBitmapInfo)kCGBitmapByteOrder32Little;
            CGContextRef ctx = CGBitmapContextCreate(
                buf.data(), w, h, 8, bpr, cs, binfo);
            CGColorSpaceRelease(cs);
            if (!ctx) return;
            CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
            CGContextRelease(ctx);

            QImage qimg(buf.data(), int(w), int(h), int(bpr),
                        QImage::Format_ARGB32_Premultiplied);
            QImage owned = qimg.copy();
            QMetaObject::invokeMethod(this, [this, owned]() {
                // Captures are async — several can be in flight when
                // stop() lands, and a straggler frame delivered after
                // the stop would flip the controller back to "active"
                // (the stop-only-works-if-you-spam-click bug). Drop
                // anything that completes once we're no longer live.
                if (!m_active) return;
                m_consecutiveFails = 0;
                emit frameReady(owned);
            }, Qt::QueuedConnection);
        };

        SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
        config.capturesAudio = NO;
        config.showsCursor = YES;
        // Config width/height will auto-match the filter's source.
        [SCScreenshotManager captureImageWithFilter:(SCContentFilter*)m_filter
                                       configuration:config
                                   completionHandler:completion];
        [config release];
    }
}
