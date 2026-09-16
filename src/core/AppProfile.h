#pragma once

#include <QByteArray>
#include <QString>

// Multi-instance support.
//
// Everything the client persists is keyed, directly or indirectly, off
// QCoreApplication::applicationName()/organizationName():
//
//   * QSettings          — Settings.cpp constructs QSettings("BSFChat", ...)
//   * LocalCache         — QStandardPaths::AppDataLocation + "/cache"
//   * the file log       — ~/Library/Logs/<applicationName> (macOS)
//   * single-instance /  — UrlHandler's QLocalServer socket name
//     bsfchat:// handoff
//
// so two clients started on one machine fight over all four. A profile
// name (from `--profile <name>` or $BSFCHAT_PROFILE) suffixes the
// application name and the IPC socket, which namespaces all four at once.
//
// The empty profile — no flag, no env var — must resolve to exactly the
// historical values ("BSFChat"/"BSFChat", unsuffixed socket) so existing
// installs keep their settings, cache and logs. test_profile.cpp pins that.
namespace bsfchat {

// Reduce a user-supplied profile name to something safe to embed in a
// path, a QSettings application name and a QLocalServer socket name:
// [A-Za-z0-9._-] survives, every other character becomes '_', leading and
// trailing separators are trimmed and the result is capped at 32 chars.
// Returns "" for a name that contains nothing usable.
QString sanitizeProfileName(const QString& raw);

// Extract a profile from the command line: `--profile <name>` or
// `--profile=<name>`. Falls back to `envValue` ($BSFCHAT_PROFILE) when the
// flag is absent. The result is sanitized. Returns "" for no profile.
// Callable before QCoreApplication exists — the single-instance handoff in
// main() runs that early.
QString profileFromArguments(int argc, char** argv, const QByteArray& envValue);

// Names derived from a profile. Both are pure functions of `profile`, so
// tests can exercise them without touching global state.
QString organizationNameForProfile(const QString& profile);
QString applicationNameForProfile(const QString& profile);
// Suffix appended to the bsfchat:// IPC / single-instance socket name.
// "" for the default profile, "-<profile>" otherwise.
QString socketSuffixForProfile(const QString& profile);

// Process-wide active profile, set once at the top of main() before
// anything reads a path. Defaults to "" (the historical behaviour).
void setActiveProfile(const QString& profile);
QString activeProfile();

// Convenience wrappers around the active profile, for call sites that
// cannot go through QCoreApplication (Settings' QSettings ctor runs before
// nothing in particular, but being explicit keeps the mapping in one file).
QString organizationName();
QString applicationName();

} // namespace bsfchat
