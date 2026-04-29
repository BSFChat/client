// Objective-C++ shim that bypasses Qt's (missing on Homebrew Qt)
// permissions plugin by calling AVFoundation directly. Triggers
// the TCC prompt on first use and reports the current status.
#import <AVFoundation/AVFoundation.h>

#include <QObject>
#include <QDebug>
#include <functional>

namespace mac_camera_permission {

// Synchronous status check. "granted" / "denied" / "restricted" /
// "undetermined".
QString status() {
    AVAuthorizationStatus s = [AVCaptureDevice
        authorizationStatusForMediaType:AVMediaTypeVideo];
    switch (s) {
    case AVAuthorizationStatusAuthorized: return "granted";
    case AVAuthorizationStatusDenied: return "denied";
    case AVAuthorizationStatusRestricted: return "restricted";
    case AVAuthorizationStatusNotDetermined: default: return "undetermined";
    }
}

// Async request. Triggers the TCC prompt on first call. `callback` is
// invoked on the main thread once the user responds (or immediately
// if a decision is on record already).
void request(std::function<void(bool granted)> callback) {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL granted) {
        dispatch_async(dispatch_get_main_queue(), ^{
            callback((bool)granted);
        });
    }];
}

} // namespace mac_camera_permission
