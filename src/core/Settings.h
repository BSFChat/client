#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class QMediaDevices;

class Settings : public QObject {
    Q_OBJECT
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    // Accent color as a "#rrggbb" string. Used for highlights, active
    // channel chips, focused controls, and (in accessibility mode) panel
    // borders. Theme.qml consumes this via the AppSettings QML singleton.
    Q_PROPERTY(QString accent READ accent WRITE setAccent NOTIFY accentChanged)
    // Hue int driving the Designer-kit accent palette — one of 180 (cyan),
    // 260 (violet), 320 (magenta), 30 (amber). Theme.qml binds to this
    // directly; the legacy `accent` hex is kept for accessibility-border
    // tinting but the swatches write the hue.
    Q_PROPERTY(int accentHue READ accentHue WRITE setAccentHue NOTIFY accentHueChanged)
    // Accessibility mode draws strong, high-contrast borders between the
    // server sidebar / channel list / message view / member list so panel
    // boundaries are obvious to low-vision users.
    Q_PROPERTY(bool accessibilityMode READ accessibilityMode WRITE setAccessibilityMode NOTIFY accessibilityModeChanged)
    Q_PROPERTY(bool verboseVoiceLogging READ verboseVoiceLogging WRITE setVerboseVoiceLogging NOTIFY verboseVoiceLoggingChanged)
    Q_PROPERTY(bool showVideoDiagnostics READ showVideoDiagnostics WRITE setShowVideoDiagnostics NOTIFY showVideoDiagnosticsChanged)
    // Layout density — one of "standard" / "compact" / "focus". Matches the
    // three branches in Theme.layout (see qml/theme/Theme.qml). Compact
    // narrows sidebars & shrinks participant tiles; focus hides chat +
    // member list. Stored as a string so Theme.variant can bind directly.
    Q_PROPERTY(QString layoutVariant READ layoutVariant WRITE setLayoutVariant NOTIFY layoutVariantChanged)
    // Audio: preferred input/output device description strings (human-readable
    // names from QMediaDevices). Empty == system default. Volume is 0..100.
    Q_PROPERTY(QString audioInputDevice READ audioInputDevice WRITE setAudioInputDevice NOTIFY audioInputDeviceChanged)
    Q_PROPERTY(QString audioOutputDevice READ audioOutputDevice WRITE setAudioOutputDevice NOTIFY audioOutputDeviceChanged)
    Q_PROPERTY(int inputVolume READ inputVolume WRITE setInputVolume NOTIFY inputVolumeChanged)
    Q_PROPERTY(int outputVolume READ outputVolume WRITE setOutputVolume NOTIFY outputVolumeChanged)
    // Notifications (placeholder — not yet routed through the OS; setting
    // persists so the UI keeps the user's choice across restarts.)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)
    Q_PROPERTY(bool notificationSound READ notificationSound WRITE setNotificationSound NOTIFY notificationSoundChanged)
    // Whether the right-hand member list is expanded. Also toggleable via
    // the chat-header users button and the Ctrl+M shortcut.
    Q_PROPERTY(bool showMemberList READ showMemberList WRITE setShowMemberList NOTIFY showMemberListChanged)
    // Persisted window geometry. Negative values == "use default" (first
    // run, or the saved position was off-screen / on a detached monitor).
    // We save each field independently so a partial restore still works if
    // QSettings was hand-edited.
    Q_PROPERTY(int windowX READ windowX WRITE setWindowX NOTIFY windowXChanged)
    Q_PROPERTY(int windowY READ windowY WRITE setWindowY NOTIFY windowYChanged)
    Q_PROPERTY(int windowWidth READ windowWidth WRITE setWindowWidth NOTIFY windowWidthChanged)
    Q_PROPERTY(int windowHeight READ windowHeight WRITE setWindowHeight NOTIFY windowHeightChanged)
    Q_PROPERTY(int windowVisibility READ windowVisibility WRITE setWindowVisibility NOTIFY windowVisibilityChanged)
    // List of {description, id} maps for audio devices; populated live from
    // QMediaDevices. The id isn't persistent across reboots on every OS, so
    // selection is stored by description and resolved on startup.
    // NOTIFY, not CONSTANT: the lists are enumerated fresh on every read, so
    // a headset plugged in after startup does show up — but with CONSTANT no
    // binding ever re-read them, so the combo boxes were frozen at the set of
    // devices present when the dialog first loaded.
    //
    // audioDevicesChanged is now driven by a live QMediaDevices hot-plug
    // subscription owned by the constructor, not only by the dialog's
    // onAboutToShow. Re-enumerating on open was still one open too late for
    // the case that prompted this: a Bluetooth headset that connects WHILE
    // the dialog is open — which is exactly what a user who cannot find
    // their AirPods in the list does next.
    //
    // The first entry of each list is the "follow the system default"
    // choice: stored as an empty string, flagged with systemDefault:true,
    // and labelled with the device it currently resolves to ("System
    // default (Josh's AirPods Pro)"), so the list says which device that
    // actually is instead of leaving the user to guess.
    Q_PROPERTY(QVariantList audioInputDevices READ audioInputDevices NOTIFY audioDevicesChanged)
    Q_PROPERTY(QVariantList audioOutputDevices READ audioOutputDevices NOTIFY audioDevicesChanged)
    // The device the voice pipeline is ACTUALLY on right now, empty when
    // not in a call. Distinct from the preference above, which can be
    // "system default" or can name a device that has since gone away.
    // See core/AudioDeviceStatus.h.
    Q_PROPERTY(QString audioInputInUse READ audioInputInUse NOTIFY audioInUseChanged)
    Q_PROPERTY(QString audioOutputInUse READ audioOutputInUse NOTIFY audioInUseChanged)

public:
    // Re-publish the device lists. Cheap (a QMediaDevices enumeration).
    //
    // Must stay under an access specifier that makes it public. It was
    // declared in the leading block above with the Q_PROPERTYs, which reads
    // as if it belongs with them but is `private` in a class, and moc only
    // exposes public invokables to QML — so ClientSettings.qml's
    // onAboutToShow died on "Property 'refreshAudioDevices' of object
    // Settings is not a function" and took the rest of the handler
    // (the combo-box resync) with it. tests/test_qml_hygiene.cpp now fails
    // the build if any Q_INVOKABLE drifts back under a non-public specifier.
    Q_INVOKABLE void refreshAudioDevices();

