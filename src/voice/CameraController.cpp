#include "voice/IpPrivacy.h"
#include "voice/CameraController.h"

#include "voice/video/VideoCodecPreference.h"
#include "voice/IVoiceTransport.h"
#include "voice/VoiceEngine.h"
#include "voice/video/VideoRateController.h"
#include "voice/video/VideoSendPipeline.h"
#include "net/ServerManager.h"
#include "net/ServerConnection.h"
#include "core/Settings.h"

#include "voice/video/VideoFrameOrientation.h"

#ifdef Q_OS_MACOS
#include "voice/MacCameraPermission.h"
#include "voice/MacCameraCapturer.h"
#else
#include <QCamera>
#include <QCameraDevice>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#endif

#if defined(Q_OS_IOS)
#include <QGuiApplication>
#include <QPermissions>
#endif

#include <QDateTime>
#include <QVariantMap>
#include <QBuffer>
#include <QImage>
#include <QTransform>
#include <QDebug>
#include <QVideoFrameFormat>
#include <algorithm>
#include <cstring>

// Capture ticks at RTP rate (interval re-resolved from Settings on
// every start); the JPEG numbers only apply to the legacy branch,
// which subsamples the ticks back down to ~5 fps.
static constexpr int kFrameIntervalMs = 33;
static constexpr int kJpegQuality = 60;
static constexpr int kMaxWidth = 640;
// RTP camera fallbacks when Settings isn't wired.
static constexpr int kRtpMaxLongEdge = 1280;
static constexpr int kRtpFps = 30;
static constexpr int kRtpTargetKbps = 1500;

CameraController::CameraController(QObject* parent)
    : QObject(parent)
    , m_sink(new QVideoSink(this))
    , m_throttle(new QTimer(this))
{
#ifdef Q_OS_MACOS
    // AVFoundation-direct capture. Homebrew Qt lacks the
    // QCameraPermission plugin, so QCamera just silently refuses to
    // start even with TCC granted. Skip it entirely.
    //
    // Constructing it here is safe — MacCameraCapturer's constructor is
    // empty and it opens no device until start(). The QCamera branch
    // below is NOT safe to construct here, which is why it moved to
    // ensureCaptureSession(); see the class note in the header.
    m_mac = new MacCameraCapturer(this);
    connect(m_mac, &MacCameraCapturer::frameReady, this,
        [this](const QImage& img) {
            if (!m_active) setActiveState(true);
            // Expose the frame through our internal QVideoSink so
            // QML VideoOutputs mirrored via forwardTo() render it.
            QVideoFrameFormat fmt(img.size(),
                QVideoFrameFormat::pixelFormatFromImageFormat(img.format()));
            QVideoFrame vf(fmt);
            if (vf.map(QVideoFrame::WriteOnly)) {
                std::memcpy(vf.bits(0), img.bits(),
                    size_t(img.bytesPerLine()) * size_t(img.height()));
                vf.unmap();
                m_sink->setVideoFrame(vf);
            }
            m_pendingFrame = vf;
        });
    connect(m_mac, &MacCameraCapturer::captureFailed, this,
        [this](const QString& desc) {
            m_lastError = desc;
            emit lastErrorChanged();
        });
#else
    // The QCamera / QMediaCaptureSession pair is built on first use in
    // ensureCaptureSession(), NOT here. Only the sink wiring is safe at
    // construction time — a QVideoSink is a frame destination and owns
    // no device.
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
        [this](const QVideoFrame& f) { m_pendingFrame = f; });
#endif

    m_throttle->setInterval(kFrameIntervalMs);
    connect(m_throttle, &QTimer::timeout, this,
            &CameraController::pushFrameToPeers);

    // RTP encode worker + adaptive governor for the vcamera track —
    // the same pair ScreenShareController runs for vscreen.
    m_pipeline = new VideoSendPipeline(VideoStreamId::Camera, this);
    connect(m_pipeline, &VideoSendPipeline::encodedFrameReady, this,
        [this](int, const EncodedFrame& frame) {
            if (!m_servers) return;
            if (auto* vs = m_servers->voiceServer()) {
                if (auto* voice = vs->voiceEngine())
                    voice->broadcastEncodedVideo(VideoStreamId::Camera, frame);
            }
        });
    m_rate = new VideoRateController(VideoStreamId::Camera, this);
    connect(m_rate, &VideoRateController::forceKeyframe,
            m_pipeline, &VideoSendPipeline::forceKeyframe);
    // Measured packets per frame is what turns the receivers' packet
    // loss into frame damage (videorate::frameDamagePct); encode
    // pressure lets an overloaded machine shed resolution.
    m_rate->setSendWindowSource([this](int askedFps) {
        return m_sendStats.sample(m_pipeline->takeCounters(),
                                  QDateTime::currentMSecsSinceEpoch(),
                                  askedFps, /*capturePolled=*/false);
    });
}

