#pragma once

#include <QString>
#include <QVector>

// Which channel to open when a server comes to the foreground.
//
// Extracted from the two QML shells (main.qml, mobile/MobileMain.qml) because
// the interesting part is not "does a channel get selected" but WHICH of three
// outcomes happens, and the failure mode is silent: the user lands on the
// "Pick a channel" empty state and it looks like the app forgot them.
//
// Deliberately total — every input maps to one of restore / fall back / wait.
// The caller never has to decide what an empty answer means, because
// `Outcome` says so.
namespace bsfchat::client {

// A channel as the sidebar knows it. Categories are containers, not
// destinations; voice channels are never auto-opened because landing in one
// would put the user's microphone on the network without them asking.
struct ChannelRestoreCandidate {
    QString roomId;
    bool isVoice = false;
    bool isCategory = false;
};

struct ChannelRestoreChoice {
    enum class Outcome {
        // Open `roomId` — it is the channel the user last had open here.
        Remembered,
        // Open `roomId` — the remembered channel is gone (deleted, or we lost
        // VIEW_CHANNEL on it), so this is the first text channel instead.
        Fallback,
        // Open nothing YET. We remember a channel but this server's channel
        // list is still empty, so we cannot tell "deleted" from "sync hasn't
        // delivered it". Falling back here would both flash the wrong channel
        // and overwrite the memory with the fallback.
        Wait,
        // Open nothing. There is genuinely no text channel to open.
        Nothing,
    };

    Outcome outcome = Outcome::Nothing;
    QString roomId;

    friend bool operator==(const ChannelRestoreChoice&, const ChannelRestoreChoice&) = default;
};

// `remembered` — the persisted last-opened text channel for this server, or ""
//   if we have never recorded one.
// `channels` — every channel currently known for this server, in the order the
//   sidebar shows them. Empty means sync has not delivered the list yet.
// `syncComplete` — this connection has finished its initial /sync, so
//   `channels` is the whole world rather than a partial view. Only consulted
//   when the remembered channel is missing: before the initial sync lands,
//   "missing" means "not here yet", not "gone".
inline ChannelRestoreChoice chooseChannelToRestore(const QString& remembered,
                                          const QVector<ChannelRestoreCandidate>& channels,
                                          bool syncComplete)
{
    if (!remembered.isEmpty()) {
        for (const auto& c : channels) {
            if (c.roomId != remembered) continue;
            // Known — but only openable if it is still a text channel. A
            // channel converted to voice falls through to the fallback rather
            // than dropping the user into a call.
            if (!c.isVoice && !c.isCategory)
                return {ChannelRestoreChoice::Outcome::Remembered, remembered};
            break;
        }
        // Remembered but not present. If the channel list is still arriving,
        // hold: the alternative is to pick a fallback that the persist hook
        // then writes over the real memory.
        if (!syncComplete) return {ChannelRestoreChoice::Outcome::Wait, {}};
    }

    for (const auto& c : channels) {
        if (!c.isVoice && !c.isCategory)
            return {ChannelRestoreChoice::Outcome::Fallback, c.roomId};
    }

    // No text channels at all. If sync is still in flight that is "not yet";
    // once it has landed it is the truth about this server.
    if (!syncComplete) return {ChannelRestoreChoice::Outcome::Wait, {}};
    return {ChannelRestoreChoice::Outcome::Nothing, {}};
}

} // namespace bsfchat::client
