#pragma once

#include <QString>
#include <QUrl>
#include <functional>

// ASWebAuthenticationSession, wrapped in a Qt-only interface so
// IdentityClient.cpp can stay ordinary C++ (the Objective-C++ lives in
// IosAuthSession.mm, compiled only for the iOS target — see CMakeLists.txt).
// Same shape as the other platform shims in this tree, e.g.
// src/voice/MacCameraPermission.h.
//
// WHY THIS EXISTS. The desktop sign-in opens the system browser and waits on
// a loopback QTcpServer for the redirect. On iOS that can never complete: the
// moment Safari comes forward the app is suspended, its run loop stops, the
// listening socket is never accepted from, and the user sits on "Waiting for
// browser login…" until the five-minute timeout. Sign-in was not merely
// flaky on iOS, it was impossible — which also made the OIDC path a guideline
// 2.1 rejection risk, since a reviewer who tries it sees a hung app.
//
// ASWebAuthenticationSession is Apple's answer to exactly this. It presents
// the authorization page in a SafariViewController-backed sheet that belongs
// to OUR process, so the app is never backgrounded; it watches the navigation
// for a redirect to our private-use scheme, cancels it, and hands the URL
// straight to the completion handler. No app switch, no loopback socket, no
// suspension.
namespace ios_auth_session {

// How the session ended.
enum class Outcome {
    Callback,  // `callbackUrl` holds the redirect the provider sent us
    Cancelled, // the user dismissed the sheet, or tapped Cancel
    Failed,    // could not present, or the system reported another error
};

// Invoked on the main thread, exactly once per start().
// `error` is human-readable and set only for Outcome::Failed.
using Handler = std::function<void(Outcome outcome, const QUrl& callbackUrl,
                                   const QString& error)>;

// Present `authUrl` and watch for a redirect to `<callbackScheme>://…`.
// `callbackScheme` is the bare scheme with no "://" — that is what
// ASWebAuthenticationSession wants, and passing the full URI silently never
// matches.
//
// Returns false (and does not call `handler`) if the session could not be
// created at all; every other ending arrives through `handler`.
//
// Only one session may be live at a time; starting a second cancels the
// first, whose handler then fires with Outcome::Cancelled.
bool start(const QUrl& authUrl, const QString& callbackScheme, Handler handler);

// Tear down any live session without invoking its handler. Used by
// IdentityClient::cancel(), which is also what the sign-in timeout calls, so
// the sheet cannot outlive the attempt that put it on screen.
void cancel();

} // namespace ios_auth_session
