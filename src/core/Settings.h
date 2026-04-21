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
signals:
    void mutedRoomsChanged();
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
