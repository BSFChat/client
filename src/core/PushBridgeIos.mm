// iOS end of PushBridge: APNs registration, the device token, and taps on
// remote notifications. See docs/push-notifications.md.
//
// Qt owns the UIApplicationDelegate (QIOSApplicationDelegate, private to
// the platform plugin), and the two callbacks that deliver the device token
// are delegate methods. Qt does not implement them, so they are ADDED to
// the delegate's class at runtime with class_addMethod — no subclassing,
// no link-time dependency on a Qt-private class name. If a future Qt does
// implement one, the original is kept and called first.
//
// When: at UIApplicationDidFinishLaunchingNotification, observed from a
// static constructor so it is in place before Qt even runs main(). The
// same moment installs the UNUserNotificationCenter delegate, which Apple
// requires "before the app finishes launching" for a tap that cold-starts
// the app to be delivered. Anything that arrives before C++ has a
// PushBridge (token, error, tap) is held here and flushed by attach().
//
// UIBackgroundModes remote-notification is NOT declared and not needed:
// these are alert pushes the system displays itself. Fetching the event
// content on the device would need a Notification Service Extension (a
// separate app-extension target with its own keychain access to the
// session) — not built yet; the pusher's default_payload already sets
// mutable-content so one can be added without touching the server side.
#include "core/PushBridge.h"

#include <QString>

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>
#import <objc/runtime.h>

namespace {

// Set by attach(); everything before that is held in the statics below.
PushBridge* g_bridge = nullptr;
NSString* g_pendingToken = nil;
NSString* g_pendingError = nil;
NSString* g_pendingTapRoom = nil;
NSString* g_pendingTapEvent = nil;

IMP g_origDidRegister = nullptr;
IMP g_origDidFail = nullptr;
// Kept alive for the process; UIApplication's delegate property is not a
// strong reference.
id g_appDelegate = nil;

QString toQ(NSString* s)
{
    return s ? QString::fromNSString(s) : QString();
}

NSString* stringValue(id v)
{
    if ([v isKindOfClass:NSString.class]) return (NSString*)v;
    if ([v isKindOfClass:NSNumber.class]) return [(NSNumber*)v stringValue];
    return nil;
}

void handleToken(NSData* token)
{
    const unsigned char* bytes = (const unsigned char*)token.bytes;
    NSMutableString* hex = [NSMutableString stringWithCapacity:token.length * 2];
    for (NSUInteger i = 0; i < token.length; ++i) [hex appendFormat:@"%02x", bytes[i]];
    if (g_bridge) {
        g_bridge->deliverToken(toQ(hex), QString());
    } else {
        g_pendingToken = [hex copy];
    }
}

void handleError(NSString* reason)
{
    if (g_bridge) {
        g_bridge->deliverUnavailable(toQ(reason));
    } else {
        g_pendingError = [reason copy];
    }
}

void handleTap(NSDictionary* userInfo)
{
    // Sygnal's APNs payload carries room_id / event_id at the top level, in
    // both its event_id_only (default_payload) and its full path.
    NSString* room = stringValue(userInfo[@"room_id"]);
    NSString* event = stringValue(userInfo[@"event_id"]);
    if (room.length == 0) return;
    if (g_bridge) {
        g_bridge->deliverTap(toQ(room), toQ(event));
    } else {
        g_pendingTapRoom = [room copy];
        g_pendingTapEvent = [event copy];
    }
}

void bsfDidRegister(id self, SEL cmd, UIApplication* app, NSData* token)
{
    if (g_origDidRegister)
        ((void (*)(id, SEL, UIApplication*, NSData*))g_origDidRegister)(self, cmd, app, token);
    handleToken(token);
}

void bsfDidFail(id self, SEL cmd, UIApplication* app, NSError* error)
{
    if (g_origDidFail)
        ((void (*)(id, SEL, UIApplication*, NSError*))g_origDidFail)(self, cmd, app, error);
    // Simulator without APNs, a missing aps-environment entitlement, no
    // network at first launch. Logged by PushBridge.
    handleError(error.localizedDescription ?: @"registration-failed");
}

void addOrChain(Class cls, SEL sel, IMP imp, IMP* original)
{
    Method existing = class_getInstanceMethod(cls, sel);
    if (!existing) {
        class_addMethod(cls, sel, imp, "v@:@@");
        return;
    }
    *original = method_setImplementation(existing, imp);
}

} // namespace

