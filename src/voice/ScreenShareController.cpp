#include "voice/ScreenShareController.h"

#include "voice/video/VideoCodecSelect.h"
#include "voice/IVoiceTransport.h"
#include "voice/VoiceEngine.h"
#include "voice/video/VideoRateController.h"
#include "voice/video/LatestWinsWorker.h"
#include "voice/video/VideoSendPipeline.h"
#include "net/ServerManager.h"
#include "net/ServerConnection.h"

#ifdef Q_OS_MACOS
#include "voice/MacScreenCapturer.h"
#endif

#include "core/Settings.h"
#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>
#include <QBuffer>
#include <QImage>
#include <QVariantMap>
#include <QDebug>
#include <QProcess>
#include <QVideoFrame>
#include <algorithm>

#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
extern "C" bool CGPreflightScreenCaptureAccess(void);
extern "C" bool CGRequestScreenCaptureAccess(void);
#endif

ScreenShareController::QualityPreset
ScreenShareController::presetFor(int level)
{
    switch (std::clamp(level, 0, 3)) {
    case 0:  return {2,  960, 40};
    case 1:  return {5, 1280, 60};
    case 2:  return {10, 1600, 75};
    case 3:  default: return {15, 1920, 85};
    }
}

// Resolve the effective preset on every start: min(user pref, server
// max). Kept function-scope statics in ScreenShareController.cpp so
// the push-side scaling + throttle picks them up without plumbing
// through m_mac.
static int g_frameIntervalMs = 200;
static int g_jpegQuality = 60;
static int g_maxWidth = 1280;
// Resolved encoder config for the RTP path — same resolution/fps
// source as the JPEG globals.
static EncoderConfig g_encoderConfig;
// Lossless-tier resolution: user toggle ∧ server policy. The final
// gate (every peer advertises av1-dc) is checked per push because
// peers churn.
static bool g_losslessWanted = false;
static bool g_losslessAllowed = true;
// Latched when the transport reports lossless frames can't be
// delivered (e.g. oversized for the peer's channel) — forces the
// H.264 path for the rest of the share instead of silently sending
// nothing. Cleared whenever quality settings are (re)applied.
static bool g_losslessBroken = false;
// Admission budget for the reliable lossless channel: with more than
// this queued toward any peer, capture frames are DROPPED (never
// delayed) until the channel drains.
static constexpr qint64 kLosslessBufferBudget = 2 * 1024 * 1024;

