// Compile-time facts about mobile push for this build. See
// docs/push-notifications.md.
//
// BSFCHAT_PUSH_ENABLED is defined by CMakeLists.txt when the build carries
// a push transport: Android with the four BSFCHAT_FCM_* values supplied
// (Firebase Cloud Messaging), or iOS with BSFCHAT_IOS_PUSH on (APNs).
// BSFCHAT_PUSH_APP_ID is the Matrix pusher app_id the gateway knows this
// build by.
#pragma once

namespace bsfchat::push {

#ifdef BSFCHAT_PUSH_ENABLED
inline constexpr bool kCompiledIn = true;
#else
inline constexpr bool kCompiledIn = false;
#endif

#ifdef BSFCHAT_PUSH_APP_ID
inline constexpr const char* kAppId = BSFCHAT_PUSH_APP_ID;
#else
inline constexpr const char* kAppId = "";
#endif

} // namespace bsfchat::push
