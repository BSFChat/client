#include "core/Settings.h"

#include <algorithm>
#include <QCryptographicHash>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QVariantMap>

Settings::Settings(QObject* parent)
    : QObject(parent)
    , m_settings("BSFChat", "BSFChat")
{
}

QList<Settings::ServerEntry> Settings::savedServers() const
{
    QList<ServerEntry> servers;
    int count = m_settings.beginReadArray("servers");
    for (int i = 0; i < count; ++i) {
        m_settings.setArrayIndex(i);
        ServerEntry entry;
        entry.url = m_settings.value("url").toString();
        entry.userId = m_settings.value("userId").toString();
        entry.accessToken = m_settings.value("accessToken").toString();
        entry.deviceId = m_settings.value("deviceId").toString();
        entry.displayName = m_settings.value("displayName").toString();
        entry.identityRefreshToken = m_settings.value("identityRefreshToken").toString();
        entry.identityProviderUrl = m_settings.value("identityProviderUrl").toString();
        servers.append(entry);
    }
    m_settings.endArray();
    return servers;
}

void Settings::addServer(const ServerEntry& entry)
{
    auto servers = savedServers();
    servers.append(entry);

    m_settings.beginWriteArray("servers", servers.size());
    for (int i = 0; i < servers.size(); ++i) {
        m_settings.setArrayIndex(i);
        m_settings.setValue("url", servers[i].url);
        m_settings.setValue("userId", servers[i].userId);
        m_settings.setValue("accessToken", servers[i].accessToken);
        m_settings.setValue("deviceId", servers[i].deviceId);
        m_settings.setValue("displayName", servers[i].displayName);
        m_settings.setValue("identityRefreshToken", servers[i].identityRefreshToken);
        m_settings.setValue("identityProviderUrl", servers[i].identityProviderUrl);
    }
    m_settings.endArray();
}

void Settings::removeServer(int index)
{
    auto servers = savedServers();
    if (index < 0 || index >= servers.size()) return;
    servers.removeAt(index);

    m_settings.beginWriteArray("servers", servers.size());
    for (int i = 0; i < servers.size(); ++i) {
        m_settings.setArrayIndex(i);
        m_settings.setValue("url", servers[i].url);
        m_settings.setValue("userId", servers[i].userId);
        m_settings.setValue("accessToken", servers[i].accessToken);
        m_settings.setValue("deviceId", servers[i].deviceId);
        m_settings.setValue("displayName", servers[i].displayName);
        m_settings.setValue("identityRefreshToken", servers[i].identityRefreshToken);
        m_settings.setValue("identityProviderUrl", servers[i].identityProviderUrl);
    }
    m_settings.endArray();
}

void Settings::updateServer(int index, const ServerEntry& entry)
{
    auto servers = savedServers();
    if (index < 0 || index >= servers.size()) return;
    servers[index] = entry;

    m_settings.beginWriteArray("servers", servers.size());
    for (int i = 0; i < servers.size(); ++i) {
        m_settings.setArrayIndex(i);
        m_settings.setValue("url", servers[i].url);
        m_settings.setValue("userId", servers[i].userId);
        m_settings.setValue("accessToken", servers[i].accessToken);
        m_settings.setValue("deviceId", servers[i].deviceId);
        m_settings.setValue("displayName", servers[i].displayName);
        m_settings.setValue("identityRefreshToken", servers[i].identityRefreshToken);
        m_settings.setValue("identityProviderUrl", servers[i].identityProviderUrl);
    }
    m_settings.endArray();
}

int Settings::activeServerIndex() const
{
    return m_settings.value("activeServerIndex", -1).toInt();
}

void Settings::setActiveServerIndex(int index)
{
    m_settings.setValue("activeServerIndex", index);
}

int Settings::fontSize() const
{
    return m_settings.value("fontSize", 14).toInt();
}

void Settings::setFontSize(int size)
{
    if (fontSize() != size) {
        m_settings.setValue("fontSize", size);
        emit fontSizeChanged();
    }
}

QString Settings::theme() const
{
    return m_settings.value("theme", "dark").toString();
}

void Settings::setTheme(const QString& theme)
{
    if (this->theme() != theme) {
        m_settings.setValue("theme", theme);
        emit themeChanged();
    }
}

QString Settings::accent() const
{
    return m_settings.value("accent", "#5865f2").toString();
}

void Settings::setAccent(const QString& accent)
{
    if (this->accent() != accent) {
        m_settings.setValue("accent", accent);
        emit accentChanged();
    }
}