ScreenShareController::ScreenShareController(QObject* parent)
    : QObject(parent)
    , m_sink(new QVideoSink(this))
    , m_throttle(new QTimer(this))
{
#ifdef Q_OS_MACOS
    // Homebrew Qt is built without QT_FEATURE_screen_capture, so
    // QScreenCapture is unusable on macOS in this environment.
    // Replace it with our CGDisplayCreateImage-based polling capturer.
    m_mac = new MacScreenCapturer(this);
    connect(m_mac, &MacScreenCapturer::captureFailed, this,
        [this](const QString& desc) {
            m_lastError = QStringLiteral(
                "Screen capture refused (\"%1\"). If a permission prompt "
                "just appeared and you granted it, quit and reopen "
                "BSFChat — macOS applies Screen Recording grants on the "
                "next launch. If there was no prompt, the stored grant "
                "is stale (it no longer matches this binary's code "
                "signature after a rebuild): run  tccutil reset "
                "ScreenCapture com.bsfchat.app.dev  in a terminal and "
                "relaunch, or remove the entry in System Settings → "
                "Privacy & Security → Screen & System Audio Recording."
            ).arg(desc);
            emit lastErrorChanged();
            stop();
        });
    // S-12: showPicker() starts the capture throttle up front so the
    // first frame after a selection isn't delayed by a timer tick. If
    // the user cancels instead, that timer was left running forever,
    // waking the app 2-15 times a second to look at an invalid frame.
    connect(m_mac, &MacScreenCapturer::pickerCancelled, this,
        [this]() {
            if (m_active) return;   // a capture was already running
            m_throttle->stop();
            m_pendingFrame = {};
            setTransmitting(false);
        });
    connect(m_mac, &MacScreenCapturer::frameReady, this,
        [this](const QImage& img) {
            // First frame — flip active state so QML preview shows.
            if (!m_active) setActiveState(true);
            // S-10: the QImage -> QVideoFrame copy is a full-frame
            // memcpy (33 MB at 4K Retina) and used to run right here,
            // in the GUI thread, once per captured frame. Hand the
            // (implicitly shared, so free to pass) image to a worker
            // and take the result back on this thread. A capture tick
            // that arrives while the worker is still busy replaces the
            // pending one — latest wins, never a backlog.
            if (!m_frameWorker) {
                m_frameWorker = std::make_unique<LatestWinsWorker>(
                    QStringLiteral("video-frame-copy"));
            }
            m_frameWorker->submit([this, img]() {
                QVideoFrameFormat fmt(img.size(),
                    QVideoFrameFormat::pixelFormatFromImageFormat(img.format()));
                QVideoFrame vf(fmt);
                if (!vf.map(QVideoFrame::WriteOnly)) return;
                // One plane, RGB image.
                std::memcpy(vf.bits(0), img.bits(),
                            size_t(img.bytesPerLine()) * size_t(img.height()));
                vf.unmap();
                QMetaObject::invokeMethod(this, [this, vf]() {
                    if (!m_active) return;
                    // Feed the internal sink so any VideoOutput
                    // mirroring via forwardTo() receives frames, and
                    // cache the latest for the peer push.
                    m_sink->setVideoFrame(vf);
                    m_pendingFrame = vf;
                }, Qt::QueuedConnection);
            });
        });
#else
    m_capture = new QScreenCapture(this);
    m_windowCapture = new QWindowCapture(this);
    m_session = new QMediaCaptureSession(this);
    // Both capturers stay attached to the one session; only one is
    // ever started at a time (startForScreen/startForWindow stop the
    // other first), so the shared sink never sees interleaved frames.
    m_session->setScreenCapture(m_capture);
    m_session->setWindowCapture(m_windowCapture);
    m_session->setVideoSink(m_sink);

    // "Active" means either capturer is running.
    auto updateActive = [this]() {
        const bool a = m_capture->isActive() || m_windowCapture->isActive();
        if (m_active == a) return;
        qInfo("[screenshare] active=%d", int(a));
        setActiveState(a);
    };
    connect(m_capture, &QScreenCapture::activeChanged, this, updateActive);
    connect(m_windowCapture, &QWindowCapture::activeChanged,
            this, updateActive);

    auto reportError = [this](const QString& description) {
        if (description.isEmpty()) return;
        m_lastError = description;
        qWarning("[screenshare] error: %s", qUtf8Printable(description));
        emit lastErrorChanged();
    };
    connect(m_capture, &QScreenCapture::errorOccurred, this,
        [reportError](QScreenCapture::Error, const QString& description) {
            reportError(description);
        });
    // Window capture errors also fire when the shared window closes
    // mid-share — the toast doubles as "your share just ended".
    connect(m_windowCapture, &QWindowCapture::errorOccurred, this,
        [reportError](QWindowCapture::Error, const QString& description) {
            reportError(description);
        });
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
        [this](const QVideoFrame& frame) { m_pendingFrame = frame; });
#endif

    m_throttle->setInterval(g_frameIntervalMs);
    connect(m_throttle, &QTimer::timeout, this,
            &ScreenShareController::pushFrameToPeers);

    // Encode worker for the RTP video path. Encoded access units come
    // back on a queued signal; the voice engine is re-resolved per
    // frame (engines are per-session and churn with join/leave).
    m_pipeline = new VideoSendPipeline(VideoStreamId::Screen, this);
    connect(m_pipeline, &VideoSendPipeline::encodedFrameReady, this,
        [this](int, const EncodedFrame& frame) {
            if (!m_servers) return;
            if (auto* vs = m_servers->voiceServer()) {
                if (auto* voice = vs->voiceEngine()) {
                    if (frame.codec == VideoCodecKind::Av1Lossless)
                        voice->broadcastLosslessVideo(VideoStreamId::Screen, frame);
                    else
                        voice->broadcastEncodedVideo(VideoStreamId::Screen, frame);
                }
            }
        });

    // The rate controller adjusts bitrate/resolution between capture
    // ticks; pushFrameToPeers reads its outputs into the encoder
    // config each frame, and a back-off forces an immediate IDR.
    m_rate = new VideoRateController(VideoStreamId::Screen, this);
    connect(m_rate, &VideoRateController::forceKeyframe,
            m_pipeline, &VideoSendPipeline::forceKeyframe);
}

// Forward decl — definition is below applyEffectiveQuality, which
// itself depends on the static globals declared above. Keeps the
// "smallest hop down the file" reading order while letting
// setSettings() bind a lambda that calls apply.
static void applyEffectiveQuality(Settings*, ServerManager*);

