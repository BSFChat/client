#include "identity/IosAuthSession.h"

#import <AuthenticationServices/AuthenticationServices.h>
#import <UIKit/UIKit.h>

#include <QDebug>

// ---------------------------------------------------------------------------
// Presentation anchor.
//
// ASWebAuthenticationSession refuses to present without one (it fails with
// ASWebAuthenticationSessionErrorCodePresentationContextNotProvided), and the
// anchor has to be a window that is actually on screen. Qt's iOS platform
// plugin creates a normal UIWindow in a UIWindowScene, so the scene walk below
// finds it; the flat `windows` list is the fallback for anything that does not
// look like that.
// ---------------------------------------------------------------------------
@interface BSFChatAuthAnchorProvider
    : NSObject <ASWebAuthenticationPresentationContextProviding>
@end

@implementation BSFChatAuthAnchorProvider

- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:
    (ASWebAuthenticationSession*)session
{
    (void)session;
    // Two passes, both over window scenes only: UIApplication.windows is
    // deprecated from iOS 15 and Qt's window lives in a scene anyway.
    // A backgrounded scene's window is not a legal anchor, so the
    // foreground-active one is tried first and any scene second.
    UIWindow* fallback = nil;
    for (int pass = 0; pass < 2; ++pass) {
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class]) continue;
            UIWindowScene* windowScene = (UIWindowScene*)scene;
            if (pass == 0
                && windowScene.activationState != UISceneActivationStateForegroundActive)
                continue;
            for (UIWindow* window in windowScene.windows) {
                if (window.isKeyWindow) return window;
                if (!fallback) fallback = window;
            }
        }
    }
    return fallback;
}

@end

