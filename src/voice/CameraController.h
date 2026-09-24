#pragma once

#include <QObject>
#include <QPointer>
#include <QVideoSink>
#include <QVideoFrame>
#include <QTimer>
#include <QVariantList>

#include "voice/CameraPermissionPolicy.h"
#include "voice/video/VideoSendStats.h"

#include <functional>

#ifdef Q_OS_MACOS
class MacCameraCapturer;
#else
class QCamera;
class QMediaCaptureSession;
#endif

class IVoiceTransport;
class ServerManager;
class Settings;
class VideoSendPipeline;
class VideoRateController;

// Webcam broadcaster — companion to ScreenShareController. Captures
// from QCamera, feeds a local preview sink, and (every ~200 ms)
// pushes the latest frame as a JPEG over the voice data channel
// using the 0x03 type tag (screen share uses 0x02, audio uses 0x01).
//
// Unlike QScreenCapture, QCamera works on Homebrew's Qt Multimedia
// build — no native Objective-C++ wrapper needed.
//
// ON MOBILE, NOTHING IN THIS CLASS MAY TOUCH THE CAMERA BEFORE start().
//
// The instance is constructed in main() at launch on every platform, so
// anything the constructor does happens on the splash screen. On Android
// and iOS that rules out constructing a QCamera there — bringing Qt's
// multimedia stack up is enough to provoke the platform's CAMERA prompt,
// which is exactly how users got a camera prompt on the SIGN-IN screen
// (observed on a device, fixed 2026-09-22; Play treats an unprompted
// sensitive-permission request as a policy problem and App Review reads
// it the same way). It equally rules out querying or requesting a
// permission from the constructor.
//
// So on mobile the constructor wires signals and nothing else, and
// ensureCaptureSession() builds the capture objects at the first
// startForCamera() — which is only reachable from the dock's camera
// button. Desktop still primes them in the constructor: there is no
// prompt to provoke there, and nothing should change for it. macOS uses
// MacCameraCapturer instead, whose constructor is empty.
//
// The permission itself is asked in startForCamera() on both Apple
// platforms and nowhere else; see CameraPermissionPolicy.h.
class CameraController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // True only while frames are actually landing on ≥1 open peer
    // data channel — same semantics as ScreenShareController.
    Q_PROPERTY(bool transmitting READ transmitting NOTIFY transmittingChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVideoSink* previewSink READ previewSink CONSTANT)
    Q_PROPERTY(QVariantList availableCameras READ availableCameras NOTIFY camerasChanged)
    Q_PROPERTY(QString cameraDescription READ cameraDescription NOTIFY cameraDescriptionChanged)

public:
    explicit CameraController(QObject* parent = nullptr);

    bool active() const { return m_active; }
    bool transmitting() const { return m_transmitting; }
    QString lastError() const { return m_lastError; }
    QVideoSink* previewSink() const { return m_sink; }
    QVariantList availableCameras() const;
    QString cameraDescription() const { return m_cameraDescription; }

    void setServerManager(ServerManager* mgr);
    void setSettings(Settings* settings) { m_settings = settings; }

    Q_INVOKABLE void start();
    Q_INVOKABLE void startForCamera(int index);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggle();

    // Mirror frames from our internal sink into a caller-supplied
    // VideoOutput sink (which is read-only from QML).
    Q_INVOKABLE void forwardTo(QVideoSink* sink);

    // ---- "Hide my IP while sharing" ----------------------------------
    //
    // A per-share override on the user's standing "Hide my IP address"
    // setting, set by the picker (or the camera menu) BEFORE the share
    // starts. Defaults to the standing setting, so leaving it alone does
    // what the user already asked for globally.
    //
    // Because one peer connection carries voice AND both video streams,
    // honouring this means the whole connection is relay-only for as long as
    // the share lasts — the engine re-establishes its peers when it goes on,
    // and again when it goes off. There is no way to relay the video and leave
    // the voice direct.
    Q_INVOKABLE void setHideIpForShare(bool hideIp) { m_hideIpForShare = hideIp; }
    Q_INVOKABLE bool hideIpForShare() const { return m_hideIpForShare; }
    // Whether this server can honour it at all (it needs a relay). QML asks so
    // the option can be shown disabled with a reason, rather than offered and
    // then refused.
    Q_INVOKABLE bool canHideIpWhileSharing() const;