void CameraController::ensureCaptureSession()
{
#ifndef Q_OS_MACOS
    if (m_camera) return;
    m_camera = new QCamera(this);
    m_session = new QMediaCaptureSession(this);
    m_session->setCamera(m_camera);
    m_session->setVideoSink(m_sink);

    connect(m_camera, &QCamera::activeChanged, this, [this](bool a) {
        setActiveState(a);
    });
    connect(m_camera, &QCamera::errorOccurred, this, [this](QCamera::Error err,
                                                             const QString& d) {
        Q_UNUSED(err);
        if (d.isEmpty()) return;
        failWith(d);
    });
#endif
}

camperm::Status CameraController::cameraPermission() const
{
#if defined(Q_OS_MACOS)
    // AVFoundation directly — Homebrew Qt has no QCameraPermission
    // plugin, and without it Qt answers Denied for everything.
    const QString s = mac_camera_permission::status();
    if (s == QLatin1String("granted"))    return camperm::Status::Granted;
    if (s == QLatin1String("denied"))     return camperm::Status::Denied;
    if (s == QLatin1String("restricted")) return camperm::Status::Restricted;
    return camperm::Status::Undetermined;
#elif defined(Q_OS_IOS) && QT_CONFIG(permissions)
    // Qt's QDarwinCameraPermission backend IS linked on iOS — Qt decides
    // that at configure time by finding NSCameraUsageDescription in
    // ios/Info.plist.in (see the note at the top of that file), so unlike
    // the Homebrew Mac the public API is the right one to use here.
    //
    // iOS has no separate "restricted" in Qt's enum; a restricted device
    // reports Denied, and camperm::action() refuses both, so the only
    // difference is which sentence the user reads.
    switch (qApp->checkPermission(QCameraPermission{})) {
    case Qt::PermissionStatus::Granted:      return camperm::Status::Granted;
    case Qt::PermissionStatus::Denied:       return camperm::Status::Denied;
    case Qt::PermissionStatus::Undetermined: return camperm::Status::Undetermined;
    }
    return camperm::Status::Unsupported;
#else
    // Windows / Linux / Android. Android asks through its own JNI bridge
    // in QML before it ever calls start() (androidPerms.requestCamera),
    // and the desktops either have no such concept or answer it inside
    // the capture backend. Proceeding preserves exactly today's
    // behaviour on all of them.
    return camperm::Status::Unsupported;
#endif
}

void CameraController::requestCameraPermission(std::function<void(bool)> done)
{
#if defined(Q_OS_MACOS)
    mac_camera_permission::request(std::move(done));
#elif defined(Q_OS_IOS) && QT_CONFIG(permissions)
    qApp->requestPermission(QCameraPermission{}, this,
        [done = std::move(done)](const QPermission& result) {
            done(result.status() == Qt::PermissionStatus::Granted);
        });
#else
    done(true);
#endif
}

void CameraController::failWith(const QString& message)
{
    m_lastError = message;
    emit lastErrorChanged();
}

QVariantList CameraController::availableCameras() const
{
#ifdef Q_OS_MACOS
    return m_mac ? m_mac->availableCameras() : QVariantList{};
#else
    QVariantList out;
    const auto cams = QMediaDevices::videoInputs();
    for (int i = 0; i < cams.size(); ++i) {
        QVariantMap m;
        m[QStringLiteral("index")] = i;
        m[QStringLiteral("description")] = cams[i].description();
        m[QStringLiteral("id")] = QString::fromUtf8(cams[i].id());
        m[QStringLiteral("isDefault")] = cams[i].isDefault();
        out.append(m);
    }
    return out;
#endif
}

