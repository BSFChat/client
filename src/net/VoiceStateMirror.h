#pragma once

// The UI-facing copy of VoiceSession's mute/deafen toggles.
//
// ServerConnection exposes voiceMuted/voiceDeafened as QML properties. They
// used to be a separate pair of bools that teardownVoiceSession() reset to
// false on every leave or channel switch, while VoiceSession deliberately
// keeps the user's choice across both (V-M7). The dock then showed "live"
// while the mic was actually shut, and the next click did the opposite of
// what it showed (audit 3, fix-first #8). The mirror now has exactly one
// writer: it is always a copy of the session, never set on its own.

#include "net/VoiceSession.h"

struct VoiceStateMirror {
    bool muted = false;
    bool deafened = false;

    struct Changes {
        bool muted = false;
        bool deafened = false;
    };

    // Copy the session's state; reports which flags moved so the caller
    // emits only what changed (or everything, if it prefers).
    Changes syncFrom(const VoiceSession& session)
    {
        Changes c;
        c.muted = muted != session.muted();
        c.deafened = deafened != session.deafened();
        muted = session.muted();
        deafened = session.deafened();
        return c;
    }
};