void ScreenShareController::setSettings(Settings* settings)
{
    if (m_settings == settings) return;
    m_settings = settings;
    if (!settings) return;
    // Live-reapply when any of the user-facing knobs change so a
    // slider tweak takes effect mid-share — but debounced: fps and
    // resolution changes rebuild the encoder session (an IDR each
    // time), and a slider DRAG emits one change per detent. Apply
    // once, after the knob settles.
    if (!m_reapplyDebounce) {
        m_reapplyDebounce = new QTimer(this);
        m_reapplyDebounce->setSingleShot(true);
        m_reapplyDebounce->setInterval(300);
        connect(m_reapplyDebounce, &QTimer::timeout, this, [this]() {
            applyEffectiveQuality(m_settings, m_servers);
            if (m_active) {
                m_throttle->setInterval(g_frameIntervalMs);
#ifdef Q_OS_MACOS
                if (m_mac) m_mac->setFps(1000 / g_frameIntervalMs);
#endif
            }
        });
    }
    auto reapply = [this]() { m_reapplyDebounce->start(); };
    connect(settings, &Settings::screenShareFpsChanged, this, reapply);
    connect(settings, &Settings::screenShareMaxWidthChanged, this, reapply);
    connect(settings, &Settings::screenShareJpegQualityChanged, this, reapply);
    connect(settings, &Settings::screenShareTargetKbpsChanged, this, reapply);
    connect(settings, &Settings::screenShareKeyframeSecChanged, this, reapply);
    connect(settings, &Settings::screenShareLosslessChanged, this, reapply);
}

QVariantList ScreenShareController::availableScreens() const
{
    QVariantList out;
    const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        QVariantMap m;
        m[QStringLiteral("index")] = i;
        m[QStringLiteral("name")] = screens[i]->name();
        m[QStringLiteral("width")] = screens[i]->geometry().width();
        m[QStringLiteral("height")] = screens[i]->geometry().height();
        m[QStringLiteral("primary")] = (screens[i] == QGuiApplication::primaryScreen());
        out.append(m);
    }
    return out;
}

void ScreenShareController::refreshWindows()
{
#ifdef Q_OS_MACOS
    // macOS never populates this list — window selection goes through
    // the native SCContentSharingPicker (showPicker()), which handles
    // enumeration, thumbnails and TCC in one system surface.
#else
    m_qtWindows.clear();
    QVariantList out;
    const auto windows = QWindowCapture::capturableWindows();
    for (const auto& w : windows) {
        // Wayland compositors without the right portal return nothing;
        // X11/Windows return everything including nameless utility
        // windows — skip those, a user can't tell blank rows apart.
        if (!w.isValid() || w.description().isEmpty()) continue;
        QVariantMap m;
        m[QStringLiteral("index")] = m_qtWindows.size();
        m[QStringLiteral("name")] = w.description();
        out.append(m);
        m_qtWindows.append(w);
    }
    m_windowList = out;
    emit windowsChanged();
#endif
}

void ScreenShareController::startForWindow(int windowIndex)
{
    if (m_active) return;
    qInfo("[screenshare] startForWindow(%d)", windowIndex);
#ifdef Q_OS_MACOS
    Q_UNUSED(windowIndex);
    // Unreachable via the UI (the picker dialog never lists windows
    // on macOS) — route to the native picker just in case.
    showPicker();
#else
    applyEffectiveQuality(m_settings, m_servers);
    if (windowIndex < 0 || windowIndex >= m_qtWindows.size()) {
        m_lastError = QStringLiteral("That window is no longer available.");
        emit lastErrorChanged();
        refreshWindows();
        return;
    }
    const QCapturableWindow win = m_qtWindows[windowIndex];
    // The list is a snapshot from when the picker opened — the window
    // may have closed while the user was choosing.
    if (!win.isValid()) {
        m_lastError = QStringLiteral(
            "\"%1\" closed before sharing started.").arg(win.description());
        emit lastErrorChanged();
        refreshWindows();
        return;
    }
    m_lastError.clear();
    emit lastErrorChanged();
    m_capture->stop();
    m_windowCapture->setWindow(win);
    m_windowCapture->start();
    m_throttle->setInterval(g_frameIntervalMs);
    m_throttle->start();
#endif
}

void ScreenShareController::setServerManager(ServerManager* mgr)
{
    if (m_servers == mgr) return;
    m_servers = mgr;
    if (!mgr) return;
    connect(mgr, &ServerManager::activeServerChanged,
            this, &ScreenShareController::rewireVoiceLeaveWatch);
    // Rewire on list changes too — a connection added after the share
    // started must still be watched.
    connect(mgr, &ServerManager::serverAdded,
            this, &ScreenShareController::rewireVoiceLeaveWatch);
    connect(mgr, &ServerManager::serverRemoved,
            this, &ScreenShareController::rewireVoiceLeaveWatch);
    rewireVoiceLeaveWatch();
}