void CameraController::setServerManager(ServerManager* mgr)
{
    if (m_servers == mgr) return;
    m_servers = mgr;
    if (!mgr) return;
    // Re-evaluate the per-server signal bindings whenever the server
    // list or active server changes, then prime them.
    connect(mgr, &ServerManager::activeServerChanged,
            this, &CameraController::rewireVoiceLeaveWatch);
    connect(mgr, &ServerManager::serverAdded,
            this, &CameraController::rewireVoiceLeaveWatch);
    connect(mgr, &ServerManager::serverRemoved,
            this, &CameraController::rewireVoiceLeaveWatch);
    rewireVoiceLeaveWatch();
}

void CameraController::rewireVoiceLeaveWatch()
{
    // Drop prior subscriptions before adding new ones — a removed
    // server's lambda would capture a dangling connection pointer.
    // Tracking the QMetaObject::Connections avoids that.
    for (const auto& conn : m_voiceRoomConns) disconnect(conn);
    m_voiceRoomConns.clear();
    if (!m_servers) return;
    // Watch EVERY connection, not just the active one — the user can
    // be in voice on server A while browsing server B, and the camera
    // must stop when the voice session (wherever it lives) ends.
    for (int i = 0; i < m_servers->connectionCount(); ++i) {
        auto* sc = m_servers->connectionAt(i);
        if (!sc) continue;
        m_voiceRoomConns.append(
            connect(sc, &ServerConnection::activeVoiceRoomIdChanged,
                    this, [this]() {
            // If no connection is in voice anymore and the camera is
            // running, stop it — otherwise it would silently
            // re-broadcast to whatever voice channel is joined next.
            // Camera state shouldn't outlive the call.
            if (m_active && !m_servers->voiceServer()) {
                stop();
            }
        }));
    }
}

void CameraController::start() { startForCamera(-1); }

void CameraController::startForCamera(int index)
{
    // "Hide my IP while sharing", checked BEFORE anything starts. A share that
    // began and then found there was no relay would be a share that silently
    // did not hide the address it promised to hide — the one outcome this
    // whole feature exists to prevent. Refuse with a reason instead.
    if (m_hideIpForShare && !canHideIpWhileSharing()) {
        failWith(voice::relayRefusalMessage(voice::RelaySource::ShareOption));
        return;
    }

    if (m_active) return;
    // Capture cadence follows the user's camera fps (legacy JPEG
    // subsamples from it); re-resolved on every start.
    m_throttle->setInterval(1000 / (m_settings ? m_settings->cameraFps()
                                               : kRtpFps));

    // ---- Permission, asked HERE and nowhere earlier ------------------
    //
    // This is the first line of the first function that the camera
    // button can reach, and it is the only place in the class that asks.
    // macOS and iOS share the rule (camperm::action) and differ only in
    // which API answers it; every other platform reports Unsupported and
    // falls straight through to Proceed.
    const camperm::Status perm = cameraPermission();
    switch (camperm::action(perm)) {
    case camperm::Action::Refuse:
        qInfo("[camera] permission refuses the start (status=%d)", int(perm));
        failWith(camperm::refusalMessage(perm));
        return;
    case camperm::Action::RequestThenStart:
        qInfo("[camera] requesting camera permission at first video use");
        requestCameraPermission([this, index](bool granted) {
            if (!granted) {
                // Deliberately re-read the status rather than assuming
                // Denied: on iOS a prompt can also be dismissed by a
                // restriction profile, and the two need different words.
                failWith(camperm::refusalMessage(cameraPermission()));
                return;
            }
            // Re-enter with the decision on record; the status is now
            // Granted, so this lands on Proceed.
            startForCamera(index);
        });
        return;
    case camperm::Action::Proceed:
        break;
    }

    ensureCaptureSession();
    m_lastError.clear();
    emit lastErrorChanged();

#ifdef Q_OS_MACOS
    m_mac->start(index);
    m_cameraDescription = m_mac->currentDescription();
    emit cameraDescriptionChanged();
    m_throttle->start();
#else
    // Enumeration happens here and not in the constructor. On iOS an
    // AVCaptureDevice discovery does not itself raise the permission
    // prompt, but it is still device work on the launch path, and the
    // rule this class follows is the simple one: nothing before the
    // button.
    const auto cams = QMediaDevices::videoInputs();
    if (cams.isEmpty()) {
        failWith(QStringLiteral("No camera detected."));
        return;
    }
    QCameraDevice target = (index >= 0 && index < cams.size())
        ? cams[index] : QMediaDevices::defaultVideoInput();
    m_cameraDescription = target.description();
    emit cameraDescriptionChanged();
    m_camera->setCameraDevice(target);
    m_camera->start();
    m_throttle->start();
#endif
}

