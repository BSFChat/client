#include "voice/IpPrivacy.h"

namespace voice {

QString relayRefusalMessage(RelaySource source)
{
    switch (source) {
    case RelaySource::UserSetting:
        // Names the setting, and names where it is. A refusal that says only
        // "relayed connections are required" reads as a server fault, and the
        // user goes looking for an administrator instead of for the switch they
        // turned on themselves.
        return QStringLiteral(
            "Can't join: you have \"Hide my IP address\" turned on in "
            "Settings → Voice, but this server has no relay (TURN) server for "
            "your calls to go through. Turn the setting off to connect "
            "directly — your peers will then see your IP address — or ask the "
            "server administrator to configure a relay.");
    case RelaySource::ShareOption:
        return QStringLiteral(
            "Can't start sharing with \"Hide my IP address while sharing\" "
            "turned on: this server has no relay (TURN) server for your call "
            "to go through. Start the share without that option to continue — "
            "the people in the call will then see your IP address.");
    case RelaySource::Server:
    case RelaySource::None:
        break;
    }
    // The server-wide case, unchanged from the message this refusal has always
    // carried.
    return refusalMessage(StartRefusal::RelayOnlyNoTurn);
}

QString ipPrivacyBadge(IpExposure exposure)
{
    switch (exposure) {
    case IpExposure::Hidden:
        return QStringLiteral("IP hidden");
    case IpExposure::Pending:
        // Deliberately not "IP hidden" yet. The policy is set, the path is not
        // confirmed, and the gap between the two is seconds of gathering and
        // connectivity checks during which the claim would be unproven.
        return QStringLiteral("Hiding IP…");
    case IpExposure::Shared:
        break;
    }
    return QString();
}

QString ipPrivacyDetail(IpExposure exposure)
{
    switch (exposure) {
    case IpExposure::Hidden:
        return QStringLiteral(
            "Your calls are going through this server's relay, so the people "
            "you are talking to see the relay's address instead of yours. "
            "Their addresses are still their own choice to make. Relaying adds "
            "some delay and uses the server's bandwidth, and can lower video "
            "quality if the server is the slowest link.");
    case IpExposure::Pending:
        return QStringLiteral(
            "Trying to route your calls through this server's relay. Until "
            "every connection is on the relay this is not settled, so nothing "
            "is being claimed yet — your address has not been sent to anyone "
            "either way, because with this setting on only relay addresses are "
            "gathered.");
    case IpExposure::Shared:
        return QStringLiteral(
            "Your calls connect directly to the other people in them, which is "
            "the fastest route. A direct connection means each side learns the "
            "other's IP address, which is how peer-to-peer works. Turn on "
            "\"Hide my IP address\" in Settings → Voice to route through the "
            "server instead.");
    }
    return QString();
}

} // namespace voice
