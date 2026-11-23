// iOS half of MobileKeyboard: read back how far the platform plugin has
// scrolled the Qt scene. See the long note in MobileKeyboard.h.
//
// READ ONLY. We never write this transform. Qt owns it, re-applies it
// whenever it recomputes, and a fight over it would be a loop rather than
// a fix — the shell's job is to make Qt's own arithmetic come out at
// zero, and this is how it checks whether it did.
#include <QtGlobal>
#include <QWindow>

#import <UIKit/UIKit.h>

namespace bsfchat {
namespace platform {

static int scrollOf(UIView* view)
{
    return (int)qRound(-view.layer.sublayerTransform.m42);
}

int iosPlatformScrollOffset(const QWindow* window)
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
    // The plugin scrolls the root view of the UIWindow that holds the focused
    // Qt view (QIOSInputContext::scrollToCursor walks focusView.window). For
    // this window, that is the window its own QUIView lives in. Reading it
    // directly means the answer is about the window the plugin moved, not the
    // key window, which can be a different UIWindow (an auth sheet, a
    // presented alert, the status bar window) and would report 0 while the
    // scene is in fact scrolled.
    if (window && window->winId()) {
        UIView* qtView = (__bridge UIView*)reinterpret_cast<void*>(window->winId());
        if (qtView.window && qtView.window.rootViewController)
            return scrollOf(qtView.window.rootViewController.view);
    }

    // No Qt view to ask (not yet created, or the window is gone): fall back to
    // the scene walk the auth session uses.
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
                    return scrollOf(window.rootViewController.view);
                if (!fallback) fallback = window;
            }
        }
    }
    if (fallback && fallback.rootViewController)
        return scrollOf(fallback.rootViewController.view);
    return 0;
}

} // namespace platform
} // namespace bsfchat
