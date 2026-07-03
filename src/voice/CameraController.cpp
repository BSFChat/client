#include "voice/CameraController.h"
#include "voice/VoiceEngine.h"
#include "net/ServerManager.h"
#include "net/ServerConnection.h"
#include "core/Settings.h"

#ifdef Q_OS_MACOS
#include "voice/MacCameraPermission.h"
#include "voice/MacCameraCapturer.h"
#else
#include <QMediaDevices>
#include <QCameraDevice>
#endif

#include <QVariantMap>
#include <QBuffer>
#include <QImage>
#include <QDebug>
#include <QVideoFrameFormat>
#include <algorithm>
#include <cstring>

static constexpr int kFrameIntervalMs = 200;
static constexpr int kJpegQuality = 60;
static constexpr int kMaxWidth = 640;

CameraController::CameraController(QObject* parent)
    : QObject(parent)
    , m_sink(new QVideoSink(this))
    , m_throttle(new QTimer(this))
{
#ifdef Q_OS_MACOS
    // AVFoundation-direct capture. Homebrew Qt lacks the
    // QCameraPermission plugin, so QCamera just silently refuses to
    // start even with TCC granted. Skip it entirely.
    m_mac = new MacCameraCapturer(this);
    connect(m_mac, &MacCameraCapturer::frameReady, this,
        [this](const QImage& img) {
            if (!m_active) {
                m_active = true;
                emit activeChanged();
            }
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
    m_camera = new QCamera(this);
    m_session = new QMediaCaptureSession(this);
    m_session->setCamera(m_camera);
    m_session->setVideoSink(m_sink);

    connect(m_camera, &QCamera::activeChanged, this, [this](bool a) {
        if (m_active == a) return;
        m_active = a;
        emit activeChanged();
    });
    connect(m_camera, &QCamera::errorOccurred, this, [this](QCamera::Error err,
                                                             const QString& d) {
        Q_UNUSED(err);
        if (d.isEmpty()) return;
        m_lastError = d;
        emit lastErrorChanged();
    });
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
        [this](const QVideoFrame& f) { m_pendingFrame = f; });
#endif

    m_throttle->setInterval(kFrameIntervalMs);
    connect(m_throttle, &QTimer::timeout, this,
            &CameraController::pushFrameToPeers);
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
    if (m_active) return;

#ifdef Q_OS_MACOS
    auto s = mac_camera_permission::status();
    qInfo("[camera] macOS camera TCC status=%s", qUtf8Printable(s));
    if (s == "denied" || s == "restricted") {
        m_lastError = "Camera access denied. Grant it in System "
                      "Settings → Privacy & Security → Camera, then "
                      "restart BSFChat.";
        emit lastErrorChanged();
        return;
    }
    if (s == "undetermined") {
        mac_camera_permission::request([this, index](bool granted) {
            if (!granted) {
                m_lastError = "Camera access denied.";
                emit lastErrorChanged();
                return;
            }
            startForCamera(index);
        });
        return;
    }
    m_lastError.clear();
    emit lastErrorChanged();
    m_mac->start(index);
    m_cameraDescription = m_mac->currentDescription();
    emit cameraDescriptionChanged();
    m_throttle->start();
#else
    const auto cams = QMediaDevices::videoInputs();
    if (cams.isEmpty()) {
        m_lastError = "No camera detected.";
        emit lastErrorChanged();
        return;
    }
    QCameraDevice target = (index >= 0 && index < cams.size())
        ? cams[index] : QMediaDevices::defaultVideoInput();
    m_cameraDescription = target.description();
    emit cameraDescriptionChanged();
    m_camera->setCameraDevice(target);
    m_lastError.clear();
    emit lastErrorChanged();
    m_camera->start();
    m_throttle->start();
#endif
}

void CameraController::stop()
{
    m_throttle->stop();
    m_pendingFrame = {};
    setTransmitting(false);
    // Push an empty frame so QML previews blank out instead of
    // keeping the frozen last frame.
    m_sink->setVideoFrame(QVideoFrame());
#ifdef Q_OS_MACOS
    if (m_mac) m_mac->stop();
    if (m_active) {
        m_active = false;
        emit activeChanged();
    }
#else
    if (m_camera && m_camera->isActive()) m_camera->stop();
#endif
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
    VoiceEngine* voice = nullptr;
    if (m_servers) {
        if (auto* vs = m_servers->voiceServer()) voice = vs->voiceEngine();
    }
    const bool canTransmit = voice && voice->hasOpenPeers();
    if (!canTransmit) setTransmitting(false);
    if (!voice) return;
    if (!m_pendingFrame.isValid()) return;

    QImage img = m_pendingFrame.toImage();
    if (img.isNull()) return;
    if (img.width() > kMaxWidth)
        img = img.scaledToWidth(kMaxWidth, Qt::SmoothTransformation);

    QByteArray jpeg;
    {
        QBuffer buf(&jpeg);
        buf.open(QIODevice::WriteOnly);
        if (!img.save(&buf, "JPEG", kJpegQuality)) return;
    }
    voice->broadcastCameraFrame(jpeg);
    setTransmitting(canTransmit);
    m_pendingFrame = {};
}
