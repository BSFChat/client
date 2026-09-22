#include "voice/video/LatencyCriticalActivity.h"

#import <Foundation/Foundation.h>

// See LatencyCriticalActivity.h. Written to be correct with or without
// ARC: CFBridgingRetain/Release are plain CFRetain/CFRelease under MRC.

void LatencyCriticalActivity::begin() {
    if (m_token) return;
    @autoreleasepool {
        id<NSObject> activity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:(NSActivityUserInitiatedAllowingIdleSystemSleep
                                      | NSActivityLatencyCritical)
                              reason:@"Presenting a video stream"];
        m_token = const_cast<void*>(CFBridgingRetain(activity));
    }
}

void LatencyCriticalActivity::end() {
    if (!m_token) return;
    @autoreleasepool {
        id<NSObject> activity = CFBridgingRelease(m_token);
        m_token = nullptr;
        [[NSProcessInfo processInfo] endActivity:activity];
    }
}
