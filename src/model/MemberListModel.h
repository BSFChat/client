#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QMap>
#include <QString>
#include <QVector>

#include <bsfchat/MatrixTypes.h>

class QTimer;

class MemberListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        UserIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        AvatarUrlRole,
        MembershipRole,
        // The per-server nickname, empty when the member has none.
        //
        // DisplayNameRole is already the name to RENDER — the server puts the
        // effective name (nickname if set, else global) in the member event's
        // `displayname`, so nothing has to choose between them at paint time. This
        // role exists so the UI can tell WHY a name is what it is: an admin needs
        // "clear nickname" to be distinguishable from "reset to nothing", and a
        // profile card wants to show the global name underneath.
        NicknameRole,
        // True when this member is a bot account, for the BOT badge.
        //
        // Carried by the member event itself, in `bsfchat.bot`, exactly like
        // the nickname above — so it arrives with the row rather than after
        // it, and there is nothing to reconcile. The server derives the flag
        // from the user id at read time and writes it on every member event
        // it serves (stored state, /members, initial and incremental sync,
        // leaves included), and scrubs any forged value out of client-sent
        // content, so what lands here is both complete and trustworthy.
        //
        // ABSENT MEANS FALSE. The server never writes `false`, so a missing
        // key is the normal encoding for "human" and must not be read as
        // "unknown" — there is nothing to go and ask.
        IsBotRole,
        // True while this member's voice is above the speaking floor.
        //
        // UNLIKE EVERY OTHER ROLE HERE, this one is not carried by the
        // m.room.member event. It is voice state, and it is pushed in from
        // outside by ServerConnection — see the block below roleNames() for
        // where it comes from and why it is a bool rather than a level.
        //
        // ABSENT MEANS FALSE, in the same sense IsBotRole means it: a member
        // nobody has reported a level for is silent, not unknown.
        IsSpeakingRole
    };

    explicit MemberListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void processEvent(const bsfchat::RoomEvent& event);
    void clear();

    // Invokable because QML calls it (MessageBubble's reaction tooltip did, and
    // silently got "not a function" because it was a plain member).
    Q_INVOKABLE QString displayNameForUser(const QString& userId) const;

    // The member's per-server nickname, or an empty string when they have none.
    Q_INVOKABLE QString nicknameForUser(const QString& userId) const;

    // Global user display-name cache (owned by ServerConnection). Used as a
    // fallback when the member event didn't carry a displayname (older
    // rooms, or events written before the server's broadcast was added).
    void setDisplayNameCache(const QMap<QString, QString>* cache) { m_dnCache = cache; }
    // Re-resolve every member's display name from the cache and refresh.
    void refreshDisplayNames();

    // Whether a member is a bot, for QML that has a user id but not a row.
    // Invokable for the same reason displayNameForUser is — QML calls it
    // directly. False for anyone not in this room's roster.
    Q_INVOKABLE bool isBot(const QString& userId) const;

    // ── Voice speaking state ────────────────────────────────────────────
    //
    // WHERE THIS COMES FROM. There is one source of speaking information in
    // the client and it is not here: VoiceEngine's `micLevelChanged` (our own
    // mic, 50 Hz) and `peerLevelChanged` (per remote peer, ~12.5 Hz), which
    // ServerConnection already fans out to the voice grid as the `micLevel`
    // property and the `peerLevel()` invokable. ServerConnection pushes those
    // same numbers in here through setVoiceLevel(), so the member row and the
    // ParticipantTile are two views of one signal rather than two mechanisms
    // that can disagree. Nothing in this class samples audio or decides who
    // is in a call.
    //
    // WHY A BOOL AND NOT THE LEVEL. A float role would emit dataChanged at
    // the level rate — up to 50 Hz, for every row in the list. The member
    // row's ACCESSIBLE NAME contains "speaking", so each of those would be a
    // QAccessible::NameChanged, and a screen reader would be re-reading rows
    // continuously for the whole call. The threshold and the release hold are
    // therefore applied once, here, and dataChanged fires only on a genuine
    // transition.
    //
    // The floor a level has to clear to count as speech. Matches the value
    // AudioWorker documents as the point the speaking ring lights
    // (src/voice/AudioWorker.cpp) and the one VoiceParticipantList.qml uses.
    // ParticipantTile.qml independently uses 0.04 — a pre-existing
    // divergence, left alone here rather than reaching into the voice grid.
    static constexpr float kSpeakingLevelFloor = 0.05f;
    // How long a member stays speaking after their level last cleared the
    // floor. Speech is not continuous: the gaps between words sit below the
    // floor, so with no hold the flag would chatter several times a second —
    // a strobing ring, and an accessible name that changes mid-word. 400 ms
    // is longer than an inter-word gap and far shorter than a conversational
    // turn.
    static constexpr qint64 kSpeakingHoldMs = 400;

    // Feed one level sample. Above the floor (re)arms the hold; at or below
    // it does nothing at all — release is the hold expiring, never this call.
    void setVoiceLevel(const QString& userId, float level);
    // Immediate release for one member. For the peer-disconnected edge, where
    // waiting out the hold would leave a ring on somebody who has gone.
    void clearVoiceLevel(const QString& userId);
    // Immediate release for everybody — the call ended.
    void clearVoiceLevels();
    // For QML that has a user id but not a row, the same reason isBot() is
    // invokable. False for anyone nobody has reported a level for.
    Q_INVOKABLE bool isSpeaking(const QString& userId) const;

    // Clock seams. The three calls above read the wall clock; tests drive
    // these directly so that asserting the hold does not cost 400 ms of test
    // time per case, and so a slow machine cannot turn a timing assertion
    // into a flake.
    void setVoiceLevelAt(const QString& userId, float level, qint64 nowMs);
    // Releases every member whose hold has run out by `nowMs`. Returns how
    // many were released.
    int expireSpeakingAt(qint64 nowMs);

private:
    int findMember(const QString& userId) const;

    struct MemberEntry {
        QString userId;
        QString displayName;
        QString avatarUrl;
        QString membership;
        QString nickname;
        bool isBot = false;
    };

    QVector<MemberEntry> m_members;
    const QMap<QString, QString>* m_dnCache = nullptr;

    // Speaking, keyed by user id and NOT stored on MemberEntry: a level can
    // arrive before the member event that creates the row (joining a call and
    // a channel race), and the roster is cleared and rebuilt on every room
    // switch while the call carries on. Keeping it beside the rows means
    // neither of those loses it. Presence in the map IS the state; the value
    // is the wall-clock ms at which the hold runs out.
    QHash<QString, qint64> m_speakingUntil;
    // Drives the release edge. Only runs while somebody is speaking, so a
    // client sitting outside a call has no timer at all.
    QTimer* m_speakingExpiry = nullptr;

    // dataChanged for one member's IsSpeakingRole, or nothing if they have no
    // row in this room.
    void emitSpeakingChanged(const QString& userId);

    QString resolveName(const QString& userId, const QString& localName) const;
};
