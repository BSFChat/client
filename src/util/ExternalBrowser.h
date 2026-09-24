#pragma once

// Handing a URL to the user's browser, and being honest when that fails.
//
// ── Why this is not just QDesktopServices::openUrl ───────────────────────
//
// On Linux QDesktopServices::openUrl shells out: it looks for a launcher
// (xdg-open, or the desktop's own), spawns it detached and reports whether
// the SPAWN succeeded. Two things follow, and both produced the same
// user-visible symptom — a button that does nothing at all:
//
//   1. It can return false. No xdg-utils installed, no browser Qt can
//      detect, a portal-only sandbox that refuses. Every call site in this
//      app that ignored the return value turned that into silence.
//
//   2. It can return TRUE and still open nothing, and that is the one the
//      release tarball hits. BSFChat/bsfchat.sh exports LD_LIBRARY_PATH
//      (plus QT_PLUGIN_PATH and the QML import paths) so the bundled Qt is
//      found — and every process the app spawns inherits them. xdg-open is
//      a shell script that execs the real browser, so the browser starts
//      with our bundled libicu/libssl/libav* ahead of its own and dies on a
//      symbol mismatch before it can draw a window. The spawn succeeded;
//      nothing opened; nothing was said.
//
// So: when we were started by the bundle launcher (it exports
// BSFCHAT_BUNDLE_DIR) we run the launcher ourselves through `env`, putting
// LD_LIBRARY_PATH back to whatever the user's session had — nothing, in the
// normal case — and dropping the Qt path variables. Everywhere else, and as
// the fallback if that cannot be arranged, QDesktopServices does its job.
//
// The return value is the contract. openExternalUrl() returns false when the
// URL has NOT been handed to anything, and callers must surface that. For
// sign-in that means showing the user the URL so they can paste it into a
// browser by hand — the loopback listener is still up, so a sign-in finished
// that way completes normally.

#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

namespace bsfchat {

// Hand `url` to the user's browser (or, for a file:// URL, to whatever the
// desktop opens it with). False means nothing was launched.
bool openExternalUrl(const QUrl& url);

// The argv passed to `env` for the bundled-Linux case, as a pure function so
// it can be tested off Linux. `hostLdLibraryPath` is what LD_LIBRARY_PATH
// held before the launch script prepended our lib/ directory (empty if it
// held nothing — then it is unset rather than set to "", which some loaders
// read as "the current directory").
//
// Option arguments come first and the assignment after, because that is the
// order `env` parses: `env [-u NAME]... [NAME=VALUE]... COMMAND [ARG]...`.
QStringList sanitizedLaunchArgv(const QString& hostLdLibraryPath,
                                const QString& opener, const QUrl& url);

// Test seam. Set a handler to answer openExternalUrl() without touching the
// desktop; pass nullptr to restore the real one. Tests use it to prove the
// failure path is surfaced, which is otherwise only reachable on a machine
// with no browser.
using OpenUrlHandler = std::function<bool(const QUrl&)>;
void setOpenUrlHandlerForTesting(OpenUrlHandler handler);

} // namespace bsfchat
