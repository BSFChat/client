#include "HapticsIos.h"

#import <UIKit/UIKit.h>

namespace bsfchat {
namespace {

// One generator each, created on first use and kept for the life of the
// process. Two reasons, and neither is micro-optimisation:
//
//  * Apple's own guidance is to hold a generator, `prepare` it, and fire
//    it — a freshly-allocated one has to spin the Taptic Engine up, which
//    is exactly the latency you notice on a long-press menu.
//  * It is correct whether or not this file is compiled with ARC. A
//    per-call `[[… alloc] init…]` leaks under manual retain/release, and
//    the `-fobjc-arc` flag is Qt's to set, not ours — see the `g = nil`
//    release idiom in src/identity/IosAuthSession.mm, which only works
//    under ARC. A single never-released object is the one shape that is
//    right under both.
//
// Only ever touched from the main queue (see onMain), so the lazy
// initialisation needs no lock.
UIImpactFeedbackGenerator* g_impact = nil;
UISelectionFeedbackGenerator* g_selection = nil;

// UIKit is main-thread-only. QML signal handlers run on the GUI thread
// today, but a haptic fired from anywhere else is a crash rather than a
// missed buzz, so the hop is unconditional.
void onMain(void (^block)(void))
{
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

} // namespace

void iosHapticImpact()
{
    onMain(^{
        if (!g_impact) {
            g_impact = [[UIImpactFeedbackGenerator alloc]
                initWithStyle:UIImpactFeedbackStyleMedium];
        }
        [g_impact prepare];
        [g_impact impactOccurred];
    });
}

void iosHapticSelection()
{
    onMain(^{
        if (!g_selection) {
            g_selection = [[UISelectionFeedbackGenerator alloc] init];
        }
        [g_selection prepare];
        [g_selection selectionChanged];
    });
}

} // namespace bsfchat
