#include "voice/MacScreenCapturer.h"

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
// SCContentFilter we retain and use as the capture target.

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

void MacScreenCapturer::ensureScreenAccess()
{
    // SCScreenshotManager NEVER triggers the Screen Recording TCC
    // prompt — it just fails with "user declined" when no valid grant
    // exists (fresh install, tccutil reset, or a stale grant from a
    // differently-signed build). CGRequestScreenCaptureAccess is the
    // only API that asks. With no TCC entry it shows the real system
    // prompt; with a denied/stale entry it opens System Settings —
    // either way, once per app run is the right amount of nagging.
    if (CGPreflightScreenCaptureAccess()) return;
    if (m_accessRequested) return;
    m_accessRequested = true;
    qInfo("[mac-capture] no screen-capture grant — requesting");
    CGRequestScreenCaptureAccess();
}

void MacScreenCapturer::showPicker()
{
    if (@available(macOS 14.0, *)) {
        ensureScreenAccess();
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
        [picker present];
    } else {
        qWarning("[mac-capture] showPicker requires macOS 14+; "
                 "falling back to primary display capture");
        start(0, 5);
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

void MacScreenCapturer::start(uint32_t displayID, int fps)
{
    if (m_active) return;
    ensureScreenAccess();
    m_displayID = displayID ? displayID : CGMainDisplayID();
    m_fps = fps > 0 ? fps : 5;
    m_timer->setInterval(1000 / m_fps);
    m_timer->start();
    m_active = true;
    m_consecutiveFails = 0;
    m_failureNotified = false;
    qInfo("[mac-capture] start displayID=%u fps=%d", m_displayID, m_fps);
    grabWithFilter();
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
    qInfo("[mac-capture] stop");
}

// Helper that does the actual SCScreenshotManager capture — either
// with the user-picked filter, or by building one from the main
// display as a fallback.
void MacScreenCapturer::grabWithFilter()
{
    static int s_tick = 0;
    ++s_tick;
    if (s_tick % 25 == 1) {
        qInfo("[mac-capture] tick #%d filter=%p active=%d",
              s_tick, (void*)m_filter, int(m_active));
    }
    if (@available(macOS 14.0, *)) {
        __block SCContentFilter* filter = m_filter;
        auto completion = ^(CGImageRef image, NSError *err) {
            if (err || !image) {
                static int s_failCount = 0;
                ++s_failCount;
                if (s_failCount <= 3 || s_failCount % 25 == 0) {
                    qWarning("[mac-capture] captureImage failed #%d: %s",
                             s_failCount,
                             err ? err.localizedDescription.UTF8String
                                 : "nil image");
                }
                // Per-call failures (stale TCC grant, revoked mid-share)
                // must surface after a few consecutive misses — the
                // completion runs off-thread, so count on the Qt thread.
                QString desc = err
                    ? QString::fromNSString(err.localizedDescription)
                    : QStringLiteral("capture returned no image");
                QMetaObject::invokeMethod(this, [this, desc]() {
                    if (!m_active) return;
                    if (++m_consecutiveFails >= kMaxConsecutiveFails
                        && !m_failureNotified) {
                        m_failureNotified = true;
                        emit captureFailed(desc);
                    }
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

        if (filter) {
            // Config width/height will auto-match the filter's source.
            [SCScreenshotManager captureImageWithFilter:filter
                                           configuration:config
                                       completionHandler:completion];
            return;
        }

        // Fallback: build a display-wide filter from SCShareableContent.
        uint32_t wantDisplayID = m_displayID;
        [SCShareableContent getShareableContentWithCompletionHandler:
            ^(SCShareableContent *content, NSError *error) {
            if (error || !content || content.displays.count == 0) {
                QString desc = error
                    ? QString::fromNSString(error.localizedDescription)
                    : QStringLiteral("no displays");
                qWarning("[mac-capture] getShareableContent failed: %s",
                         qUtf8Printable(desc));
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    QMetaObject::invokeMethod(this, [this, desc]() {
                        emit captureFailed(desc);
                    }, Qt::QueuedConnection);
                }
                return;
            }
            SCDisplay *target = content.displays.firstObject;
            for (SCDisplay *d in content.displays) {
                if (d.displayID == wantDisplayID) { target = d; break; }
            }
            SCContentFilter *fallbackFilter =
                [[SCContentFilter alloc] initWithDisplay:target
                                        excludingWindows:@[]];
            SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
            cfg.width = target.width;
            cfg.height = target.height;
            cfg.capturesAudio = NO;
            cfg.showsCursor = YES;
            [SCScreenshotManager captureImageWithFilter:fallbackFilter
                                           configuration:cfg
                                       completionHandler:completion];
        }];
        return;
    }
}
