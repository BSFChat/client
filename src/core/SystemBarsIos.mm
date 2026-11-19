// iOS half of SystemBars (src/core/SystemBars.h).
//
// Qt's view controller leaves preferredStatusBarStyle at the default, and
// the default picks light or dark content from the window's interface
// style. Overriding that style on our windows to match the app theme is
// therefore all it takes for the status bar to stay readable, and it keeps
// the keyboard and other system chrome in the same appearance.

#include "core/SystemBars.h"

#import <UIKit/UIKit.h>

namespace bsfchat {

void iosSetInterfaceStyle(bool dark)
{
    const UIUserInterfaceStyle style =
        dark ? UIUserInterfaceStyleDark : UIUserInterfaceStyleLight;
    // UIKit is main-thread-only.
    dispatch_async(dispatch_get_main_queue(), ^{
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            for (UIWindow* window in ((UIWindowScene*)scene).windows) {
                if (window.overrideUserInterfaceStyle == style) continue;
                window.overrideUserInterfaceStyle = style;
                [window.rootViewController setNeedsStatusBarAppearanceUpdate];
            }
        }
    });
}

} // namespace bsfchat