    // Persist a device choice from the combo boxes. The description is
    // the key, exactly as before — ids are not stable across reboots on
    // every platform. The id rides along as a hint so that when several
    // devices share one description the pipeline can prefer whichever
    // one the user actually picked. Empty description == follow the
    // system default.
    Q_INVOKABLE void selectAudioInputDevice(const QString& description,
                                            const QString& id);
    Q_INVOKABLE void selectAudioOutputDevice(const QString& description,
                                             const QString& id);

    explicit Settings(QObject* parent = nullptr);

    struct ServerEntry {
        QString url;
        QString userId;
        QString accessToken;
        QString deviceId;
        QString displayName;
        QString identityRefreshToken;
        QString identityProviderUrl;
    };

    // Server persistence
    QList<ServerEntry> savedServers() const;
    void addServer(const ServerEntry& entry);
    void removeServer(int index);
    void updateServer(int index, const ServerEntry& entry);

    // Active server
    int activeServerIndex() const;
    void setActiveServerIndex(int index);

    // UI preferences
    int fontSize() const;
    void setFontSize(int size);

    QString theme() const;
    void setTheme(const QString& theme);

    QString accent() const;
    void setAccent(const QString& accent);

    int accentHue() const;
    void setAccentHue(int hue);

    bool accessibilityMode() const;
    void setAccessibilityMode(bool v);

    // Advanced: runtime toggle for the bsfchat.* debug/info logging
    // categories (applies immediately, persists across launches).
    bool verboseVoiceLogging() const;
    void setVerboseVoiceLogging(bool v);
    // Advanced: per-stream receive stats overlay on video tiles.
    bool showVideoDiagnostics() const;
    void setShowVideoDiagnostics(bool v);
    // Folder the rotating file log writes into (FileLogger).
    Q_INVOKABLE QString logDirectory() const;

    QString layoutVariant() const;
    void setLayoutVariant(const QString& variant);

    QString audioInputDevice() const;
    void setAudioInputDevice(const QString& desc);
    QString audioOutputDevice() const;
    void setAudioOutputDevice(const QString& desc);
    int inputVolume() const;
    void setInputVolume(int v);
    int outputVolume() const;
    void setOutputVolume(int v);
    bool notificationsEnabled() const;
    void setNotificationsEnabled(bool v);
    bool notificationSound() const;
    void setNotificationSound(bool v);

    bool showMemberList() const;
    void setShowMemberList(bool v);