void CameraController::stop()
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
    // Null until the first start — stopping a camera that was never
    // built is a no-op, not a crash.
    // QCamera::activeChanged routes the flip through setActiveState().
    if (m_camera && m_camera->isActive()) m_camera->stop();
#endif
}

void CameraController::setActiveState(bool active)
{
    if (m_active == active) return;
    m_active = active;
    // S-11: the encode session outlives a stop/start, so the first
    // frame of a restarted camera would reference a picture no viewer
    // holds. Start clean.
    if (active && m_pipeline) m_pipeline->forceKeyframe();
    // Before the announcement — see the same call in ScreenShareController.
    applyShareIpPrivacy(active);
    announceStream(active);
    emit activeChanged();
}

IVoiceTransport* CameraController::currentVoice() const
{
    if (!m_servers) return nullptr;
    auto* vs = m_servers->voiceServer();
    return vs ? vs->voiceEngine() : nullptr;
}


bool CameraController::canHideIpWhileSharing() const
{
    // No voice session yet means the question is unanswerable, and the honest
    // default is "yes, as far as we know": the option stays offered, and the
    // check that actually matters runs at start, against a live engine.
    auto* voice = currentVoice();
    return !voice || voice->canHideIpAddress();
}

void CameraController::applyShareIpPrivacy(bool on)
{
    if (auto* voice = currentVoice())
        voice->setStreamIpPrivacy(VideoStreamId::Camera, on && m_hideIpForShare);
}

void CameraController::announceStream(bool on)
{
    // S-7: explicit lifecycle, so a viewer's camera tile clears the
    // moment the camera goes off rather than after the liveness timeout.
    if (auto* voice = currentVoice())
        voice->announceVideoStreamState(VideoStreamId::Camera, on);
}

void CameraController::setTransmitting(bool transmitting)
{
    if (m_transmitting == transmitting) return;
    m_transmitting = transmitting;
    emit transmittingChanged();
}

void CameraController::toggle()
{
    if (m_active) stop(); else start();
}

void CameraController::forwardTo(QVideoSink* sink)
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

