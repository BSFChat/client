// iOS half of MobileKeyboard: read back how far the platform plugin has
// scrolled the Qt scene. See the long note in MobileKeyboard.h.
//
// READ ONLY. We never write this transform. Qt owns it, re-applies it
// whenever it recomputes, and a fight over it would be a loop rather than
// a fix — the shell's job is to make Qt's own arithmetic come out at
// zero, and this is how it checks whether it did.
#include <QtGlobal>

#import <UIKit/UIKit.h>

namespace bsfchat {
namespace platform {

int iosPlatformScrollOffset()
{
    // QIOSInputContext::scroll() walks focusView.window.rootViewController
    // and sets `view.layer.sublayerTransform` to a translation of (0, -y).
    // So -m42 is the scroll, in the root view's points, which on iOS are
    // Qt's logical pixels.
    //
    // The window is found the same way src/identity/IosAuthSession.mm
    // finds its presentation anchor: scenes only, foreground-active
    // preferred. UIApplication.windows is deprecated from iOS 15 and Qt's
    // window lives in a scene regardless.
    UIWindow* fallback = nil;
    for (int pass = 0; pass < 2; ++pass) {
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class]) continue;
            UIWindowScene* windowScene = (UIWindowScene*)scene;
            if (pass == 0
                && windowScene.activationState != UISceneActivationStateForegroundActive)
                continue;
            for (UIWindow* window in windowScene.windows) {
                if (!window.rootViewController) continue;
                if (pass == 0 && window.isKeyWindow)
                    return (int)qRound(-window.rootViewController.view.layer.sublayerTransform.m42);
                if (!fallback) fallback = window;
            }
        }
    }
    if (fallback && fallback.rootViewController)
        return (int)qRound(-fallback.rootViewController.view.layer.sublayerTransform.m42);
    return 0;
}

} // namespace platform
} // namespace bsfchat
