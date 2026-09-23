// iOS half of the Haptics bridge (src/core/Haptics.h).
//
// UIKit's feedback generators are Objective-C, so the implementation has
// to live in a .mm; this header is the C++ face of it so Haptics.cpp
// stays a plain translation unit. Compiled only on iOS — CMakeLists.txt
// adds HapticsIos.mm behind $<$<BOOL:${IOS}>:…>, the same way
// IosAuthSession.mm is added.
#pragma once

namespace bsfchat {

// UIImpactFeedbackGenerator, medium style. The closest match to
// Android's HapticFeedbackConstants.LONG_PRESS: one definite tap that
// says "the long press registered, here is your menu".
void iosHapticImpact();

// UISelectionFeedbackGenerator. The very light click iOS uses for a
// picker passing a detent — the counterpart to KEYBOARD_TAP, and the
// right feel for a swipe crossing a threshold.
void iosHapticSelection();

} // namespace bsfchat
