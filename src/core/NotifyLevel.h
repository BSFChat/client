#pragma once

#include <QString>

// Which messages in a room notify, when the user has not chosen (audit 3).
//
// The server decides pushes with: everything in a DM, mentions only in a
// channel (server/src/push/PushService.cpp — "joining a busy server doesn't
// mean a phone buzzing all day"). The client defaulted every room to "all",
// so desktop and phone disagreed about the same channel, and the room menu
// showed "All" ticked for a level the server was not applying.
//
// `stored` is the explicit choice from Settings, "" when there is none.
namespace bsfchat::client {

inline bool isNotifyLevel(const QString& level)
{
    return level == QLatin1String("all") || level == QLatin1String("mentions")
        || level == QLatin1String("none");
}

inline QString defaultNotifyLevel(bool isDirect)
{
    return isDirect ? QStringLiteral("all") : QStringLiteral("mentions");
}

inline QString effectiveNotifyLevel(const QString& stored, bool isDirect)
{
    return isNotifyLevel(stored) ? stored : defaultNotifyLevel(isDirect);
}

} // namespace bsfchat::client
