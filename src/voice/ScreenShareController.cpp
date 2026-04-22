#include "voice/ScreenShareController.h"
#include "voice/VoiceEngine.h"
#include "net/ServerManager.h"
#include "net/ServerConnection.h"

#ifdef Q_OS_MACOS
#include "voice/MacScreenCapturer.h"
#endif

#include "core/Settings.h"
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
                "Screen capture refused (\"%1\"). On an adhoc-signed "
                "dev build the TCC grant is bound to the binary's code "
                "signature hash — rebuilding invalidates it even though "
                "the toggle still looks on. Fix: open System Settings → "
                "Privacy & Security → Screen & System Audio Recording, "
                "remove the bsfchat-app entry (use the \"−\" button), "
                "quit BSFChat, relaunch, click Share once to get a "
                "fresh prompt, grant it, quit + relaunch one more time."
            ).arg(desc);
            emit lastErrorChanged();
            stop();
        });
    connect(m_mac, &MacScreenCapturer::frameReady, this,
        [this](const QImage& img) {
            if (!m_active) {
                // First frame — flip active state so QML preview shows.
                m_active = true;
                emit activeChanged();
            }
            // Feed the internal sink so any VideoOutput mirroring via
            // forwardTo() receives frames. Construct a QVideoFrame
            // from the QImage via a frame format that matches.
            QVideoFrameFormat fmt(img.size(),
                QVideoFrameFormat::pixelFormatFromImageFormat(img.format()));
            QVideoFrame vf(fmt);
            if (vf.map(QVideoFrame::WriteOnly)) {
                // One plane, RGB image.
                std::memcpy(vf.bits(0), img.bits(),
                            size_t(img.bytesPerLine()) * size_t(img.height()));
                vf.unmap();
                m_sink->setVideoFrame(vf);
            }
            // Cache the latest image for peer push — skip the
            // QVideoFrame→QImage conversion the other OSes need.
            m_pendingFrame = vf;
        });
#else
    m_capture = new QScreenCapture(this);
    m_session = new QMediaCaptureSession(this);
    m_session->setScreenCapture(m_capture);
    m_session->setVideoSink(m_sink);

    connect(m_capture, &QScreenCapture::activeChanged, this, [this](bool a) {
        if (m_active == a) return;
        m_active = a;
        qInfo("[screenshare] active=%d", int(a));
        emit activeChanged();
    });
    connect(m_capture, &QScreenCapture::errorOccurred, this,
        [this](QScreenCapture::Error err, const QString& description) {
            Q_UNUSED(err);
            if (description.isEmpty()) return;
            m_lastError = description;
            qWarning("[screenshare] error: %s", qUtf8Printable(description));
            emit lastErrorChanged();
        });
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
        [this](const QVideoFrame& frame) { m_pendingFrame = frame; });
#endif

    m_throttle->setInterval(g_frameIntervalMs);
    connect(m_throttle, &QTimer::timeout, this,
            &ScreenShareController::pushFrameToPeers);
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

void ScreenShareController::start() { startForScreen(-1); }

// Apply the currently-selected quality preset (min of user pref and
// server max) to the encoder + throttle globals. Called at the top
// of every start path so a just-changed preference or server policy
// takes effect on the next share without requiring a restart.
static void applyEffectiveQuality(Settings* settings, ServerManager* servers)
{
    int userPref = settings ? settings->screenShareQuality() : 1;
    int serverMax = 3;
    if (servers) {
        auto* active = servers->activeServer();
        if (active) serverMax = active->maxScreenShareQuality();
    }
    int effective = std::min(std::clamp(userPref, 0, 3),
                             std::clamp(serverMax, 0, 3));
    auto p = ScreenShareController::presetFor(effective);
    g_frameIntervalMs = p.fps > 0 ? (1000 / p.fps) : 200;
    g_jpegQuality = p.jpegQuality;
    g_maxWidth = p.maxWidth;
    qInfo("[screenshare] quality preset=%d (user=%d, serverMax=%d) "
          "fps=%d maxW=%d Q=%d",
          effective, userPref, serverMax,
          p.fps, p.maxWidth, p.jpegQuality);
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
    m_capture->setScreen(target);
    m_capture->start();
    m_throttle->start();
#endif
}

void ScreenShareController::stop()
{
    m_throttle->stop();
    m_pendingFrame = {};
#ifdef Q_OS_MACOS
    if (m_mac) m_mac->stop();
    if (m_active) {
        m_active = false;
        emit activeChanged();
    }
#else
    if (m_active) m_capture->stop();
#endif
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
    if (!m_active) return;
    VoiceEngine* voice = nullptr;
    if (m_servers) {
        auto* active = m_servers->activeServer();
        if (active) voice = active->voiceEngine();
    }
    static int s_tickCount = 0;
    if (++s_tickCount % 25 == 1) {
        qInfo("[screenshare] push tick #%d: voice=%p frameValid=%d",
              s_tickCount, (void*)voice, int(m_pendingFrame.isValid()));
    }
    if (!voice) return;
    if (!m_pendingFrame.isValid()) return;

    QImage img = m_pendingFrame.toImage();
    if (img.isNull()) {
        if (s_tickCount % 25 == 1)
            qWarning("[screenshare] toImage() returned null, frame format=%d",
                     int(m_pendingFrame.pixelFormat()));
        return;
    }

    if (img.width() > g_maxWidth)
        img = img.scaledToWidth(g_maxWidth, Qt::SmoothTransformation);

    QByteArray jpeg;
    {
        QBuffer buf(&jpeg);
        buf.open(QIODevice::WriteOnly);
        if (!img.save(&buf, "JPEG", g_jpegQuality)) return;
    }

    voice->broadcastScreenFrame(jpeg);
    if (s_tickCount % 25 == 1)
        qInfo("[screenshare] broadcast %d-byte JPEG (tick #%d)",
              int(jpeg.size()), s_tickCount);
    m_pendingFrame = {};
}
