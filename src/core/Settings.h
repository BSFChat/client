#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariantList>

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
    Q_PROPERTY(QVariantList audioInputDevices READ audioInputDevices CONSTANT)
    Q_PROPERTY(QVariantList audioOutputDevices READ audioOutputDevices CONSTANT)

public:
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

    QVariantList audioInputDevices() const;
    QVariantList audioOutputDevices() const;

    // Category collapse state
    QStringList collapsedCategories() const;
    void setCollapsedCategories(const QStringList& categories);

    // Per-room "last read" timestamp (ms since epoch) for the unread-
    // messages divider. Returns 0 if never seen (caller treats as "no
    // boundary — don't show a divider"). Stored under unread/<roomId>.
    Q_INVOKABLE qint64 lastReadTs(const QString& roomId) const;
    Q_INVOKABLE void setLastReadTs(const QString& roomId, qint64 tsMs);

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
    int screenShareTargetKbps() const;
    void setScreenShareTargetKbps(int kbps);
    int screenShareKeyframeSec() const;
    void setScreenShareKeyframeSec(int sec);
    bool screenShareLossless() const;
    void setScreenShareLossless(bool on);

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

    // Voice mode: "open" ⇒ open mic (current behaviour), "ptt" ⇒
    // push-to-talk. In PTT the mic only transmits while the user is
    // holding down `pttKeySequence`.
    Q_PROPERTY(QString voiceMode READ voiceMode WRITE setVoiceMode NOTIFY voiceModeChanged)
    Q_PROPERTY(QString pttKeySequence READ pttKeySequence WRITE setPttKeySequence NOTIFY pttKeySequenceChanged)
    QString voiceMode() const;
    void setVoiceMode(const QString& v);
    QString pttKeySequence() const;
    void setPttKeySequence(const QString& seq);

    // Auto-update check on launch + periodic re-poll. Defaults to
    // ON for desktop builds — Windows users in particular have
    // been complaining about the manual-MSI-download cadence, and
    // the Updater class hits GitHub Releases at a polite 6h
    // interval so the network impact is minimal.
    Q_PROPERTY(bool autoUpdateCheck READ autoUpdateCheck WRITE setAutoUpdateCheck NOTIFY autoUpdateCheckChanged)
    bool autoUpdateCheck() const;
    void setAutoUpdateCheck(bool v);
signals:
    void mutedRoomsChanged();
    void screenShareQualityChanged();
    void screenShareFpsChanged();
    void screenShareMaxWidthChanged();
    void screenShareJpegQualityChanged();
    void screenShareTargetKbpsChanged();
    void screenShareKeyframeSecChanged();
    void screenShareLosslessChanged();
    void cameraFpsChanged();
    void cameraMaxWidthChanged();
    void cameraTargetKbpsChanged();
    void voiceModeChanged();
    void pttKeySequenceChanged();
    void autoUpdateCheckChanged();
public:

signals:
    void fontSizeChanged();
    void themeChanged();
    void accentChanged();
    void accentHueChanged();
    void accessibilityModeChanged();
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
};
