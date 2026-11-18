#include "voice/MacCameraCapturer.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <QCoreApplication>
#include <QDebug>
#include <QVariantMap>

@interface MacCameraDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    std::shared_ptr<MacCameraCapturer::Link> link;
}
@end

@implementation MacCameraDelegate
- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection {
    // Cheap early-out only; the authoritative check is on the GUI thread.
    if (!link || !link->owner.load(std::memory_order_relaxed)) return;
    CVImageBufferRef pixel = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixel) return;
    CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);

    const size_t w = CVPixelBufferGetWidth(pixel);
    const size_t h = CVPixelBufferGetHeight(pixel);
    const size_t bpr = CVPixelBufferGetBytesPerRow(pixel);
    void* base = CVPixelBufferGetBaseAddress(pixel);
    OSType fmt = CVPixelBufferGetPixelFormatType(pixel);

    QImage img;
    if (fmt == kCVPixelFormatType_32BGRA) {
        // Most common on macOS AVFoundation. BGRA with premultiplied
        // alpha — maps 1:1 to QImage::Format_ARGB32_Premultiplied on
        // LE systems (Qt reads the bytes in BGRA order under that
        // format on little-endian).
        img = QImage(reinterpret_cast<const uchar*>(base),
                     int(w), int(h), int(bpr),
                     QImage::Format_ARGB32_Premultiplied).copy();
    } else {
        qWarning("[mac-camera] unexpected pixel format 0x%x", fmt);
    }

    CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    if (!img.isNull()) {
        // Delegate call runs on the capture queue — hop to Qt main. The
        // context is the application object (always alive); the owner is
        // re-read there, where stop() clears it.
        std::shared_ptr<MacCameraCapturer::Link> l = link;
        QMetaObject::invokeMethod(qApp, [l, img]() {
            if (auto* o = l->owner.load()) o->_onFrame(img);
        }, Qt::QueuedConnection);
    }
}
@end

MacCameraCapturer::MacCameraCapturer(QObject* parent)
    : QObject(parent)
{
    m_sessionQueue = dispatch_queue_create("com.bsfchat.cam.session",
                                           DISPATCH_QUEUE_SERIAL);
}

MacCameraCapturer::~MacCameraCapturer()
{
    stop();
    // Pending start/stop blocks hold their own references to what they
    // touch; releasing our reference to the queue does not cancel them.
    if (m_sessionQueue) dispatch_release((dispatch_queue_t)m_sessionQueue);
}

QVariantList MacCameraCapturer::availableCameras() const
{
    QVariantList out;
    NSArray<AVCaptureDeviceType>* wanted = @[
        AVCaptureDeviceTypeBuiltInWideAngleCamera,
        AVCaptureDeviceTypeExternalUnknown
    ];
    AVCaptureDeviceDiscoverySession* discovery =
        [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:wanted
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];
    int i = 0;
    for (AVCaptureDevice* dev in discovery.devices) {
        QVariantMap m;
        m[QStringLiteral("index")] = i++;
        m[QStringLiteral("description")] = QString::fromNSString(dev.localizedName);
        m[QStringLiteral("id")] = QString::fromNSString(dev.uniqueID);
        out.append(m);
    }
    return out;
}