void ScreenShareController::rewireVoiceLeaveWatch()
{
    for (const auto& conn : m_voiceRoomConns) disconnect(conn);
    m_voiceRoomConns.clear();
    if (!m_servers) return;
    // Watch EVERY connection, not just the active one — the user can
    // be in voice on server A while browsing server B, and the share
    // must stop when the voice session (wherever it lives) ends.
    for (int i = 0; i < m_servers->connectionCount(); ++i) {
        auto* sc = m_servers->connectionAt(i);
        if (!sc) continue;
        m_voiceRoomConns.append(
            connect(sc, &ServerConnection::activeVoiceRoomIdChanged,
                    this, [this]() {
            // Mirror CameraController: tear down the share the moment
            // no connection is in voice anymore, so a follow-up join
            // doesn't silently resume sharing the screen.
            if (m_active && !m_servers->voiceServer()) {
                stop();
            }
        }));
    }
}

void ScreenShareController::start() { startForScreen(-1); }

// ROADMAP — H.264 / VP9 over RTP migration
// ----------------------------------------
// The current pipeline is per-frame JPEG over an SCTP data channel.
// That works at any resolution but is wildly bandwidth-inefficient
// vs. a real video codec — every frame is a full keyframe, no
// inter-frame compression. To hit "4K near-lossless on a LAN" we
// need to migrate to:
//   1. A platform encoder (VideoToolbox on macOS, MediaCodec on
//      Android, libavcodec/x264 on Linux+Windows). Add a
//      VideoEncoder C++ abstraction at src/voice/VideoEncoder.h
//      with a NAL-out / config-in API.
//   2. libdatachannel rtc::Track + H264RtpPacketizer in
//      PeerConnectionManager — the SDP offer needs a video m-line.
//      Receive side: rtc::Track::onMessage → depacketize → feed a
//      mirror VideoDecoder.
//   3. Quality config carries bitrate (kbps) + keyframe interval
//      + profile in addition to fps/resolution.
// This file's preset/clamp logic is the right hook point for that
// migration: the resolved Config struct just gains a `codec: enum
// {JPEG, H264, VP9}` field and the encode dispatch swaps on it.
// Until then, JPEG with arbitrary user-controllable fps/quality is
// good enough to give users the full flexibility envelope.

