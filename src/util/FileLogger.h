#pragma once

#include <QString>

namespace bsfchat {

// Installs a Qt message handler that mirrors every log line (qDebug /
// qInfo / qWarning / qCritical, categories included) to a rotating file
// in addition to the default stderr sink. Without this, a client
// launched from Finder/Explorer logs nowhere — voice signaling issues
// become undiagnosable after the fact.
//
// Location: ~/Library/Logs/BSFChat/bsfchat.log on macOS, otherwise
// <AppDataLocation>/logs/bsfchat.log. Rotates at 5 MB, keeps 3 old
// generations (bsfchat.log.1..3).
//
// Call once, after QCoreApplication::setApplicationName().
void installFileLogger();

// Directory the logger writes into (for surfacing in diagnostics UI).
QString logDirectory();

} // namespace bsfchat
