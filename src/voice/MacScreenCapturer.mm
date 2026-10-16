#include "voice/MacScreenCapturer.h"
#include "voice/ScreenCapturePolicy.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#include <QDateTime>
#include <QDebug>
#include <QPointer>
#include <QTimer>
#include <atomic>

// ScreenCaptureKit (macOS 12.3+) is Apple's sanctioned replacement
// for the now-obsolete CGDisplayCreateImage path. We capture with ONE
// long-lived SCStream per share, built from the picker's filter, with
// SCStreamConfiguration.minimumFrameInterval set from the target fps.
//
// This file used to poll +[SCScreenshotManager captureImageWithFilter:]
// once per QTimer tick instead ("no stream lifecycle to juggle"). That
// API is one-shot: every call is a fresh kTCCServiceScreenCapture
// authorisation check made by replayd on our behalf. On 2026-10-16
// tccd logged ~13,500 of them in 15 minutes of sharing at 5 fps, and
// the owner's Mac froze for seconds at a time and periodically showed
// the screen-recording prompt again. A stream is checked once, when it
// starts. Do not go back to per-frame screenshots.
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

// Output + delegate for one SCStream. Sample buffers arrive on the
// sink's own serial queue and stream errors on an SCK queue; neither may
// touch the capturer there. Both hop to the main queue — the thread that
// destroys the capturer — and re-check the QPointer before calling in,
// exactly like the old SCScreenshotManager completion (490f332): a raw
// `this` read from these queues is the 2026-09-25 replayd-thread crash.
@interface MacStreamSink : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    QPointer<MacScreenCapturer> _guard;   // read only on the main queue
    quint64 _gen;
    dispatch_queue_t _queue;
    // Throttle backstop, in microseconds. minimumFrameInterval is the
    // real limit; this drops anything that still arrives early (e.g.
    // between setFps() and the stream applying the new config).
    std::atomic<int64_t> _minIntervalUs;
    int64_t _lastPtsUs;                   // sample queue only
}
@end