// Resolve the user's chosen quality + the active server's policy
// caps into the live encoder/throttle globals. Honoured envelope:
//   fps         1 .. 60
//   maxWidth    480 .. 3840 (long edge)
//   jpegQuality 1 .. 100
// Server caps (`maxScreenShare{Fps,Width,Jpeg}`) clamp downward
// when set; -1 sentinels mean "no cap on this axis".
static void applyEffectiveQuality(Settings* settings, ServerManager* servers)
{
    int userFps   = settings ? settings->screenShareFps() : 5;
    int userMaxW  = settings ? settings->screenShareMaxWidth() : 1280;
    int userJpegQ = settings ? settings->screenShareJpegQuality() : 60;
    int userKbps  = settings ? settings->screenShareTargetKbps() : 4000;
    int userGop   = settings ? settings->screenShareKeyframeSec() : 10;

    int srvFps = -1, srvW = -1, srvJpegQ = -1, srvKbps = -1;
    if (servers) {
        // Caps come from the server hosting the voice session the
        // frames go to; fall back to the focused server when the
        // share starts before a voice join.
        auto* host = servers->voiceServer();
        if (!host) host = servers->activeServer();
        if (host) {
            srvFps    = host->maxScreenShareFps();
            srvW      = host->maxScreenShareWidth();
            srvJpegQ  = host->maxScreenShareJpeg();
            srvKbps   = host->maxScreenShareBitrate();
        }
    }

    int fps   = (srvFps   >= 0) ? std::min(userFps,   srvFps)   : userFps;
    int maxW  = (srvW     >= 0) ? std::min(userMaxW,  srvW)     : userMaxW;
    int jpegQ = (srvJpegQ >= 0) ? std::min(userJpegQ, srvJpegQ) : userJpegQ;
    int kbps  = (srvKbps  >= 0) ? std::min(userKbps,  srvKbps)  : userKbps;

    fps   = std::clamp(fps,   1,  60);
    maxW  = std::clamp(maxW,  480, 3840);
    jpegQ = std::clamp(jpegQ, 1,  100);
    // No arbitrary bitrate ceiling. The only bound is content-derived:
    // past the rate that codes EVERY frame as a high-quality keyframe
    // at this resolution/fps (~0.5 bits per pixel per frame), extra
    // bits are provably wasted — the encoder cannot spend them. Actual
    // path safety is the rate controller's job: it climbs only while
    // peers' delivery reports prove the path carries it, and holds at
    // a conservative rate when it's flying blind.
    const double ceilPixels = double(maxW) * (double(maxW) * 9.0 / 16.0);
    const int allKeyframeKbps =
        int(ceilPixels * double(fps) * 0.5 / 1000.0);
    const int kbpsWanted = kbps;
    kbps  = std::clamp(kbps,  250, std::max(allKeyframeKbps, 1000));
    if (kbps < kbpsWanted) {
        // Visible on purpose: this is the ONE place a user's
        // configured ceiling is legitimately reduced, and "I set
        // 50 Mbps and it never went near it" is otherwise
        // indistinguishable from the rate controller misbehaving.
        qInfo("[screenshare] %d kbps requested; %d px @ %d fps cannot spend "
              "more than %d kbps — using that",
              kbpsWanted, maxW, fps, kbps);
    }

    g_frameIntervalMs = 1000 / fps;
    g_jpegQuality = jpegQ;
    g_maxWidth = maxW;

    // RTP encoder config shares the resolved fps/resolution envelope.
    // width/height express the max long edge — the pipeline follows
    // the source's actual (scaled) dimensions per frame. The target
    // bitrate is the rate controller's CEILING; it converges up to it
    // when delivery is clean and rides below it under loss.
    g_encoderConfig.codec = VideoCodecKind::H264;
    g_encoderConfig.width = maxW;
    g_encoderConfig.height = maxW;
    g_encoderConfig.fps = fps;
    g_encoderConfig.targetBitrateKbps = kbps;
    g_encoderConfig.maxBitrateKbps = kbps + kbps / 2;
    g_encoderConfig.keyframeIntervalSec = std::clamp(userGop, 1, 30);
    g_encoderConfig.screenContent = true;

    g_losslessWanted = settings && settings->screenShareLossless();
    // Settings (re)applied — give lossless another chance; the stall
    // signal re-latches if the transport still can't carry it.
    g_losslessBroken = false;
    g_losslessAllowed = true;
    if (servers) {
        auto* host = servers->voiceServer();
        if (!host) host = servers->activeServer();
        if (host) g_losslessAllowed = host->allowLossless();
    }

    qInfo("[screenshare] effective fps=%d maxW=%d Q=%d "
          "(user fps=%d maxW=%d Q=%d, server caps fps=%d maxW=%d Q=%d)",
          fps, maxW, jpegQ,
          userFps, userMaxW, userJpegQ,
          srvFps, srvW, srvJpegQ);
}

void ScreenShareController::startForScreen(int screenIndex)
{
    if (m_active) return;
    qInfo("[screenshare] startForScreen(%d)", screenIndex);
    applyEffectiveQuality(m_settings, m_servers);

#ifdef Q_OS_MACOS
    // DO NOT call CGRequestScreenCaptureAccess here — on modern
    // macOS that pops System Settings to the Screen Recording pane
    // on every invocation. We already know the user has granted
    // access (otherwise SCScreenshotManager will just fail the
    // capture call silently, which MacScreenCapturer logs).
    // CGPreflight is informational only.
    qInfo("[screenshare] CGPreflight=%d",
          int(CGPreflightScreenCaptureAccess()));

    m_lastError.clear();
    emit lastErrorChanged();
    // Map screen index to CGDirectDisplayID if needed — MVP uses
    // the main display. FPS comes from the applied preset.
    m_mac->start(0, 1000 / g_frameIntervalMs);
    m_throttle->setInterval(g_frameIntervalMs);
    m_throttle->start();
#else
    auto screens = QGuiApplication::screens();
    QScreen* target = nullptr;
    if (screenIndex >= 0 && screenIndex < screens.size())
        target = screens[screenIndex];
    else
        target = QGuiApplication::primaryScreen();
    if (!target) {
        m_lastError = "No screen available";
        emit lastErrorChanged();
        return;
    }
    m_lastError.clear();
    emit lastErrorChanged();
    m_windowCapture->stop();
    m_capture->setScreen(target);
    m_capture->start();
    m_throttle->setInterval(g_frameIntervalMs);
    m_throttle->start();
#endif
}