void CameraController::pushFrameToPeers()
{
    if (!m_active) {
        setTransmitting(false);
        return;
    }
    // Resolve the connection that's actually in voice — NOT the
    // active (sidebar-focused) server, which may be a different one
    // the user is just browsing while broadcasting.
    // See the note in ScreenShareController::pushFrameToPeers — the
    // send-side interface is all this needs.
    IVoiceTransport* voice = nullptr;
    if (m_servers) {
        if (auto* vs = m_servers->voiceServer()) voice = vs->voiceEngine();
    }
    const bool canTransmit = voice && voice->hasOpenPeers();
    if (!canTransmit) setTransmitting(false);
    if (!voice) return;
    if (!m_pendingFrame.isValid()) return;
    ++m_tick;

    // (Re)wire this session's engine: keyframe demands and delivery
    // reports for the camera stream feed the pipeline/governor.
    if (m_wiredEngine != voice) {
        if (m_wiredEngine) {
            disconnect(m_wiredEngine, nullptr, m_pipeline, nullptr);
            disconnect(m_wiredEngine, nullptr, m_rate, nullptr);
        }
        connect(voice, &IVoiceTransport::videoKeyframeRequested, m_pipeline,
            [this](int streamId) {
                if (streamId == int(VideoStreamId::Camera))
                    m_pipeline->forceKeyframe();
            });
        connect(voice, &IVoiceTransport::videoDeliveryReport, m_rate,
            [this](const QString& userId, int streamId,
                   const VideoDeliveryReport& r) {
                if (streamId == int(VideoStreamId::Camera))
                    m_rate->reportDelivery(userId, r);
            });
        connect(voice, &IVoiceTransport::videoKeyframeRequested, m_rate,
            [this](int streamId) {
                if (streamId == int(VideoStreamId::Camera))
                    m_rate->reportKeyframeRequest();
            });
        m_wiredEngine = voice;
        // Engine created after the camera was switched on — replay the
        // "stream on" this session never heard (S-7).
        if (m_active)
            voice->announceVideoStreamState(VideoStreamId::Camera, true);
    }

    // RTP path (capable peers). User knobs with hardcoded fallbacks
    // when Settings isn't wired (tests).
    if (voice->hasVideoCapablePeers()) {
        voice->prepareVideoSend();
        const int fps = m_settings ? m_settings->cameraFps() : kRtpFps;
        const int maxEdge = m_settings ? m_settings->cameraMaxWidth()
                                       : kRtpMaxLongEdge;
        const int targetKbps = m_settings ? m_settings->cameraTargetKbps()
                                          : kRtpTargetKbps;
        // The configured target is the rate controller's CEILING, and
        // headroom above it is what lets a clean path actually reach
        // the number the user chose instead of orbiting half of it.
        m_rate->setEnvelope(150, targetKbps, fps, maxEdge);
        m_rate->setActive(true);
        EncoderConfig cfg;
        // S-18: asked every tick, exactly like the H.264 profile below.
        // The answer changes when someone joins or leaves, and a
        // changed codec is not sameSessionAs the running session, so
        // VideoSendPipeline rebuilds the encoder and emits an IDR. The
        // rate controller is told first so the new codec's (lower)
        // quality floors govern the very next ladder decision.
        cfg.codec = voice->negotiatedVideoCodec(
            videocodec::preferenceFromString(
                m_settings ? m_settings->videoCodecPreference() : QString()));
        m_rate->setCodec(cfg.codec);
        // Camera content gives up RESOLUTION before frame rate — a
        // soft face reads as fine, a stuttering one reads as broken.
        cfg.fps = qMin(fps, m_rate->fps());
        cfg.screenContent = false;   // camera tuning: motion over text
        cfg.profile = voice->negotiatedH264Profile();
        cfg.targetBitrateKbps = m_rate->targetKbps();
        cfg.maxBitrateKbps = m_rate->maxKbps();
        cfg.width = cfg.height = qMin(maxEdge, m_rate->longEdge());
        cfg.keyframeIntervalSec = 10;   // background refresh only —
                                        // receivers PLI when they need one
        // S-15: pacer ceiling follows the encoder's, never sits below.
        voice->setVideoSendCeiling(VideoStreamId::Camera, cfg.maxBitrateKbps);
        m_pipeline->configure(cfg);
        m_pipeline->submitFrame(m_pendingFrame,
                                QDateTime::currentMSecsSinceEpoch() * 1000);
    }

    // Legacy JPEG path, subsampled from the capture cadence back
    // down to ~5 fps.
    const int legacyDivisor = qMax(1,
        (m_settings ? m_settings->cameraFps() : kRtpFps) / 5);
    if (voice->hasLegacyOpenPeers(VideoStreamId::Camera)
        && (m_tick % legacyDivisor) == 0) {
        QImage img = m_pendingFrame.toImage();
        if (img.isNull()) return;
        // Same orientation fix the RTP path gets inside
        // FrameConverter::toI420, applied here too because
        // QVideoFrame::toImage() returns the raw pixels and drops the
        // presentation rotation with them. Without this a phone's legacy
        // peers — which is every peer for the first seconds of a call,
        // before the RTP track opens — see a sideways picture.
        // Zero-cost on desktop, where the angle is always 0.
        const int deg = videoorient::wireRotation(
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
            int(m_pendingFrame.rotation())
#else
            int(m_pendingFrame.surfaceFormat().rotationAngle())
#endif
        );
        if (deg != 0)
            img = img.transformed(QTransform().rotate(deg),
                                  Qt::SmoothTransformation);
        if (img.width() > kMaxWidth)
            img = img.scaledToWidth(kMaxWidth, Qt::SmoothTransformation);

        QByteArray jpeg;
        {
            QBuffer buf(&jpeg);
            buf.open(QIODevice::WriteOnly);
            if (!img.save(&buf, "JPEG", kJpegQuality)) return;
        }
        voice->broadcastCameraFrame(jpeg);
    }

    setTransmitting(canTransmit);
    m_pendingFrame = {};
}