int Settings::accentHue() const
{
    return m_settings.value("accentHue", 180).toInt();
}

void Settings::setAccentHue(int hue)
{
    // Designer palette supports only these four hues; anything else would
    // fall through to the 180 default in Theme.qml and look unthemed.
    if (hue != 180 && hue != 260 && hue != 320 && hue != 30) hue = 180;
    if (accentHue() == hue) return;
    m_settings.setValue("accentHue", hue);
    emit accentHueChanged();
}

bool Settings::accessibilityMode() const
{
    return m_settings.value("accessibilityMode", false).toBool();
}

void Settings::setAccessibilityMode(bool v)
{
    if (accessibilityMode() != v) {
        m_settings.setValue("accessibilityMode", v);
        emit accessibilityModeChanged();
    }
}

QString Settings::layoutVariant() const
{
    return m_settings.value("layoutVariant", "standard").toString();
}

void Settings::setLayoutVariant(const QString& variant)
{
    // Only three valid values — anything else gets coerced to standard so
    // a typo in saved state can't put Theme.variant into an unknown mode
    // (Theme's layout switcher falls through to _layoutStandard anyway,
    // but coercing here keeps the persisted value clean).
    QString v = variant;
    if (v != "standard" && v != "compact" && v != "focus") v = "standard";
    if (layoutVariant() == v) return;
    m_settings.setValue("layoutVariant", v);
    emit layoutVariantChanged();
}

QString Settings::audioInputDevice() const {
    return m_settings.value("audio/inputDevice").toString();
}
void Settings::setAudioInputDevice(const QString& desc) {
    if (audioInputDevice() != desc) {
        m_settings.setValue("audio/inputDevice", desc);
        emit audioInputDeviceChanged();
    }
}
QString Settings::audioOutputDevice() const {
    return m_settings.value("audio/outputDevice").toString();
}
void Settings::setAudioOutputDevice(const QString& desc) {
    if (audioOutputDevice() != desc) {
        m_settings.setValue("audio/outputDevice", desc);
        emit audioOutputDeviceChanged();
    }
}
int Settings::inputVolume() const {
    return m_settings.value("audio/inputVolume", 100).toInt();
}
void Settings::setInputVolume(int v) {
    v = qBound(0, v, 100);
    if (inputVolume() != v) {
        m_settings.setValue("audio/inputVolume", v);
        emit inputVolumeChanged();
    }
}
int Settings::outputVolume() const {
    return m_settings.value("audio/outputVolume", 100).toInt();
}
void Settings::setOutputVolume(int v) {
    v = qBound(0, v, 100);
    if (outputVolume() != v) {
        m_settings.setValue("audio/outputVolume", v);
        emit outputVolumeChanged();
    }
}
bool Settings::notificationsEnabled() const {
    return m_settings.value("notifications/enabled", true).toBool();
}
void Settings::setNotificationsEnabled(bool v) {
    if (notificationsEnabled() != v) {
        m_settings.setValue("notifications/enabled", v);
        emit notificationsEnabledChanged();
    }
}
bool Settings::notificationSound() const {
    return m_settings.value("notifications/sound", true).toBool();
}
void Settings::setNotificationSound(bool v) {
    if (notificationSound() != v) {
        m_settings.setValue("notifications/sound", v);
        emit notificationSoundChanged();
    }
}

bool Settings::showMemberList() const {
    return m_settings.value("ui/showMemberList", true).toBool();
}
void Settings::setShowMemberList(bool v) {
    if (showMemberList() != v) {
        m_settings.setValue("ui/showMemberList", v);
        emit showMemberListChanged();
    }
}

int Settings::windowX() const {
    return m_settings.value("window/x", -1).toInt();
}
void Settings::setWindowX(int v) {
    if (windowX() != v) {
        m_settings.setValue("window/x", v);
        emit windowXChanged();
    }
}
int Settings::windowY() const {
    return m_settings.value("window/y", -1).toInt();
}
void Settings::setWindowY(int v) {
    if (windowY() != v) {
        m_settings.setValue("window/y", v);
        emit windowYChanged();
    }
}
int Settings::windowWidth() const {
    return m_settings.value("window/width", -1).toInt();
}
void Settings::setWindowWidth(int v) {
    if (windowWidth() != v) {
        m_settings.setValue("window/width", v);
        emit windowWidthChanged();
    }
}
int Settings::windowHeight() const {
    return m_settings.value("window/height", -1).toInt();
}
void Settings::setWindowHeight(int v) {
    if (windowHeight() != v) {
        m_settings.setValue("window/height", v);
        emit windowHeightChanged();
    }
}
// Matches Qt's QWindow::Visibility enum: 2 = Windowed, 4 = Maximized,
// 5 = FullScreen. Default -1 means "apply platform default".
int Settings::windowVisibility() const {
    return m_settings.value("window/visibility", -1).toInt();
}
void Settings::setWindowVisibility(int v) {
    if (windowVisibility() != v) {
        m_settings.setValue("window/visibility", v);
        emit windowVisibilityChanged();
    }
}