void ScreenShareController::stop()
{
    m_throttle->stop();
    m_pendingFrame = {};
    setTransmitting(false);
    if (m_rate) m_rate->setActive(false);
    // Push an empty frame so QML previews blank out instead of
    // keeping the frozen last frame.
    m_sink->setVideoFrame(QVideoFrame());
#ifdef Q_OS_MACOS
    if (m_mac) m_mac->stop();
    if (m_active) setActiveState(false);
#else
    if (m_active) {
        // Both capturers report through updateActive(), which routes
        // the flip through setActiveState().
        m_capture->stop();
        m_windowCapture->stop();
    }
#endif
}

void ScreenShareController::setActiveState(bool active)
{
    if (m_active == active) return;
    m_active = active;
    if (!active) {
        // A frame conversion in flight completes into a share that no
        // longer exists; the completion checks m_active, and clearing
        // here (after the flag drops) keeps a late one from becoming
        // the first frame of the NEXT share.
        m_pendingFrame = {};
    }
    if (active && m_pipeline) {
        // S-11: the encode session survives a stop/start (that is the
        // point — a restart costs no renegotiation), so without this
        // the first frame of the new share is a P-frame against a
        // reference nobody has, and every viewer sits on a placeholder
        // until the periodic IDR.
        m_pipeline->forceKeyframe();
    }
    announceStream(active);
    emit activeChanged();
}

IVoiceTransport* ScreenShareController::currentVoice() const
{
    if (!m_servers) return nullptr;
    auto* vs = m_servers->voiceServer();
    return vs ? vs->voiceEngine() : nullptr;
}

void ScreenShareController::announceStream(bool on)
{
    // S-7: an explicit end-of-stream. Without it a viewer holds the
    // last frame for the 4 s liveness timeout and then shows
    // "Starting share…" until the ~5 s roster poll disagrees.
    if (auto* voice = currentVoice())
        voice->announceVideoStreamState(VideoStreamId::Screen, on);
}

void ScreenShareController::setTransmitting(bool transmitting)
{
    if (m_transmitting == transmitting) return;
    m_transmitting = transmitting;
    emit transmittingChanged();
}

void ScreenShareController::toggle()
{
    if (m_active) stop(); else start();
}

void ScreenShareController::showPicker()
{
    applyEffectiveQuality(m_settings, m_servers);
#ifdef Q_OS_MACOS
    m_lastError.clear();
    emit lastErrorChanged();
    m_mac->setFps(1000 / g_frameIntervalMs);
    if (!m_throttle->isActive()) {
        m_throttle->setInterval(g_frameIntervalMs);
        m_throttle->start();
    } else {
        m_throttle->setInterval(g_frameIntervalMs);
    }
    m_mac->showPicker();
#else
    start();
#endif
}

void ScreenShareController::forwardTo(QVideoSink* sink)
{
    if (!sink) return;
    connect(m_sink, &QVideoSink::videoFrameChanged, sink,
        [sink](const QVideoFrame& f) { sink->setVideoFrame(f); });
    // Replay whatever is on the internal sink right now, the same way
    // VideoStreamRegistry::attachOutput does for remote streams. A
    // second surface on an already-running preview (a pop-out, a
    // fullscreen window) would otherwise be black until the next
    // capture tick — a third of a second at the 3 fps a static share
    // settles to. `sink` is the connection's context object, so both
    // the mirror and this replay die with it.
    const QVideoFrame current = m_sink->videoFrame();
    if (current.isValid()) sink->setVideoFrame(current);
}

void ScreenShareController::openSystemSettings()
{
#ifdef Q_OS_MACOS
    QProcess::startDetached("open", {
        "x-apple.systempreferences:com.apple.preference.security?"
        "Privacy_ScreenCapture"
    });
#endif
}