@interface BSFChatPushCenterDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation BSFChatPushCenterDelegate

// In the foreground the live /sync already has the message; showing the
// push banner too would double it.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    completionHandler(UNNotificationPresentationOptionNone);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier])
        handleTap(response.notification.request.content.userInfo);
    completionHandler();
}

@end

namespace {

BSFChatPushCenterDelegate* g_centerDelegate = nil;

void install(UIApplication* app)
{
    static bool done = false;
    if (done) return;
    done = true;

    g_centerDelegate = [[BSFChatPushCenterDelegate alloc] init];
    [UNUserNotificationCenter currentNotificationCenter].delegate = g_centerDelegate;

    id delegate = app.delegate;
    if (!delegate) return;
    Class cls = object_getClass(delegate);
    addOrChain(cls, @selector(application:didRegisterForRemoteNotificationsWithDeviceToken:),
               (IMP)bsfDidRegister, &g_origDidRegister);
    addOrChain(cls, @selector(application:didFailToRegisterForRemoteNotificationsWithError:),
               (IMP)bsfDidFail, &g_origDidFail);
    // UIApplication caches which optional delegate methods exist when the
    // delegate is set; set it again so it sees the two just added (the same
    // step GoogleUtilities' app-delegate swizzler takes).
    g_appDelegate = delegate;
    app.delegate = nil;
    app.delegate = g_appDelegate;
}

__attribute__((constructor)) void observeLaunch()
{
    @autoreleasepool {
        [[NSNotificationCenter defaultCenter]
            addObserverForName:UIApplicationDidFinishLaunchingNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* note) {
                        install((UIApplication*)note.object);
                    }];
    }
}

} // namespace

namespace bsfchat::push::ios {

void attach(PushBridge* bridge)
{
    // Belt and braces: if the launch notification was somehow missed.
    if ([NSThread isMainThread] && UIApplication.sharedApplication)
        install(UIApplication.sharedApplication);
    g_bridge = bridge;
    if (g_pendingToken) {
        g_bridge->deliverToken(toQ(g_pendingToken), QString());
        g_pendingToken = nil;
    }
    if (g_pendingError) {
        g_bridge->deliverUnavailable(toQ(g_pendingError));
        g_pendingError = nil;
    }
    if (g_pendingTapRoom) {
        g_bridge->deliverTap(toQ(g_pendingTapRoom), toQ(g_pendingTapEvent));
        g_pendingTapRoom = nil;
        g_pendingTapEvent = nil;
    }
}

void queryPermission();

void requestAuthorizationAndRegister()
{
    UNAuthorizationOptions options =
        UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge;
    [[UNUserNotificationCenter currentNotificationCenter]
        requestAuthorizationWithOptions:options
                      completionHandler:^(BOOL granted, NSError* error) {
                          dispatch_async(dispatch_get_main_queue(), ^{
                              if (!granted) {
                                  handleError(error ? error.localizedDescription
                                                    : @"notifications-denied");
                                  queryPermission();
                                  return;
                              }
                              [[UIApplication sharedApplication] registerForRemoteNotifications];
                          });
                      }];
}

QString deviceModel()
{
    return toQ(UIDevice.currentDevice.model);
}

void queryPermission()
{
    // Denied = the user switched notifications off for BSFChat (or refused
    // the prompt). Push still registers, but nothing would be shown, so
    // Settings says so instead of "Push is on".
    [[UNUserNotificationCenter currentNotificationCenter]
        getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
            const bool blocked = settings.authorizationStatus == UNAuthorizationStatusDenied;
            if (g_bridge) g_bridge->deliverPermission(blocked);
        }];
}

void openNotificationSettings()
{
    dispatch_async(dispatch_get_main_queue(), ^{
        NSString* target = UIApplicationOpenSettingsURLString;
        if (@available(iOS 16.0, *)) target = UIApplicationOpenNotificationSettingsURLString;
        NSURL* url = [NSURL URLWithString:target];
        if (url) [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
    });
}

} // namespace bsfchat::push::ios
