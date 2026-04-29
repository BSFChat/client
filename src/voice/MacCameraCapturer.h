#pragma once

#include <QObject>
#include <QImage>
#include <QVariantList>

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

signals:
    void frameReady(const QImage& frame);
    void captureFailed(const QString& description);

private:
    AVCaptureSession* m_session = nullptr;
    MacCameraDelegate* m_delegate = nullptr;
    bool m_active = false;
    QString m_currentDescription;

public:
    // Called from the ObjC delegate on the Qt thread once a frame
    // has been converted into a QImage. Private by convention —
    // exposed because Obj-C can't friend C++.
    void _onFrame(const QImage& img);
};