void ScreenShareController::pushFrameToPeers()
{
    if (!m_active) {
        setTransmitting(false);
        return;
    }
    // Resolve the connection that's actually in voice — NOT the
    // active (sidebar-focused) server, which may be a different one
    // the user is just browsing while sharing.
    // Interface, not VoiceEngine: capture + encode are transport-agnostic,
    // so this controller only needs the send-side sink. It never touches
    // mesh signalling.
    IVoiceTransport* voice = nullptr;
    if (m_servers) {
        if (auto* vs = m_servers->voiceServer()) voice = vs->voiceEngine();
    }
    static int s_tickCount = 0;
    if (++s_tickCount % 25 == 1) {
        qInfo("[screenshare] push tick #%d: voice=%p frameValid=%d",
              s_tickCount, (void*)voice, int(m_pendingFrame.isValid()));
    }
    // Transmitting = a live engine with ≥1 open peer channel will
    // receive this frame. Anything less means peers can't see us.
    const bool canTransmit = voice && voice->hasOpenPeers();
    if (!canTransmit) setTransmitting(false);
    if (!voice) return;
    if (!m_pendingFrame.isValid()) return;

    // Engines are per-voice-session; (re)wire this one's keyframe
    // demands (RTCP PLI, app-level "kf", late-joiner track opens) to
    // the encoder the first time we push a frame at it.
    if (m_wiredEngine != voice) {
        if (m_wiredEngine) {
            disconnect(m_wiredEngine, nullptr, m_pipeline, nullptr);
            disconnect(m_wiredEngine, nullptr, m_rate, nullptr);
        }
        connect(voice, &IVoiceTransport::videoKeyframeRequested, m_pipeline,
            [this](int streamId) {
                if (streamId == int(VideoStreamId::Screen))
                    m_pipeline->forceKeyframe();
            });
        // Feed the rate controller: per-peer delivery ratios and
        // keyframe-request pressure for our screen stream.
        connect(voice, &IVoiceTransport::videoDeliveryReport, m_rate,
            [this](const QString& userId, int streamId,
                   const VideoDeliveryReport& r) {
                if (streamId == int(VideoStreamId::Screen))
                    m_rate->reportDelivery(userId, r);
            });
        connect(voice, &IVoiceTransport::videoKeyframeRequested, m_rate,
            [this](int streamId) {
                if (streamId == int(VideoStreamId::Screen))
                    m_rate->reportKeyframeRequest();
            });
        // Transport can't deliver lossless frames (e.g. oversized for
        // the peer's channel cap) — force H.264 for the rest of the
        // share and tell the user, instead of streaming nothing.
        connect(voice, &IVoiceTransport::losslessSendUnavailable, this,
            [this]() {
                if (g_losslessBroken) return;
                g_losslessBroken = true;
                qWarning("[screenshare] lossless undeliverable — "
                         "falling back to H.264");
                m_lastError = tr("Lossless mode isn't deliverable to a "
                                 "viewer — switched to H.264 for this share.");
                emit lastErrorChanged();
            });
        m_wiredEngine = voice;
        // The engine can be created AFTER the share started (share
        // first, join voice second), so the "stream on" this session
        // never heard about is replayed here (S-7).
        if (m_active)
            voice->announceVideoStreamState(VideoStreamId::Screen, true);
    }

    // RTP path: hand the raw frame to the encode worker. Capable
    // peers get real H.264; adding tracks (with renegotiation) is
    // kicked off lazily here so a share started before any capable
    // peer joined still upgrades the moment one appears.
    if (voice->hasVideoCapablePeers()) {
        voice->prepareVideoSend();
        // True lossless (AV1 identity-I444 over the reliable channel)
        // requires: user toggle + server policy + EVERY capable peer
        // advertising av1-dc. Mixed fleets fall back to H.264 for
        // everyone rather than running two encoders.
        const bool lossless = g_losslessWanted && g_losslessAllowed
            && !g_losslessBroken && voice->allPeersSupportLossless();
        if (lossless) {
            m_rate->setActive(false);   // bitrate is content-determined
            // Admission control: never queue latency — drop this
            // capture frame while any peer's channel is backed up.
            if (voice->losslessBackpressure(kLosslessBufferBudget)) {
                setTransmitting(canTransmit);
                m_pendingFrame = {};
                return;
            }
            EncoderConfig cfg = g_encoderConfig;
            cfg.codec = VideoCodecKind::Av1Lossless;
            cfg.lossless = true;
            m_pipeline->configure(cfg);
            m_pipeline->submitFrame(m_pendingFrame,
                                    QDateTime::currentMSecsSinceEpoch() * 1000);
        } else {
            // Emit the best profile every current receiver decodes — a
            // profile flip rebuilds the encoder session and IDRs.
            g_encoderConfig.profile = voice->negotiatedH264Profile();
            // S-18: and the best CODEC every current receiver decodes.
            // Same mechanism, same tick: a changed codec is not
            // sameSessionAs the running encoder session, so the
            // pipeline rebuilds it and the next frame out is an IDR —
            // which is exactly what a viewer that just forced the
            // switch by joining needs. Tell the rate controller first,
            // so the new codec's quality floors govern the next ladder
            // decision rather than the previous codec's.
            g_encoderConfig.codec = voice->negotiatedVideoCodec(
                videocodec::preferenceFromString(
                    m_settings ? m_settings->videoCodecPreference() : QString()));
            m_rate->setCodec(g_encoderConfig.codec);
            // Rate-controller outputs override the static envelope:
            // the governor moves bitrate live and steps the resolution
            // ladder down/up with the measured delivery ratio.
            m_rate->setEnvelope(250, g_encoderConfig.maxBitrateKbps,
                                g_encoderConfig.fps, g_maxWidth);
            m_rate->setActive(true);
            EncoderConfig cfg = g_encoderConfig;
            cfg.targetBitrateKbps = m_rate->targetKbps();
            cfg.maxBitrateKbps = m_rate->maxKbps();
            cfg.width = cfg.height = qMin(g_maxWidth, m_rate->longEdge());
            // Screen content gives up FRAME RATE before resolution —
            // in-game text at 15 fps is readable and the same text at
            // half the long edge is not — so the ladder's fps is an
            // output now, not the static setting.
            cfg.fps = qMin(g_encoderConfig.fps, m_rate->fps());
            // S-15: keep every peer's RTP pacer above what this config
            // allows the encoder to emit. A pacer budget below it does
            // not shave peaks, it accumulates them.
            voice->setVideoSendCeiling(VideoStreamId::Screen,
                                       cfg.maxBitrateKbps);
            m_pipeline->configure(cfg);
            m_pipeline->submitFrame(m_pendingFrame,
                                    QDateTime::currentMSecsSinceEpoch() * 1000);
        }
    }

    // Legacy JPEG path — only when someone still needs it (no open
    // video track): old clients, Android, or a capable peer whose
    // renegotiation hasn't finished yet. Subsampled to ~5 fps (the
    // rate this path was designed for — the camera path has the same
    // divisor): at the RTP-oriented 30-60 fps capture cadence, per-
    // tick JPEG broadcast firehoses tens of Mbit/s at the data
    // channel, whose backpressure then drops frames in bursts — the
    // viewer sees clumpy, flickering playback instead of a slow but
    // steady slideshow.
    const int legacyDivisor = qMax(1, g_encoderConfig.fps / 5);
    if (voice->hasLegacyOpenPeers(VideoStreamId::Screen)
        && (s_tickCount % legacyDivisor) == 0) {
        QImage img = m_pendingFrame.toImage();
        if (img.isNull()) {
            if (s_tickCount % 25 == 1)
                qWarning("[screenshare] toImage() returned null, frame format=%d",
                         int(m_pendingFrame.pixelFormat()));
            return;
        }

        // The user's resolution/quality settings size the RTP encoder;
        // the legacy JPEG rides a data channel whose SCTP message cap
        // is ~256 KB, and libdatachannel refuses (throws on) anything
        // larger. Clamp this path to dimensions/quality that reliably
        // fit — 1600 px Q75 tops out around 200 KB — instead of
        // letting a 3840 px Q100 setting produce ~1.5 MB frames that
        // can never be sent.
        const int legacyMaxW = qMin(g_maxWidth, 1600);
        const int legacyQ    = qMin(g_jpegQuality, 75);

        // S-10: encode off the GUI thread. A 1600 px Q75 encode is
        // milliseconds of solid CPU and it ran in the timer slot, so
        // every legacy viewer cost the sharer UI smoothness. The
        // broadcast happens back on this thread, where the transport
        // lives — and the engine is re-resolved there because a voice
        // session can end while a frame is in flight.
        if (!m_jpegWorker) {
            m_jpegWorker = std::make_unique<LatestWinsWorker>(
                QStringLiteral("video-jpeg-enc"));
        }
        const int tick = s_tickCount;
        m_jpegWorker->submit([this, img, legacyMaxW, legacyQ, tick]() {
            QImage scaled = img.width() > legacyMaxW
                ? img.scaledToWidth(legacyMaxW, Qt::SmoothTransformation)
                : img;
            QByteArray jpeg;
            {
                QBuffer buf(&jpeg);
                buf.open(QIODevice::WriteOnly);
                if (!scaled.save(&buf, "JPEG", legacyQ)) return;
            }
            QMetaObject::invokeMethod(this, [this, jpeg, tick]() {
                auto* transport = currentVoice();
                if (!transport) return;
                transport->broadcastScreenFrame(jpeg);
                if (tick % 25 == 1)
                    qInfo("[screenshare] broadcast %d-byte JPEG (tick #%d)",
                          int(jpeg.size()), tick);
            }, Qt::QueuedConnection);
        });
    }

    setTransmitting(canTransmit);
    m_pendingFrame = {};
}
