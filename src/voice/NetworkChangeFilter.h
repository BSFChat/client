#pragma once

// Which QNetworkInformation notifications are worth rebuilding a call for.
//
// Every reachability/transport notification used to rebuild every peer
// (audit 3): including the first emission after the backend loads, an
// Unknown → Online "change" that is only the backend settling, and a drop
// to Disconnected, where a rebuild has no network to gather candidates on
// and has to be done again when it comes back. Pure, so it is tested
// without a network (tests/test_voice_lifecycle.cpp); VoiceEngine maps
// QNetworkInformation's enums onto these.

namespace voice::netchange {

enum class Reach { Unknown, Disconnected, Local, Site, Online };

struct State {
    Reach reach = Reach::Unknown;
    int medium = 0;   // QNetworkInformation::TransportMedium; 0 = Unknown
};

enum class Action {
    Ignore,    // nothing that moves media paths
    Offline,   // the network went away: cancel any pending rebuild, wait
    Rebuild,   // a real path change (or back online): rebuild once
};

inline Action classify(const State& prev, const State& cur)
{
    if (cur.reach == Reach::Disconnected)
        return prev.reach == Reach::Disconnected ? Action::Ignore : Action::Offline;
    if (cur.reach == Reach::Unknown) return Action::Ignore;
    // The first real reading (backend just loaded, or was settling).
    if (prev.reach == Reach::Unknown) return Action::Ignore;
    // Back from no network at all: the one rebuild an outage needs.
    if (prev.reach == Reach::Disconnected) return Action::Rebuild;
    if (cur.reach == prev.reach) {
        if (cur.medium == prev.medium) return Action::Ignore;
        // Medium learnt or forgotten, same reachability: settling, not a
        // new path.
        if (prev.medium == 0 || cur.medium == 0) return Action::Ignore;
    }
    return Action::Rebuild;
}

} // namespace voice::netchange