@implementation MacStreamSink
- (instancetype)initWithOwner:(MacScreenCapturer*)owner gen:(quint64)gen fps:(int)fps
{
    if ((self = [super init])) {
        _guard = owner;
        _gen = gen;
        _queue = dispatch_queue_create("com.bsfchat.screencapture",
                                       DISPATCH_QUEUE_SERIAL);
        _minIntervalUs.store(fps > 0 ? 1000000 / fps : 200000);
        _lastPtsUs = INT64_MIN;
    }
    return self;
}
- (void)dealloc
{
    if (_queue) dispatch_release(_queue);
    [super dealloc];
}
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type API_AVAILABLE(macos(12.3))
{
    if (type != SCStreamOutputTypeScreen) return;
    if (!CMSampleBufferIsValid(sampleBuffer)) return;

    // Only Complete frames carry new pixels; Idle/Blank/Suspended/...
    // have no image buffer worth sending.
    CFArrayRef infos = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (!infos || CFArrayGetCount(infos) < 1) return;
    NSDictionary* info = (NSDictionary*)CFArrayGetValueAtIndex(infos, 0);
    NSNumber* status = info[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) return;

    // Drop frames that beat the target interval (0.9 of it, so normal
    // jitter around the configured rate does not halve it).
    const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    if (CMTIME_IS_VALID(pts)) {
        const int64_t ptsUs = CMTimeConvertScale(
            pts, 1000000, kCMTimeRoundingMethod_Default).value;
        const int64_t minUs = _minIntervalUs.load() * 9 / 10;
        if (_lastPtsUs != INT64_MIN && ptsUs >= _lastPtsUs
            && ptsUs - _lastPtsUs < minUs) {
            return;
        }
        _lastPtsUs = ptsUs;
    }

    CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pb) return;
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly)
        != kCVReturnSuccess) {
        return;
    }
    const size_t w = CVPixelBufferGetWidth(pb);
    const size_t h = CVPixelBufferGetHeight(pb);
    const size_t bpr = CVPixelBufferGetBytesPerRow(pb);
    void* base = CVPixelBufferGetBaseAddress(pb);
    QImage owned;
    if (base && w && h) {
        // kCVPixelFormatType_32BGRA is QImage's ARGB32 byte order on
        // little-endian; same format the CGImage path produced. Copy
        // out before unlocking — the buffer goes back to SCK's pool.
        QImage view(static_cast<const uchar*>(base), int(w), int(h),
                    int(bpr), QImage::Format_ARGB32_Premultiplied);
        owned = view.copy();
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (owned.isNull()) return;

    // Copying the QPointer here is fine (atomic refcount); dereferencing
    // it is not — that waits for the main queue.
    QPointer<MacScreenCapturer> guard = _guard;
    const quint64 gen = _gen;
    dispatch_async(dispatch_get_main_queue(), ^{
        MacScreenCapturer* owner = guard.data();
        if (!owner) return;
        owner->_onStreamFrame(gen, owned);
    });
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
    API_AVAILABLE(macos(12.3))
{
    NSLog(@"[mac-capture] stream stopped with error: %@", error);
    const bool isSCK = error && [error.domain isEqualToString:SCStreamErrorDomain];
    const long code = error ? long(error.code) : 0L;
    QString desc = error ? QString::fromNSString(error.localizedDescription)
                         : QStringLiteral("capture stream stopped");
    QPointer<MacScreenCapturer> guard = _guard;
    const quint64 gen = _gen;
    dispatch_async(dispatch_get_main_queue(), ^{
        MacScreenCapturer* owner = guard.data();
        if (!owner) return;
        // selection 0 = "whatever the stream was running" — see
        // _onStreamFailed.
        owner->_onStreamFailed(gen, 0, isSCK, code, desc);
    });
}
@end

namespace {

// Errors from SCK completion handlers, hopped to the Qt thread. Same
// guard rule as the sink.
API_AVAILABLE(macos(14.0))
void postStreamFailure(QPointer<MacScreenCapturer> guard, quint64 gen,
                       quint64 selection, NSError* err)
{
    const bool isSCK = err && [err.domain isEqualToString:SCStreamErrorDomain];
    const long code = err ? long(err.code) : 0L;
    QString desc = err ? QString::fromNSString(err.localizedDescription)
                       : QStringLiteral("capture stream failed");
    dispatch_async(dispatch_get_main_queue(), ^{
        MacScreenCapturer* owner = guard.data();
        if (!owner) return;
        owner->_onStreamFailed(gen, selection, isSCK, code, desc);
    });
}

// Output size: the picked content at native pixel scale, as the old
// screenshot path produced (it auto-matched the filter). An SCStream
// does not — width/height must be set, or it renders at its default.
API_AVAILABLE(macos(14.0))
SCStreamConfiguration* makeConfiguration(SCContentFilter* filter, int fps)
{
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.capturesAudio = NO;
    config.showsCursor = YES;
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.minimumFrameInterval = CMTimeMake(1, fps > 0 ? fps : 5);
    // Small pool: we copy each frame out immediately, and a deep queue
    // only adds latency.
    config.queueDepth = 3;
    const CGRect rect = filter.contentRect;
    const CGFloat scale = filter.pointPixelScale > 0 ? filter.pointPixelScale : 1;
    const size_t w = size_t(rect.size.width * scale);
    const size_t h = size_t(rect.size.height * scale);
    if (w > 0 && h > 0) {
        config.width = w;
        config.height = h;
    }
    return config;  // +1, caller releases
}

}  // namespace

MacScreenCapturer::MacScreenCapturer(QObject* parent)
    : QObject(parent)
    , m_heartbeat(new QTimer(this))
{
    // Re-send the last frame while the picked content is still. See the
    // header; at 1 s it costs nothing when frames are flowing (it only
    // fires into a no-op) and keeps a still share alive for viewers.
    m_heartbeat->setInterval(1000);
    connect(m_heartbeat, &QTimer::timeout, this, [this]() {
        if (!m_active || m_lastFrame.isNull()) return;
        if (QDateTime::currentMSecsSinceEpoch() - m_lastFrameMs < 1000) return;
        m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();
        emit frameReady(m_lastFrame);
    });

    if (@available(macOS 14.0, *)) {
        MacPickerObserver* obs = [[MacPickerObserver alloc] init];
        obs.owner = this;
        m_observer = obs;
        [[SCContentSharingPicker sharedPicker] addObserver:obs];
    }
}

MacScreenCapturer::~MacScreenCapturer() {
    // Stop and WAIT for the stream: no SCStream callback may still be
    // starting up against this object's state when it goes. (Callbacks
    // that land later are dropped by their QPointer guard.)
    stopImpl(/*waitForStream=*/true);
    if (m_filter) {
        [(id)m_filter release];
        m_filter = nullptr;
    }
    if (@available(macOS 14.0, *)) {
        if (m_observer) {
            // Clear the back-pointer BEFORE dropping our reference. The
            // observer's delegate methods hop to the main queue and
            // their blocks retain the observer, so one can still be in
            // flight after this release — and it reads `_owner`. Same
            // lifetime trap as the stream callbacks in MacStreamSink:
            // with `owner` left dangling it would call into a freed
            // MacScreenCapturer.
            ((MacPickerObserver*)m_observer).owner = nullptr;
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
// capture that fails is handled in _onStreamFailed(): end the share
// and let the user pick again.
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
        // pre-picker capture path to fall back to: the picker is 14+.
        // Say so instead of pretending to share.
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
    // Fresh share attempt — re-arm the failure notifier so a capture
    // problem surfaces again (it latches per attempt, not per app run).
    m_consecutiveFails = 0;
    m_failureNotified = false;

    if (m_active && m_stream) {
        // Mid-share re-pick: retarget the running stream. No restart,
        // so no second TCC check and no gap in the outgoing video.
        qInfo("[mac-capture] picker updated the filter; retargeting stream");
        if (@available(macOS 14.0, *)) {
            QPointer<MacScreenCapturer> guard = this;
            const quint64 gen = m_streamGen;
            const quint64 selection = m_selection;
            SCStreamConfiguration* config =
                makeConfiguration((SCContentFilter*)m_filter, m_fps);
            SCStream* stream = (SCStream*)m_stream;
            [stream updateContentFilter:(SCContentFilter*)m_filter
                      completionHandler:^(NSError* err) {
                if (err) {
                    qWarning("[mac-capture] updateContentFilter failed: %s (%ld)",
                             err.localizedDescription.UTF8String, long(err.code));
                    postStreamFailure(guard, gen, selection, err);
                    return;
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    MacScreenCapturer* owner = guard.data();
                    if (!owner) return;
                    owner->_onFilterApplied(gen, selection);
                });
            }];
            // New pick, new size (a window is not the display it
            // replaced). Errors here are not fatal: the stream keeps
            // running and scales into the old size.
            [stream updateConfiguration:config completionHandler:^(NSError* err) {
                if (err) {
                    qWarning("[mac-capture] updateConfiguration failed: %s (%ld)",
                             err.localizedDescription.UTF8String, long(err.code));
                }
            }];
            [config release];
        }
    } else {
        qInfo("[mac-capture] picker returned a filter; starting capture");
        m_active = true;
        startStream();
    }
    emit sourceSelected();
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
    // of its own — capture outside the picker, the access macOS 15+
    // periodically makes the user re-approve. Picking is the only way in.
    setFps(fps);
    qInfo("[mac-capture] start(fps=%d) → picker", m_fps);
    showPicker();
}

void MacScreenCapturer::setFps(int fps)
{
    const int next = fps > 0 ? fps : 5;
    const bool changed = next != m_fps;
    m_fps = next;
    if (!changed || !m_stream || !m_sink || !m_filter) return;
    if (@available(macOS 14.0, *)) {
        MacStreamSink* sink = (MacStreamSink*)m_sink;
        sink->_minIntervalUs.store(1000000 / m_fps);
        SCStreamConfiguration* config =
            makeConfiguration((SCContentFilter*)m_filter, m_fps);
        [(SCStream*)m_stream updateConfiguration:config
                               completionHandler:^(NSError* err) {
            if (err) {
                qWarning("[mac-capture] fps update failed: %s (%ld)",
                         err.localizedDescription.UTF8String, long(err.code));
            }
        }];
        [config release];
        qInfo("[mac-capture] stream fps → %d", m_fps);
    }
}

void MacScreenCapturer::startStream()
{
    bool pickerAvailable = false;
    if (@available(macOS 14.0, *)) pickerAvailable = true;
    switch (screencap::routeTick(pickerAvailable, m_filter != nullptr)) {
    case screencap::TickRoute::CaptureWithPickerFilter:
        break;
    case screencap::TickRoute::Skip:
    case screencap::TickRoute::Unsupported:
        return;
    }
    if (m_stream) return;

    if (@available(macOS 14.0, *)) {
        const quint64 gen = ++m_streamGen;
        const quint64 selection = m_selection;
        SCContentFilter* filter = (SCContentFilter*)m_filter;
        SCStreamConfiguration* config = makeConfiguration(filter, m_fps);
        MacStreamSink* sink = [[MacStreamSink alloc] initWithOwner:this
                                                               gen:gen
                                                               fps:m_fps];
        SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                              configuration:config
                                                   delegate:sink];
        qInfo("[mac-capture] stream #%llu start (%zux%zu @ %d fps)",
              (unsigned long long)gen, config.width, config.height, m_fps);
        [config release];

        NSError* addErr = nil;
        if (![stream addStreamOutput:sink
                                type:SCStreamOutputTypeScreen
                  sampleHandlerQueue:sink->_queue
                               error:&addErr]) {
            qWarning("[mac-capture] addStreamOutput failed: %s",
                     addErr ? addErr.localizedDescription.UTF8String : "?");
            [stream release];
            [sink release];
            QPointer<MacScreenCapturer> guard = this;
            postStreamFailure(guard, gen, selection, addErr);
            return;
        }
        m_stream = stream;
        m_sink = sink;
        m_appliedSelection = selection;

        QPointer<MacScreenCapturer> guard = this;
        [stream startCaptureWithCompletionHandler:^(NSError* err) {
            if (err) {
                qWarning("[mac-capture] startCapture failed: %s (%ld)",
                         err.localizedDescription.UTF8String, long(err.code));
                postStreamFailure(guard, gen, selection, err);
                return;
            }
            qInfo("[mac-capture] stream #%llu running", (unsigned long long)gen);
        }];
        m_heartbeat->start();
    }
}

void MacScreenCapturer::teardownStream(bool wait)
{
    m_heartbeat->stop();
    m_lastFrame = {};
    ++m_streamGen;   // anything the old stream still delivers is stale
    if (!m_stream) return;

    if (@available(macOS 14.0, *)) {
        SCStream* stream = (SCStream*)m_stream;
        MacStreamSink* sink = (MacStreamSink*)m_sink;
        m_stream = nullptr;
        m_sink = nullptr;

        // The completion block takes over our references and releases
        // them only once the stream has stopped — and after draining
        // the sample queue, so a frame callback already running there
        // finishes against a live sink. An error just means the stream
        // had already stopped (e.g. after didStopWithError).
        dispatch_semaphore_t done = wait ? dispatch_semaphore_create(0) : nullptr;
        [stream stopCaptureWithCompletionHandler:^(NSError* err) {
            if (err && err.code != SCStreamErrorAttemptToStopStreamState) {
                qInfo("[mac-capture] stopCapture: %s (%ld)",
                      err.localizedDescription.UTF8String, long(err.code));
            }
            dispatch_async(sink->_queue, ^{
                [stream release];
                [sink release];
            });
            if (done) dispatch_semaphore_signal(done);
        }];
        if (done) {
            // Bounded: never hang shutdown on replayd. The completion
            // does not need the main thread, so this cannot deadlock.
            if (dispatch_semaphore_wait(done,
                    dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0) {
                qWarning("[mac-capture] stopCapture did not complete in 2 s");
            }
            dispatch_release(done);
        }
    }
}

void MacScreenCapturer::stop()
{
    stopImpl(/*waitForStream=*/false);
}

void MacScreenCapturer::stopImpl(bool waitForStream)
{
    if (!m_active) return;
    m_active = false;
    teardownStream(waitForStream);
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

void MacScreenCapturer::_onStreamFrame(quint64 streamGen, const QImage& frame)
{
    // The stream delivers asynchronously — frames can still be queued
    // when stop() lands, and a straggler delivered after the stop would
    // flip the controller back to "active" (the stop-only-works-if-you-
    // spam-click bug). Drop anything from a stream we no longer run.
    if (!m_active) return;
    if (streamGen != m_streamGen) return;
    static int s_okCount = 0;
    ++s_okCount;
    if (s_okCount <= 3 || s_okCount % 250 == 0) {
        qInfo("[mac-capture] frame #%d (%dx%d)",
              s_okCount, frame.width(), frame.height());
    }
    m_consecutiveFails = 0;
    m_lastFrame = frame;
    m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();
    emit frameReady(frame);
}

void MacScreenCapturer::_onFilterApplied(quint64 streamGen, quint64 selection)
{
    if (!m_active || streamGen != m_streamGen) return;
    if (selection > m_appliedSelection) m_appliedSelection = selection;
}

void MacScreenCapturer::_onStreamFailed(quint64 streamGen, quint64 selection,
                                        bool isScreenCaptureKitError,
                                        long code, const QString& desc)
{
    if (!m_active) return;
    // A straggler from a stream we already tore down (or a share that
    // has since stopped) says nothing about the current one.
    if (streamGen != m_streamGen) return;
    // selection 0 ⇒ didStopWithError, which applies to whatever the
    // stream was running.
    if (selection == 0) selection = m_appliedSelection;
    qWarning("[mac-capture] stream #%llu failed: %s (%ld)",
             (unsigned long long)streamGen, qPrintable(desc), code);

    // Whatever happened, this stream is done for: tear it down before
    // deciding whether there is another.
    teardownStream(false);

    auto kind = screencap::classifyFailure(isScreenCaptureKitError, code);
    // The user re-picked (often precisely because the old window was
    // closing) and the error is about the pick being replaced: retry
    // with the new filter rather than ending the share.
    if (selection != m_selection && kind == screencap::FailureKind::SelectionEnded)
        kind = screencap::FailureKind::Retry;

    switch (screencap::onCaptureFailure(kind, ++m_consecutiveFails,
                                        kMaxConsecutiveFails,
                                        m_failureNotified)) {
    case screencap::FailureAction::KeepTrying: {
        if (m_failureNotified) return;   // reported; the controller stops
        // Restart with the current filter. Each start is one TCC check,
        // so pace it — and the failure count above bounds it.
        const quint64 sel = m_selection;
        QTimer::singleShot(500, this, [this, sel]() {
            if (!m_active || sel != m_selection || m_stream) return;
            qInfo("[mac-capture] restarting stream after failure");
            startStream();
        });
        return;
    }
    case screencap::FailureAction::EndShare:
        qInfo("[mac-capture] selection ended (code %ld) — ending share", code);
        endShare(QString::fromUtf8(screencap::selectionEndedMessage(code)));
        return;
    case screencap::FailureAction::ReportFailure:
        // Repeated failures must surface rather than retry silently
        // forever. This reports; it does NOT go looking for a wider
        // permission — the controller ends the share and the user can
        // pick again.
        m_failureNotified = true;
        emit captureFailed(desc);
        return;
    }
}
