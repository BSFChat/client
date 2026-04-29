#include "voice/MacCameraCapturer.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <QDebug>
#include <QVariantMap>

@interface MacCameraDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) MacCameraCapturer *owner;
@end

@implementation MacCameraDelegate
- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection {
    if (!_owner) return;
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
        // Delegate call runs on the capture queue — hop to Qt main.
        QMetaObject::invokeMethod(_owner, [owner = _owner, img]() {
            owner->_onFrame(img);
        }, Qt::QueuedConnection);
    }
}
@end

MacCameraCapturer::MacCameraCapturer(QObject* parent) : QObject(parent) {}

MacCameraCapturer::~MacCameraCapturer() { stop(); }

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

    MacCameraDelegate* del = [[MacCameraDelegate alloc] init];
    del.owner = this;
    dispatch_queue_t q = dispatch_queue_create(
        "com.bsfchat.cam.frames", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:del queue:q];
    if ([session canAddOutput:output]) [session addOutput:output];
    [session commitConfiguration];
    [session startRunning];

    m_session = session;
    m_delegate = del;
    m_active = true;
    m_currentDescription = QString::fromNSString(device.localizedName);
    qInfo("[mac-camera] start '%s'", qUtf8Printable(m_currentDescription));
}

void MacCameraCapturer::stop()
{
    if (!m_active) return;
    AVCaptureSession* session = m_session;
    if (session) {
        [session stopRunning];
        [session release];
        m_session = nullptr;
    }
    if (m_delegate) {
        [(id)m_delegate release];
        m_delegate = nullptr;
    }
    m_active = false;
    qInfo("[mac-camera] stop");
}

void MacCameraCapturer::_onFrame(const QImage& img)
{
    if (!m_active) return;
    emit frameReady(img);
}