namespace {
QVariantList devicesToList(const QList<QAudioDevice>& devices) {
    QVariantList out;
    QVariantMap def;
    def["description"] = QStringLiteral("System default");
    def["id"] = QString();
    out.append(def);
    for (const auto& d : devices) {
        QVariantMap m;
        m["description"] = d.description();
        m["id"] = QString::fromLatin1(d.id());
        out.append(m);
    }
    return out;
}
} // namespace

QVariantList Settings::audioInputDevices() const {
    return devicesToList(QMediaDevices::audioInputs());
}
QVariantList Settings::audioOutputDevices() const {
    return devicesToList(QMediaDevices::audioOutputs());
}

QStringList Settings::collapsedCategories() const
{
    return m_settings.value("collapsedCategories").toStringList();
}

void Settings::setCollapsedCategories(const QStringList& categories)
{
    m_settings.setValue("collapsedCategories", categories);
}

qint64 Settings::lastReadTs(const QString& roomId) const
{
    if (roomId.isEmpty()) return 0;
    return m_settings.value(QStringLiteral("unread/") + roomId, 0).toLongLong();
}

void Settings::setLastReadTs(const QString& roomId, qint64 tsMs)
{
    if (roomId.isEmpty()) return;
    m_settings.setValue(QStringLiteral("unread/") + roomId, tsMs);
}

bool Settings::isRoomMuted(const QString& roomId) const
{
    if (roomId.isEmpty()) return false;
    return m_settings.value(QStringLiteral("mutedRooms"))
        .toStringList().contains(roomId);
}

void Settings::setRoomMuted(const QString& roomId, bool muted)
{
    if (roomId.isEmpty()) return;
    auto list = m_settings.value(QStringLiteral("mutedRooms")).toStringList();
    const bool has = list.contains(roomId);
    if (muted && !has) list.append(roomId);
    else if (!muted && has) list.removeAll(roomId);
    else return;
    m_settings.setValue(QStringLiteral("mutedRooms"), list);
    emit mutedRoomsChanged();
}

QString Settings::roomNotificationMode(const QString& roomId) const
{
    if (roomId.isEmpty()) return QStringLiteral("all");
    // Hashed key keeps the QSettings group name safe for
    // room-ids containing `!:/` etc. that QSettings would treat
    // as sub-groups.
    QString key = QStringLiteral("notifMode/")
        + QString::fromLatin1(
            QCryptographicHash::hash(roomId.toUtf8(),
                                     QCryptographicHash::Sha1).toHex());
    QString v = m_settings.value(key).toString();
    if (v.isEmpty()) return QStringLiteral("all");
    if (v == QStringLiteral("all")
        || v == QStringLiteral("mentions")
        || v == QStringLiteral("none")) return v;
    return QStringLiteral("all");
}

void Settings::setRoomNotificationMode(const QString& roomId,
                                       const QString& mode)
{
    if (roomId.isEmpty()) return;
    QString normalized = mode;
    if (normalized != QStringLiteral("all")
        && normalized != QStringLiteral("mentions")
        && normalized != QStringLiteral("none")) {
        normalized = QStringLiteral("all");
    }
    QString key = QStringLiteral("notifMode/")
        + QString::fromLatin1(
            QCryptographicHash::hash(roomId.toUtf8(),
                                     QCryptographicHash::Sha1).toHex());
    if (normalized == QStringLiteral("all")) {
        m_settings.remove(key);
    } else {
        m_settings.setValue(key, normalized);
    }
}

QString Settings::lastTextRoomFor(const QString& serverUrl) const
{
    if (serverUrl.isEmpty()) return {};
    // Key by URL hash so stray punctuation in the URL doesn't trip
    // up QSettings' group parser. The hash is stable per URL.
    QString key = QStringLiteral("lastTextRoom/")
        + QString::fromLatin1(
            QCryptographicHash::hash(serverUrl.toUtf8(),
                                     QCryptographicHash::Sha1).toHex());
    return m_settings.value(key).toString();
}