    int windowX() const;
    void setWindowX(int v);
    int windowY() const;
    void setWindowY(int v);
    int windowWidth() const;
    void setWindowWidth(int v);
    int windowHeight() const;
    void setWindowHeight(int v);
    int windowVisibility() const;
    void setWindowVisibility(int v);

    // Video pop-out window geometry, remembered PER KIND ("screen" /
    // "camera") rather than per feed: a feed key carries a user id, so
    // per-feed memory would grow an entry for every person ever watched,
    // and a screen share and a webcam want very different windows anyway.
    //
    // A map rather than eight Q_PROPERTYs because QML reads it once, on
    // window creation, and writes it back debounced on move/resize —
    // there is nothing to bind to. Returns { x, y, width, height } with
    // -1 for anything never stored; qml/js/VideoWindows.js turns that
    // into concrete geometry (restoreGeometry) and is where the clamping
    // and the is-that-monitor-still-here check live.
    //
    // PUBLIC on purpose: moc records a private Q_INVOKABLE but the
    // metaobject does not offer it to QML, so it fails at runtime with
    // "not a function". test_qml_hygiene.cpp fails if one moves up into
    // the Q_PROPERTY block at the top of the class.
    Q_INVOKABLE QVariantMap popoutGeometry(const QString& kind) const;
    Q_INVOKABLE void setPopoutGeometry(const QString& kind,
                                       int x, int y, int width, int height);

    QVariantList audioInputDevices() const;
    QVariantList audioOutputDevices() const;
    QString audioInputInUse() const;
    QString audioOutputInUse() const;

    // Category collapse state
    QStringList collapsedCategories() const;
    void setCollapsedCategories(const QStringList& categories);

    // Per-room "last read" timestamp (ms since epoch) for the unread-
    // messages divider. Returns 0 if never seen (caller treats as "no
    // boundary — don't show a divider"). Stored under unread/<roomId>.
    Q_INVOKABLE qint64 lastReadTs(const QString& roomId) const;
    // Emits lastReadTsChanged(roomId) when the stored value actually moves,
    // so the channel list can drop its 800 ms unread poll (U-M4).
    Q_INVOKABLE void setLastReadTs(const QString& roomId, qint64 tsMs);
    // First-sight seeding of the marker (see core/ReadState.h). Writes only
    // when the room has no marker at all, so it can never move a real one.
    // Returns true if it wrote.
    bool seedLastReadTs(const QString& roomId, qint64 seedTs);
    // The channel-list unread dot. lastMessageTs is the room's newest
    // origin_server_ts; both sides of the comparison are server clock.
    Q_INVOKABLE bool isRoomUnread(const QString& roomId, qint64 lastMessageTs) const;

    // Muted rooms — the channel list dims them and suppresses their
    // unread dot. Stored as a QStringList under mutedRooms.
    Q_INVOKABLE bool isRoomMuted(const QString& roomId) const;
    Q_INVOKABLE void setRoomMuted(const QString& roomId, bool muted);

    // Per-room notification mode.
    //   "all"      — notify for every inbound message (default)
    //   "mentions" — only when the user is @-mentioned
    //   "none"     — never (equivalent to muted for notification
    //                purposes; the channel row still shows unread)
    // Stored under notifMode/<roomId>. Empty/missing ⇒ "all".
    Q_INVOKABLE QString roomNotificationMode(const QString& roomId) const;
    Q_INVOKABLE void setRoomNotificationMode(const QString& roomId,
                                             const QString& mode);

    // Last-active text channel per server. Used on startup to drop
    // the user back into the channel they were reading, rather than
    // always jumping to the first text room. Keyed on the server URL
    // (stable across sessions). Voice rooms deliberately never get
    // persisted — auto-rejoining voice would transmit the user's mic
    // the moment the app opens, which is a very bad default.
    Q_INVOKABLE QString lastTextRoomFor(const QString& serverUrl) const;
    Q_INVOKABLE void setLastTextRoomFor(const QString& serverUrl,
                                        const QString& roomId);

    // Legacy screen-share quality preset (0=Low..3=Ultra). Retained
    // for migration only — readers should use the explicit fps /
    // maxWidth / jpegQuality fields below. We translate the preset
    // to those values on first launch and never write the preset
    // again. Removing it entirely would break upgrades from <0.0.24
    // by losing the user's existing preference.
    Q_INVOKABLE int screenShareQuality() const;
    Q_INVOKABLE void setScreenShareQuality(int level);