signals:
    void activeChanged();
    void transmittingChanged();
    void lastErrorChanged();
    void camerasChanged();
    void cameraDescriptionChanged();

private:
    bool m_hideIpForShare = false;
    void pushFrameToPeers();
    void setTransmitting(bool transmitting);
    // Mirrors ScreenShareController: the one place the active flag
    // flips, forcing an IDR on the way up (S-11) and announcing the
    // camera stream's on/off state to peers (S-7).
    void setActiveState(bool active);
    void announceStream(bool on);
    // Push the per-share override into the live transport as the share starts
    // (on=true) and stops (on=false). Off always clears it, whatever
    // m_hideIpForShare says, so ending a share can never leave the connection
    // pinned to relay by a flag nobody can see.
    void applyShareIpPrivacy(bool on);
    IVoiceTransport* currentVoice() const;
    // Rewires the per-server "voice room changed" subscriptions
    // whenever the server list or active server changes (and at
    // initial setup), so leaving a voice channel reliably stops the
    // camera instead of letting it silently re-broadcast on the
    // next join.
    void rewireVoiceLeaveWatch();

    // ---- Lazy capture + permission (never at launch on mobile) -------
    //
    // Build QCamera + QMediaCaptureSession if they do not exist yet.
    // Called from the constructor on DESKTOP, where there is no prompt to
    // provoke and nothing should change, and from the first
    // startForCamera() on Android / iOS, where constructing them early
    // makes the platform ask for the CAMERA permission on the sign-in
    // screen. No-op on macOS, which uses MacCameraCapturer instead.
    void ensureCaptureSession();
    // This platform's current camera permission, without prompting.
    camperm::Status cameraPermission() const;
    // Prompt. `done` runs on the main thread with the user's answer.
    // Only called when cameraPermission() said Undetermined.
    void requestCameraPermission(std::function<void(bool granted)> done);
    // Set m_lastError and notify. One place so the refusal paths cannot
    // forget the signal.
    void failWith(const QString& message);
    // One line per camera start, naming what the capture backend
    // actually handed us: the frame's presentation rotation and mirror
    // flag, its size, and the screen orientation at the time.
    void logFirstFrameGeometry(const QVideoFrame& frame);

#ifdef Q_OS_MACOS
    MacCameraCapturer* m_mac = nullptr;
#else
    QCamera* m_camera = nullptr;
    QMediaCaptureSession* m_session = nullptr;
#endif
    QVideoSink* m_sink = nullptr;
    QTimer* m_throttle = nullptr;
    ServerManager* m_servers = nullptr;
    Settings* m_settings = nullptr;
    // H.264-over-RTP encode worker + adaptive governor (vcamera
    // track), mirroring ScreenShareController's screen pair. The JPEG
    // branch survives for legacy peers / the renegotiation gap.
    VideoSendPipeline* m_pipeline = nullptr;
    VideoRateController* m_rate = nullptr;
    QPointer<QObject> m_wiredEngine;
    // Capture ticks at RTP rate; the legacy JPEG branch subsamples
    // via this counter to keep old peers at their accustomed ~5 fps.
    int m_tick = 0;
    // Send-side windows for the rate controller (VideoSendStats.h):
    // encode counters and packets per frame. Capture-side fields stay
    // zero — cameras deliver on their own clock, so a missing frame is
    // not evidence the capture was late.
    videosend::Accumulator m_sendStats;
    // Per-connection voice-room subscriptions (one per server, not
    // just the active one — voice can be live on a backgrounded
    // server).
    QList<QMetaObject::Connection> m_voiceRoomConns;
    QVideoFrame m_pendingFrame;
    // Log the first frame's geometry once per start. See
    // logFirstFrameGeometry() — this is the seam that tells us, from a
    // device log, whether Qt stamps a capture rotation at all.
    bool m_loggedFrameGeometry = false;
    bool m_active = false;
    bool m_transmitting = false;
    QString m_lastError;
    QString m_cameraDescription;
};