namespace ios_auth_session {
namespace {

// The session must be held alive until its completion handler runs —
// ASWebAuthenticationSession does not retain itself, and a released one
// simply never calls back (the original symptom this whole change is about,
// reproduced in a new place). The anchor provider is likewise retained for
// the session's lifetime because `presentationContextProvider` is weak.
ASWebAuthenticationSession* g_session = nil;
BSFChatAuthAnchorProvider* g_anchor = nil;
// Which start() the live session belongs to. A completion is acted on only
// if it is still the current generation: a session that was cancelled or
// replaced can still have its completion queued (it hops to the main queue
// below), and without this guard that stale completion released the NEWER
// session and reported Cancelled for a sign-in that was still running.
// A counter rather than a pointer comparison: a released session's address
// can be reused by the next allocation.
unsigned long long g_generation = 0;

void release_session()
{
    g_session = nil;
    g_anchor = nil;
}

// presentPage()'s sheet: separate globals so it never touches a sign-in.
ASWebAuthenticationSession* g_pageSession = nil;
BSFChatAuthAnchorProvider* g_pageAnchor = nil;
unsigned long long g_pageGeneration = 0;

} // namespace

bool start(const QUrl& authUrl, const QString& callbackHost, const QString& callbackPath,
           Handler handler)
{
    // A second start() replaces the first. Drop the old one without firing
    // its handler; the caller (IdentityClient::startLogin) has already
    // cancelled the attempt it belonged to.
    cancel();

    NSURL* url = [NSURL URLWithString:authUrl.toString().toNSString()];
    if (!url) {
        qWarning() << "[IosAuthSession] refusing to present a malformed "
                      "authorization URL";
        return false;
    }

    g_anchor = [[BSFChatAuthAnchorProvider alloc] init];

    const unsigned long long generation = ++g_generation;

    // Captured by value so the block owns its copy; the handler outlives this
    // stack frame by definition. Plain value, not __block: a block captures a
    // C++ object by const copy and std::function::operator() is const, so the
    // nested dispatch_async block below gets a working copy too.
    Handler done = std::move(handler);

    // The completion, built first so the session itself can be created
    // inside the availability check below.
    void (^completion)(NSURL* _Nullable, NSError* _Nullable) =
        ^(NSURL* _Nullable callbackURL, NSError* _Nullable error) {
            // Always back to the Qt main thread before touching anything of
            // ours. AppKit/UIKit calls this on the main queue today, but the
            // signal chain this ends in (IdentityClient -> ServerConnection ->
            // QML) tolerates no ambiguity about that.
            dispatch_async(dispatch_get_main_queue(), ^{
                // Superseded or cancelled: not ours to report, and the
                // globals now belong to a newer session (or none).
                if (generation != g_generation || !g_session) return;
                release_session();

                if (error) {
                    const bool cancelled =
                        [error.domain isEqualToString:ASWebAuthenticationSessionErrorDomain]
                        && error.code == ASWebAuthenticationSessionErrorCodeCanceledLogin;
                    if (cancelled) {
                        // The user tapped Cancel or swiped the sheet away.
                        // Not an error to shout about, but it MUST be
                        // reported: leaving it silent is how the old flow
                        // stranded people on "Waiting for browser login…".
                        done(Outcome::Cancelled, QUrl(), QString());
                        return;
                    }
                    done(Outcome::Failed, QUrl(),
                         QString::fromNSString(error.localizedDescription));
                    return;
                }

                if (!callbackURL) {
                    done(Outcome::Failed, QUrl(),
                         QStringLiteral("The sign-in sheet closed without a reply"));
                    return;
                }

                // Hand the URL back untouched. Everything that decides what it
                // MEANS — the state comparison, the code — lives in
                // oidc::parseCallbackQuery, shared with the desktop loopback
                // path, and none of it belongs in a platform shim.
                done(Outcome::Callback,
                     QUrl(QString::fromNSString(callbackURL.absoluteString)),
                     QString());
            });
        };

    if (@available(iOS 17.4, *)) {
        ASWebAuthenticationSessionCallback* callback =
            [ASWebAuthenticationSessionCallback callbackWithHTTPSHost:callbackHost.toNSString()
                                                                 path:callbackPath.toNSString()];
        g_session = [[ASWebAuthenticationSession alloc] initWithURL:url
                                                           callback:callback
                                                  completionHandler:completion];
    } else {
        // No custom-scheme fallback: see the header. Reported through the
        // handler on the next turn of the loop so the caller's state is
        // settled first, like every other ending.
        release_session();
        dispatch_async(dispatch_get_main_queue(), ^{
            if (generation != g_generation) return;
            done(Outcome::Failed, QUrl(),
                 QStringLiteral("Signing in needs iOS 17.4 or later. Update iOS and try again."));
        });
        return true;
    }

    g_session.presentationContextProvider = g_anchor;
    // NO, explicitly: the point of ASWebAuthenticationSession over an
    // in-app web view is that it shares Safari's cookie jar, so a user
    // already signed in to the identity provider is one tap from done.
    // iOS shows its own "…Wants to Use…to Sign In" consent alert for this,
    // which is the sanctioned trade and the reason Apple allows the SSO at
    // all. Setting YES would make every sign-in a fresh password entry.
    g_session.prefersEphemeralWebBrowserSession = NO;

    if (![g_session start]) {
        qWarning() << "[IosAuthSession] ASWebAuthenticationSession refused to start";
        release_session();
        return false;
    }
    return true;
}

void cancel()
{
    // Any completion still queued for the session being dropped is stale.
    ++g_generation;
    if (!g_session) return;
    // -cancel does not invoke the completion handler, which is what we want:
    // the attempt is already being torn down by IdentityClient.
    [g_session cancel];
    release_session();
}

bool presentPage(const QUrl& pageUrl)
{
    NSURL* url = [NSURL URLWithString:pageUrl.toString(QUrl::FullyEncoded).toNSString()];
    if (!url) return false;
    if (g_pageSession) {
        ++g_pageGeneration;
        [g_pageSession cancel];
        g_pageSession = nil;
    }
    if (@available(iOS 17.4, *)) {
        const unsigned long long generation = ++g_pageGeneration;
        g_pageAnchor = [[BSFChatAuthAnchorProvider alloc] init];
        // A callback path nothing ever redirects to: the sheet stays up until
        // the user taps Cancel (or deletion finishes on the provider's own
        // "account deleted" page and they close it).
        ASWebAuthenticationSessionCallback* callback =
            [ASWebAuthenticationSessionCallback
                callbackWithHTTPSHost:@"bsfchat.com"
                                 path:@"/oauth/account-page-closed"];
        g_pageSession = [[ASWebAuthenticationSession alloc]
                  initWithURL:url
                     callback:callback
            completionHandler:^(NSURL* _Nullable, NSError* _Nullable) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (generation != g_pageGeneration) return;
                    g_pageSession = nil;
                    g_pageAnchor = nil;
                });
            }];
        g_pageSession.presentationContextProvider = g_pageAnchor;
        // Shared cookies, like the sign-in: the user is already signed in to
        // the provider there, so the delete form is one step away.
        g_pageSession.prefersEphemeralWebBrowserSession = NO;
        if (![g_pageSession start]) {
            g_pageSession = nil;
            g_pageAnchor = nil;
            return false;
        }
        return true;
    }
    return false;
}

} // namespace ios_auth_session