    // Direct screen-share quality knobs — what users actually want
    // to control. Allowed ranges are wide enough to cover "1 fps
    // metered cellular" through "60 fps 4K Q100"; the
    // ScreenShareController clamps the effective value to
    // min(user, server-policy).
    Q_PROPERTY(int screenShareFps READ screenShareFps WRITE setScreenShareFps NOTIFY screenShareFpsChanged)
    Q_PROPERTY(int screenShareMaxWidth READ screenShareMaxWidth WRITE setScreenShareMaxWidth NOTIFY screenShareMaxWidthChanged)
    Q_PROPERTY(int screenShareJpegQuality READ screenShareJpegQuality WRITE setScreenShareJpegQuality NOTIFY screenShareJpegQualityChanged)
    int screenShareFps() const;
    void setScreenShareFps(int fps);
    int screenShareMaxWidth() const;
    void setScreenShareMaxWidth(int px);
    int screenShareJpegQuality() const;
    void setScreenShareJpegQuality(int q);

    // RTP-video quality knobs. Target bitrate is the steady-state
    // budget the adaptive controller converges toward on a clean
    // link; the controller may exceed it briefly (probe) or ride far
    // below it under loss. jpegQuality above only governs the legacy
    // fallback path now. Lossless flips the AV1 mathematically-
    // lossless tier (LAN-class bandwidth; server policy can veto it).
    Q_PROPERTY(int screenShareTargetKbps READ screenShareTargetKbps WRITE setScreenShareTargetKbps NOTIFY screenShareTargetKbpsChanged)
    Q_PROPERTY(int screenShareKeyframeSec READ screenShareKeyframeSec WRITE setScreenShareKeyframeSec NOTIFY screenShareKeyframeSecChanged)
    Q_PROPERTY(bool screenShareLossless READ screenShareLossless WRITE setScreenShareLossless NOTIFY screenShareLosslessChanged)
    // Which RTP video codec shares and camera feeds are encoded in:
    //   "auto"       — H.265 whenever it is available to EVERYONE in
    //                  the call, H.264 otherwise (default)
    //   "preferHevc" — same rule; reserved for when "auto" learns to
    //                  weigh CPU/battery as well as capability
    //   "h264Only"   — never negotiate H.265
    // Anything else read back from disk normalises to "auto". This is
    // a PREFERENCE, not a guarantee: a mesh encodes once per stream, so
    // one viewer that cannot decode H.265 puts the stream back on
    // H.264 whatever this says. See video/VideoCodecSelect.h.
    Q_PROPERTY(QString videoCodecPreference READ videoCodecPreference WRITE setVideoCodecPreference NOTIFY videoCodecPreferenceChanged)
    int screenShareTargetKbps() const;
    void setScreenShareTargetKbps(int kbps);
    int screenShareKeyframeSec() const;
    void setScreenShareKeyframeSec(int sec);
    bool screenShareLossless() const;
    void setScreenShareLossless(bool on);
    QString videoCodecPreference() const;
    void setVideoCodecPreference(const QString& pref);

    // Camera knobs (previously hardcoded 640 px / 5 fps JPEG).
    Q_PROPERTY(int cameraFps READ cameraFps WRITE setCameraFps NOTIFY cameraFpsChanged)
    Q_PROPERTY(int cameraMaxWidth READ cameraMaxWidth WRITE setCameraMaxWidth NOTIFY cameraMaxWidthChanged)
    Q_PROPERTY(int cameraTargetKbps READ cameraTargetKbps WRITE setCameraTargetKbps NOTIFY cameraTargetKbpsChanged)
    int cameraFps() const;
    void setCameraFps(int fps);
    int cameraMaxWidth() const;
    void setCameraMaxWidth(int px);
    int cameraTargetKbps() const;
    void setCameraTargetKbps(int kbps);

    // Received-video smoothing: the most delay (ms) the playout buffer
    // may add to other people's video to show it at an even pace.
    // 0 = off, frames shown the moment they decode (lowest latency).
    // Envelope 0..400, default 150 — the reasoning is at
    // Settings::videoSmoothingMs() in Settings.cpp.
    Q_PROPERTY(int videoSmoothingMs READ videoSmoothingMs WRITE setVideoSmoothingMs NOTIFY videoSmoothingMsChanged)
    int videoSmoothingMs() const;
    void setVideoSmoothingMs(int ms);