void MacCameraCapturer::start(int index)
{
    if (m_active) return;

    NSArray<AVCaptureDeviceType>* wanted = @[
        AVCaptureDeviceTypeBuiltInWideAngleCamera,
        AVCaptureDeviceTypeExternalUnknown
    ];
    AVCaptureDeviceDiscoverySession* discovery =
        [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:wanted
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];
    NSArray<AVCaptureDevice*>* devs = discovery.devices;
    if (devs.count == 0) {
        emit captureFailed("No camera available");
        return;
    }
    AVCaptureDevice* device = (index >= 0 && (NSUInteger)index < devs.count)
        ? devs[(NSUInteger)index]
        : [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];

    NSError* err = nil;
    AVCaptureDeviceInput* input =
        [AVCaptureDeviceInput deviceInputWithDevice:device error:&err];
    if (!input) {
        emit captureFailed(QString::fromNSString(err.localizedDescription));
        return;
    }

    AVCaptureSession* session = [[AVCaptureSession alloc] init];
    [session beginConfiguration];
    session.sessionPreset = AVCaptureSessionPreset640x480;
    if ([session canAddInput:input]) [session addInput:input];

    AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = @{
        (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_32BGRA)
    };

    m_link = std::make_shared<Link>();
    m_link->owner.store(this);
    MacCameraDelegate* del = [[MacCameraDelegate alloc] init];
    del->link = m_link;
    dispatch_queue_t q = dispatch_queue_create(
        "com.bsfchat.cam.frames", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:del queue:q];
    dispatch_release(q);   // the output retains it
    if ([session canAddOutput:output]) [session addOutput:output];
    [output release];      // the session retains it
    [session commitConfiguration];

    // Off the main thread: startRunning blocks while the device spins up.
    // The block retains `session` until it has run.
    dispatch_async((dispatch_queue_t)m_sessionQueue, ^{
        [session startRunning];
    });

    // The camera going away by itself. Delivered on the main queue, and
    // through the shared link so a stopped capturer ignores them.
    std::shared_ptr<Link> link = m_link;
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    NSOperationQueue* main = [NSOperationQueue mainQueue];
    id obs0 = [nc addObserverForName:AVCaptureDeviceWasDisconnectedNotification
                              object:device queue:main
                          usingBlock:^(NSNotification* note) {
        (void)note;
        if (auto* o = link->owner.load()) o->_onLost(QStringLiteral("Your camera was disconnected."));
    }];
    id obs1 = [nc addObserverForName:AVCaptureSessionRuntimeErrorNotification
                              object:session queue:main
                          usingBlock:^(NSNotification* n) {
        NSError* e = n.userInfo[AVCaptureSessionErrorKey];
        const QString why = e ? QString::fromNSString(e.localizedDescription)
                              : QStringLiteral("unknown error");
        if (auto* o = link->owner.load())
            o->_onLost(QStringLiteral("The camera stopped: %1").arg(why));
    }];
    id obs2 = [nc addObserverForName:AVCaptureSessionWasInterruptedNotification
                              object:session queue:main
                          usingBlock:^(NSNotification* note) {
        (void)note;
        if (auto* o = link->owner.load())
            o->_onLost(QStringLiteral("The camera was taken by another "
                                      "app or the system."));
    }];
    m_observers[0] = [obs0 retain];
    m_observers[1] = [obs1 retain];
    m_observers[2] = [obs2 retain];

    m_session = session;
    m_delegate = del;
    m_active = true;
    m_currentDescription = QString::fromNSString(device.localizedName);
    qInfo("[mac-camera] start '%s'", qUtf8Printable(m_currentDescription));
}

void MacCameraCapturer::removeObservers()
{
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    for (void*& o : m_observers) {
        if (!o) continue;
        [nc removeObserver:(id)o];
        [(id)o release];
        o = nullptr;
    }
}

void MacCameraCapturer::stop()
{
    if (!m_active) return;
    // Nothing more reaches us from here on: frames already queued to the
    // main thread and notifications both re-check this.
    if (m_link) m_link->owner.store(nullptr);
    m_link.reset();
    removeObservers();
    AVCaptureSession* session = m_session;
    MacCameraDelegate* del = m_delegate;
    m_session = nullptr;
    m_delegate = nullptr;
    m_active = false;
    // stopRunning blocks until the device has stopped; do it on the
    // session queue (after any pending startRunning), then let go.
    dispatch_async((dispatch_queue_t)m_sessionQueue, ^{
        if (session) {
            [session stopRunning];
            [session release];
        }
        if (del) [(id)del release];
    });
    qInfo("[mac-camera] stop");
}

void MacCameraCapturer::_onLost(const QString& reason)
{
    if (!m_active) return;
    qWarning("[mac-camera] %s", qUtf8Printable(reason));
    stop();
    emit cameraLost(reason);
}

void MacCameraCapturer::_onFrame(const QImage& img)
{
    if (!m_active) return;
    emit frameReady(img);
}