void Settings::setLastTextRoomFor(const QString& serverUrl,
                                  const QString& roomId)
{
    if (serverUrl.isEmpty() || roomId.isEmpty()) return;
    QString key = QStringLiteral("lastTextRoom/")
        + QString::fromLatin1(
            QCryptographicHash::hash(serverUrl.toUtf8(),
                                     QCryptographicHash::Sha1).toHex());
    m_settings.setValue(key, roomId);
}

int Settings::screenShareQuality() const
{
    // Clamp stored value to the valid 0..3 range in case an older
    // version wrote something out-of-band.
    int v = m_settings.value(QStringLiteral("screenShareQuality"), 1).toInt();
    return std::clamp(v, 0, 3);
}

void Settings::setScreenShareQuality(int level)
{
    level = std::clamp(level, 0, 3);
    if (level == screenShareQuality()) return;
    m_settings.setValue(QStringLiteral("screenShareQuality"), level);
    emit screenShareQualityChanged();
}

// ── Direct screen-share knobs ─────────────────────────────────────
//
// Defaults are hydrated from the legacy preset (if present) on first
// read so an upgrade from <0.0.24 doesn't snap the user from their
// chosen "Ultra" preset back to a hard-coded "Medium" floor. After
// that initial read we honour whatever's in the explicit fields.
//
// Allowed envelopes are wide:
//   fps         1 .. 60       (1 = ultra-low cellular, 60 = LAN+screen)
//   maxWidth    480 .. 3840   (480p .. 4K long edge)
//   jpegQuality 1 .. 100      (1 = postage-stamp, 100 = near-lossless)
// ScreenShareController.applyEffectiveQuality() additionally clamps
// the chosen value against the active server's advertised policy
// (`maxScreenShare*`) before configuring the encoder.
namespace {
struct LegacyPreset { int fps; int maxWidth; int jpeg; };
LegacyPreset legacyPreset(int level) {
    switch (std::clamp(level, 0, 3)) {
    case 0:  return {2,  960, 40};
    case 1:  return {5, 1280, 60};
    case 2:  return {10, 1600, 75};
    case 3:  default: return {15, 1920, 85};
    }
}
}  // namespace

int Settings::screenShareFps() const
{
    if (m_settings.contains(QStringLiteral("screenShare/fps")))
        return std::clamp(m_settings.value("screenShare/fps").toInt(), 1, 60);
    return legacyPreset(screenShareQuality()).fps;
}

void Settings::setScreenShareFps(int fps)
{
    fps = std::clamp(fps, 1, 60);
    if (fps == screenShareFps()) return;
    m_settings.setValue(QStringLiteral("screenShare/fps"), fps);
    emit screenShareFpsChanged();
}

int Settings::screenShareMaxWidth() const
{
    if (m_settings.contains(QStringLiteral("screenShare/maxWidth")))
        return std::clamp(m_settings.value("screenShare/maxWidth").toInt(),
                          480, 3840);
    return legacyPreset(screenShareQuality()).maxWidth;
}

void Settings::setScreenShareMaxWidth(int px)
{
    px = std::clamp(px, 480, 3840);
    if (px == screenShareMaxWidth()) return;
    m_settings.setValue(QStringLiteral("screenShare/maxWidth"), px);
    emit screenShareMaxWidthChanged();
}

int Settings::screenShareJpegQuality() const
{
    if (m_settings.contains(QStringLiteral("screenShare/jpegQuality")))
        return std::clamp(m_settings.value("screenShare/jpegQuality").toInt(),
                          1, 100);
    return legacyPreset(screenShareQuality()).jpeg;
}

void Settings::setScreenShareJpegQuality(int q)
{
    q = std::clamp(q, 1, 100);
    if (q == screenShareJpegQuality()) return;
    m_settings.setValue(QStringLiteral("screenShare/jpegQuality"), q);
    emit screenShareJpegQualityChanged();
}

QString Settings::voiceMode() const
{
    return m_settings.value("voiceMode", "open").toString();
}
void Settings::setVoiceMode(const QString& v)
{
    if (v == voiceMode()) return;
    m_settings.setValue("voiceMode", v);
    emit voiceModeChanged();
}

QString Settings::pttKeySequence() const
{
    return m_settings.value("pttKeySequence", "Ctrl+Space").toString();
}
void Settings::setPttKeySequence(const QString& seq)
{
    if (seq == pttKeySequence()) return;
    m_settings.setValue("pttKeySequence", seq);
    emit pttKeySequenceChanged();
}