    // Voice mode: "open" ⇒ open mic (current behaviour), "ptt" ⇒
    // push-to-talk. In PTT the mic only transmits while the user is
    // holding down `pttKeySequence`.
    Q_PROPERTY(QString voiceMode READ voiceMode WRITE setVoiceMode NOTIFY voiceModeChanged)
    Q_PROPERTY(QString pttKeySequence READ pttKeySequence WRITE setPttKeySequence NOTIFY pttKeySequenceChanged)
    QString voiceMode() const;
    void setVoiceMode(const QString& v);
    QString pttKeySequence() const;
    void setPttKeySequence(const QString& seq);

    // "Hide my IP address". "auto" ⇒ follow the server (peer-to-peer when it
    // allows it, which is the faster route); "relayOnly" ⇒ always route this
    // client's calls through the server's relay, so peers — and the room — see
    // the relay's address instead of this machine's.
    //
    // Stored as a string rather than a bool so a future third option (relay
    // only outside the LAN, say) does not have to migrate anybody's settings,
    // and so an unrecognised value reads as "auto" — the safe direction for a
    // setting whose other value can refuse a join.
    //
    // Normalised on the way in AND on the way out: voice::relayModeFromString
    // treats anything it does not recognise as Auto, so a hand-edited or
    // downgraded settings file cannot leave the client in a state that is
    // neither.
    Q_PROPERTY(QString voiceRelayMode READ voiceRelayMode WRITE setVoiceRelayMode NOTIFY voiceRelayModeChanged)
    QString voiceRelayMode() const;
    void setVoiceRelayMode(const QString& mode);

    // Auto-update check on launch + periodic re-poll. Defaults to
    // ON for desktop builds — Windows users in particular have
    // been complaining about the manual-MSI-download cadence, and
    // the Updater class hits GitHub Releases at a polite 6h
    // interval so the network impact is minimal.
    Q_PROPERTY(bool autoUpdateCheck READ autoUpdateCheck WRITE setAutoUpdateCheck NOTIFY autoUpdateCheckChanged)
    bool autoUpdateCheck() const;
    void setAutoUpdateCheck(bool v);

    // Which release channel the Updater offers builds from:
    //   "stable" — published releases only (default)
    //   "beta"   — stable releases AND prereleases (RCs), newest wins
    // Anything else read back from disk normalises to "stable". Opting
    // out never downgrades a running prerelease build; see
    // core/ReleaseSelection.h (Outcome::AheadOfChannel).
    Q_PROPERTY(QString updateChannel READ updateChannel WRITE setUpdateChannel NOTIFY updateChannelChanged)
    QString updateChannel() const;
    void setUpdateChannel(const QString& channel);
signals:
    void updateChannelChanged();
    void mutedRoomsChanged();
    void screenShareQualityChanged();
    void screenShareFpsChanged();
    void screenShareMaxWidthChanged();
    void screenShareJpegQualityChanged();
    void screenShareTargetKbpsChanged();
    void screenShareKeyframeSecChanged();
    void screenShareLosslessChanged();
    void videoCodecPreferenceChanged();
    void cameraFpsChanged();
    void cameraMaxWidthChanged();
    void cameraTargetKbpsChanged();
    void videoSmoothingMsChanged();
    void voiceModeChanged();
    void pttKeySequenceChanged();
    void voiceRelayModeChanged();
    void autoUpdateCheckChanged();
public:

signals:
    // Per-room read marker moved. Carries the room so a listener can decide
    // whether it cares; the channel list simply bumps its generation counter.
    void lastReadTsChanged(const QString& roomId);
    // The available audio input/output device sets may have changed.
    void audioDevicesChanged();
    // The device the voice pipeline is on has changed.
    void audioInUseChanged();

    void fontSizeChanged();
    void themeChanged();
    void accentChanged();
    void accentHueChanged();
    void accessibilityModeChanged();
    void verboseVoiceLoggingChanged();
    void showVideoDiagnosticsChanged();
    void layoutVariantChanged();
    void audioInputDeviceChanged();
    void audioOutputDeviceChanged();
    void inputVolumeChanged();
    void outputVolumeChanged();
    void notificationsEnabledChanged();
    void notificationSoundChanged();
    void showMemberListChanged();
    void windowXChanged();
    void windowYChanged();
    void windowWidthChanged();
    void windowHeightChanged();
    void windowVisibilityChanged();

private:
    mutable QSettings m_settings;
    // One instance for the whole process, owned here because Settings is
    // a QML singleton on the GUI thread and QMediaDevices wants a thread
    // with an event loop. Its only job is to keep audioDevicesChanged
    // firing while a dialog is open.
    QMediaDevices* m_mediaDevices = nullptr;
};
