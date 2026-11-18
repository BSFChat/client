#pragma once

#include <QObject>
#include <QImage>
#include <QVariantList>

#include <atomic>
#include <memory>

#ifdef __OBJC__
@class AVCaptureSession;
@class MacCameraDelegate;
#else
typedef struct objc_object AVCaptureSession;
typedef struct objc_object MacCameraDelegate;
#endif

// macOS AVFoundation-direct camera capturer. Bypasses Qt's
// (Homebrew-Qt-missing) QCameraPermission plugin entirely — Qt's
// QCamera refuses to start when the plugin is absent even if the
// user has granted camera access at the TCC level.
//
// AVCaptureSession fires frames through a delegate on a background
// queue; the delegate re-emits them on the Qt thread as QImages.
//
// [AVCaptureSession startRunning]/stopRunning block for hundreds of
// milliseconds (more with a USB camera waking up), and they used to run on
// the main thread — part of why leaving a call with the camera on froze
// the window. They now run on a private serial dispatch queue; start()
// and stop() return at once, and the queue keeps them in order.
//
// The camera can also go away underneath us: unplugged
// (AVCaptureDeviceWasDisconnectedNotification), a runtime error, or an
// interruption (another app took it, lid closed with an external
// display, …). Each stops capture and emits cameraLost(reason) so the
// owner can turn camera_on off and tell the user, instead of leaving a
// frozen tile that still claims to be live.
class MacCameraCapturer : public QObject {
    Q_OBJECT
public:
    explicit MacCameraCapturer(QObject* parent = nullptr);
    ~MacCameraCapturer();

    // Start capture from device at `index` in availableCameras(), or
    // default device if negative. Idempotent if already running.
    void start(int index = -1);
    void stop();
    bool isActive() const { return m_active; }

    // Descriptions of attached cameras; populated from
    // AVCaptureDevice.devicesWithMediaType:.
    QVariantList availableCameras() const;
    QString currentDescription() const { return m_currentDescription; }

    // Shared with the ObjC side (delegate, notification blocks). Cleared
    // on the GUI thread by stop()/destruction; read there again before
    // anything is delivered, so a late frame or notification never reaches
    // a stopped or destroyed capturer.
    struct Link { std::atomic<MacCameraCapturer*> owner{nullptr}; };

signals:
    void frameReady(const QImage& frame);
    void captureFailed(const QString& description);
    // Capture stopped on its own (unplugged, runtime error, interruption).
    // Already stopped when this is emitted.
    void cameraLost(const QString& reason);

private:
    void removeObservers();

    AVCaptureSession* m_session = nullptr;
    MacCameraDelegate* m_delegate = nullptr;
    // dispatch_queue_t, opaque here so this header stays plain C++.
    void* m_sessionQueue = nullptr;
    // NSNotificationCenter observer tokens (id), retained.
    void* m_observers[3] = {nullptr, nullptr, nullptr};
    std::shared_ptr<Link> m_link;
    bool m_active = false;
    QString m_currentDescription;

public:
    // Called from the ObjC delegate on the Qt thread once a frame
    // has been converted into a QImage. Private by convention —
    // exposed because Obj-C can't friend C++.
    void _onFrame(const QImage& img);
    void _onLost(const QString& reason);
};
