#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// "Is this user a bot?", and the rule for how often we are allowed to ask.
//
// The server answers the question one user at a time: GET /profile/{userId}
// gained a `bsfchat.bot` boolean, and /account/whoami reports it for the
// caller. There is no bulk source a non-admin can read — the admin endpoint
// GET /bsfchat/bots is gated on MANAGE_BOTS, so an ordinary member who can
// merely SEE a bot in the roster has only the per-user profile call.
//
// That shapes this class, and the shape is the point:
//
//   * The answer is cached per user id for the life of the connection. A
//     badge is drawn once per message row and the roster repaints on every
//     sync, so anything that asked the network at paint time would issue
//     hundreds of requests to render one channel. The models read this cache
//     synchronously and never fetch.
//
//   * A user is PROBED at most once per session, whatever the answer. Without
//     that, a user whose profile 404s (left the server, deactivated account)
//     would be re-asked every time their name appeared, which is exactly the
//     per-message fetch the cache exists to avoid. `claimProbe` is the single
//     gate: it hands out permission to ask, once, and remembers that it did
//     even if the reply never arrives.
//
//   * Probes are handed out in bounded batches. Switching channels makes a
//     whole roster visible at once; firing one GET per member in the same
//     event-loop turn is a self-inflicted burst against the server for
//     information that is only ever cosmetic. `takeProbeBatch` drains the
//     backlog a few at a time, and the caller paces it on a timer.
//
// Split out of ServerConnection so the policy can be tested without a network
// stack, an event loop or a server — same reason as util/VoiceRoster.h and
// util/MemberCache.h. ServerConnection owns one instance and hands the models
// a const pointer to it.
namespace bsfchat::client {

class BotRegistry {
public:
    // Tri-state on purpose. "Not known to be a bot" and "known not to be a
    // bot" are different facts here: the first still owes a probe, the second
    // is settled. Collapsing them into a bool would make every unprobed user
    // look like a settled human and nothing would ever ask.
    enum class Flag { Unknown, Human, Bot };

    Flag lookup(const QString& userId) const
    {
        auto it = m_flags.find(userId);
        return it == m_flags.end() ? Flag::Unknown : *it;
    }

    // What the UI asks. An unprobed user renders as a human: a badge that
    // appears a beat late is a far smaller wrong than one that flickers onto
    // every name while the answers come in.
    bool isBot(const QString& userId) const
    {
        return lookup(userId) == Flag::Bot;
    }

    // Record an authoritative answer (a profile reply, or /whoami for us).
    // Returns true when the stored value actually moved, so the caller can
    // skip a repaint of every member row for a reply that said nothing new.
    bool record(const QString& userId, bool isBot)
    {
        if (userId.isEmpty()) return false;
        const Flag next = isBot ? Flag::Bot : Flag::Human;
        auto it = m_flags.find(userId);
        if (it != m_flags.end() && *it == next) return false;
        m_flags.insert(userId, next);
        // A settled answer retires any outstanding probe for this user: the
        // admin bot list can settle someone we had already queued.
        m_pending.removeAll(userId);
        m_probed.insert(userId);
        return true;
    }

    // Fold in the admin bot list (GET /bsfchat/bots), which arrives only for
    // callers holding MANAGE_BOTS. Returns true if anything changed.
    //
    // POSITIVE EVIDENCE ONLY: every id in the list is a bot, and users absent
    // from it are left Unknown rather than marked Human. The endpoint's scope
    // is the server's to decide — it may one day paginate, or filter to the
    // bots the caller owns — and a client that read absence as "human" would
    // then quietly un-badge real bots for the one user most likely to notice.
    // Marking humans is the profile call's job, which cannot be wrong about it.
    bool recordBotList(const QStringList& botUserIds)
    {
        bool changed = false;
        for (const QString& id : botUserIds) {
            if (record(id, true)) changed = true;
        }
        return changed;
    }

    // Ask for permission to send one GET /profile for this user. Returns true
    // exactly once per user id per session, and only while the answer is still
    // unknown. Everything that wants a flag calls this and ignores a false.
    bool claimProbe(const QString& userId)
    {
        if (userId.isEmpty()) return false;
        if (m_probed.contains(userId)) return false;
        if (m_flags.contains(userId)) return false;
        m_probed.insert(userId);
        return true;
    }

    // Queue a user to be probed later. Same once-per-session guarantee as
    // claimProbe — this simply defers the request instead of issuing it now.
    // Returns true if the user was newly queued.
    bool enqueueProbe(const QString& userId)
    {
        if (!claimProbe(userId)) return false;
        m_pending.append(userId);
        return true;
    }

    // Hand back up to `max` queued user ids to probe now, removing them from
    // the queue. They are already marked probed, so they will not come back.
    QStringList takeProbeBatch(qsizetype max)
    {
        QStringList out;
        while (!m_pending.isEmpty() && out.size() < max) {
            out.append(m_pending.takeFirst());
        }
        return out;
    }

    bool hasPendingProbes() const { return !m_pending.isEmpty(); }
    qsizetype pendingProbeCount() const { return m_pending.size(); }

    // The cache is per-connection state: a logout, or a reconnect as a
    // different user, must not carry answers across. Clears the probe ledger
    // too, so the new session re-asks.
    void clear()
    {
        m_flags.clear();
        m_probed.clear();
        m_pending.clear();
    }

    // For the models, which hold a pointer to the owning registry.
    qsizetype knownCount() const { return m_flags.size(); }

private:
    QHash<QString, Flag> m_flags;
    // Every id we have ever issued or queued a probe for, answered or not.
    QSet<QString> m_probed;
    QStringList m_pending;
};

} // namespace bsfchat::client
