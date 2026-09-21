#include "core/Settings.h"
#include "core/AppProfile.h"
#include "core/AudioDeviceStatus.h"
#include "core/ReadState.h"
#include "core/ReleaseSelection.h"
#include "util/FileLogger.h"
#include "voice/IpPrivacy.h"
#include "voice/video/VideoCodecPreference.h"

#include <QLoggingCategory>

#include <algorithm>
#include <QCryptographicHash>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QVariantMap>

namespace {
// Enables the bsfchat.* info/debug categories (they default to
// warnings-only so ordinary users' logs stay lean). Code-set rules
// stack on top of any qtlogging.ini, so this works everywhere.
void applyVerboseVoiceLogging(bool on)
{
    QLoggingCategory::setFilterRules(
        on ? QStringLiteral("bsfchat.*=true") : QString());
}
} // namespace

Settings::Settings(QObject* parent)
    : QObject(parent)
    // ("BSFChat", "BSFChat") unless --profile/$BSFCHAT_PROFILE is set, in
    // which case the application name gains a "-<profile>" suffix so two
    // clients on one machine don't share one settings file. Default
    // profile keeps the historical domain byte for byte.
    , m_settings(bsfchat::organizationName(), bsfchat::applicationName())
{
    if (verboseVoiceLogging())
        applyVerboseVoiceLogging(true);

    // Live device lists. Re-enumerating only on dialog open was one open
    // too late for the case that prompted this: a Bluetooth headset that
    // connects while the dialog is already open never appeared at all.
    m_mediaDevices = new QMediaDevices(this);
    connect(m_mediaDevices, &QMediaDevices::audioInputsChanged,
            this, &Settings::audioDevicesChanged);
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, &Settings::audioDevicesChanged);
    // On macOS these also fire when the system DEFAULT moves, so the
    // "System default (…)" entry relabels itself as the user switches
    // output in Control Centre.
    connect(&bsfchat::AudioDeviceStatus::instance(),
            &bsfchat::AudioDeviceStatus::changed,
            this, &Settings::audioInUseChanged);
}

bool Settings::verboseVoiceLogging() const
{
    return m_settings.value("advanced/verboseVoiceLogging", false).toBool();
}

void Settings::setVerboseVoiceLogging(bool v)
{
    if (verboseVoiceLogging() == v) return;
    m_settings.setValue("advanced/verboseVoiceLogging", v);
    applyVerboseVoiceLogging(v);
    emit verboseVoiceLoggingChanged();
}

bool Settings::showVideoDiagnostics() const
{
    return m_settings.value("advanced/showVideoDiagnostics", false).toBool();
}

void Settings::setShowVideoDiagnostics(bool v)
{
    if (showVideoDiagnostics() == v) return;
    m_settings.setValue("advanced/showVideoDiagnostics", v);
    emit showVideoDiagnosticsChanged();
}

QString Settings::logDirectory() const
{
    return bsfchat::logDirectory();
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

QVariantMap Settings::popoutGeometry(const QString& kind) const {
    // Anything that is not the screen kind is a camera: the caller is
    // qml/js/VideoWindows.js geometryKey(), but a settings file edited by
    // hand must not be able to conjure a third bucket.
    const QString k = (kind == QLatin1String("screen"))
        ? QStringLiteral("screen") : QStringLiteral("camera");
    const QString base = QStringLiteral("popout/") + k + QLatin1Char('/');
    QVariantMap out;
    out.insert(QStringLiteral("x"),
               m_settings.value(base + QStringLiteral("x"), -1).toInt());
    out.insert(QStringLiteral("y"),
               m_settings.value(base + QStringLiteral("y"), -1).toInt());
    out.insert(QStringLiteral("width"),
               m_settings.value(base + QStringLiteral("width"), -1).toInt());
    out.insert(QStringLiteral("height"),
               m_settings.value(base + QStringLiteral("height"), -1).toInt());
    return out;
}

void Settings::setPopoutGeometry(const QString& kind,
                                 int x, int y, int width, int height) {
    const QString k = (kind == QLatin1String("screen"))
        ? QStringLiteral("screen") : QStringLiteral("camera");
    const QString base = QStringLiteral("popout/") + k + QLatin1Char('/');
    // A window that is being destroyed reports 0x0 on some platforms, and
    // storing that would reopen the next pop-out at the minimum size for
    // no reason. Refuse anything below the floor VideoWindows.js clamps
    // to instead of writing it and clamping on the way back out.
    if (width < 320 || height < 180) return;
    m_settings.setValue(base + QStringLiteral("x"), x);
    m_settings.setValue(base + QStringLiteral("y"), y);
    m_settings.setValue(base + QStringLiteral("width"), width);
    m_settings.setValue(base + QStringLiteral("height"), height);
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
QVariantList devicesToList(const QList<QAudioDevice>& devices,
                           const QAudioDevice& defaultDevice) {
    QVariantList out;
    // First entry: "follow the system default", stored as an empty
    // string. Naming the device it currently resolves to is the whole
    // difference between a row that tells the user nothing and one that
    // answers "so which device IS it using?" on sight.
    //
    // `systemDefault` rather than matching the label text: the label now
    // contains a device name, so QML can no longer recognise this entry
    // by comparing it against a literal.
    QVariantMap def;
    def["description"] = defaultDevice.isNull()
        ? QStringLiteral("System default")
        : QStringLiteral("System default (%1)").arg(defaultDevice.description());
    def["id"] = QString();
    def["systemDefault"] = true;
    out.append(def);
    for (const auto& d : devices) {
        QVariantMap m;
        m["description"] = d.description();
        m["id"] = QString::fromLatin1(d.id());
        m["systemDefault"] = false;
        out.append(m);
    }
    return out;
}
} // namespace

QVariantList Settings::audioInputDevices() const {
    return devicesToList(QMediaDevices::audioInputs(),
                         QMediaDevices::defaultAudioInput());
}
QVariantList Settings::audioOutputDevices() const {
    return devicesToList(QMediaDevices::audioOutputs(),
                         QMediaDevices::defaultAudioOutput());
}

QString Settings::audioInputInUse() const {
    return bsfchat::AudioDeviceStatus::instance().inputInUse();
}
QString Settings::audioOutputInUse() const {
    return bsfchat::AudioDeviceStatus::instance().outputInUse();
}

void Settings::selectAudioInputDevice(const QString& description,
                                      const QString& id) {
    // Hint first, then the authoritative key — so a reader that wakes on
    // audioInputDeviceChanged never sees the new description paired with
    // the previous device's id.
    m_settings.setValue("audio/inputDeviceId", id);
    setAudioInputDevice(description);
}

void Settings::selectAudioOutputDevice(const QString& description,
                                       const QString& id) {
    m_settings.setValue("audio/outputDeviceId", id);
    setAudioOutputDevice(description);
}

void Settings::refreshAudioDevices()
{
    emit audioDevicesChanged();
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
    const QString key = QStringLiteral("unread/") + roomId;
    // Only notify on a real move. Writers call this on every room switch,
    // frequently with the value already stored, and a signal per no-op write
    // would reintroduce exactly the churn the 800 ms poll was replaced to fix.
    if (m_settings.value(key, 0).toLongLong() == tsMs) return;
    m_settings.setValue(key, tsMs);
    emit lastReadTsChanged(roomId);
}

bool Settings::seedLastReadTs(const QString& roomId, qint64 seedTs)
{
    if (roomId.isEmpty() || seedTs <= 0) return false;
    if (lastReadTs(roomId) > 0) return false;
    setLastReadTs(roomId, seedTs);
    return true;
}

bool Settings::isRoomUnread(const QString& roomId, qint64 lastMessageTs) const
{
    return bsfchat::client::isUnread(lastMessageTs, lastReadTs(roomId));
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
constexpr int kDefaultScreenShareFps = 30;
}  // namespace

int Settings::screenShareFps() const
{
    if (m_settings.contains(QStringLiteral("screenShare/fps")))
        return std::clamp(m_settings.value("screenShare/fps").toInt(), 1, 60);
    // No explicit value = the slider has never been moved (only
    // setScreenShareFps writes this key). This used to fall back to the
    // legacy preset, whose "Medium" default is 5 fps — a JPEG-era number
    // (2026-09-21). Every fresh install therefore shared at 5 fps, and at
    // 5 fps the old RTP pacer released so little per frame that most
    // frames never arrived. The presets' fps column (2/5/10/15) was sized
    // for a JPEG slideshow over a data channel and is never right for
    // H.264/HEVC over RTP, so it is no longer consulted for fps even when
    // a legacy preset is stored; resolution and JPEG quality still
    // hydrate from it. 30 fps: smooth motion, and what the rate
    // controller's floors and the default 4 Mbps target are sized for
    // (the controller trades it down itself when the path or the
    // machine cannot carry it).
    return kDefaultScreenShareFps;
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

// ── RTP-video knobs ───────────────────────────────────────────────
// Envelopes:
//   targetKbps  250 .. 100000  (metered link .. LAN near-lossless)
//   keyframe    1 .. 30 s      (shorter = faster loss recovery,
//                               longer = better compression)
int Settings::screenShareTargetKbps() const
{
    return std::clamp(m_settings.value(
        QStringLiteral("screenShare/targetKbps"), 4000).toInt(), 250, 100000);
}

void Settings::setScreenShareTargetKbps(int kbps)
{
    kbps = std::clamp(kbps, 250, 100000);
    if (kbps == screenShareTargetKbps()) return;
    m_settings.setValue(QStringLiteral("screenShare/targetKbps"), kbps);
    emit screenShareTargetKbpsChanged();
}

int Settings::screenShareKeyframeSec() const
{
    // 10 s background refresh: receivers explicitly request IDRs on
    // loss/late-join, so a short periodic GOP only pulses quality and
    // burns bitrate that P-frames could spend on sharp text.
    return std::clamp(m_settings.value(
        QStringLiteral("screenShare/keyframeSec"), 10).toInt(), 1, 30);
}

void Settings::setScreenShareKeyframeSec(int sec)
{
    sec = std::clamp(sec, 1, 30);
    if (sec == screenShareKeyframeSec()) return;
    m_settings.setValue(QStringLiteral("screenShare/keyframeSec"), sec);
    emit screenShareKeyframeSecChanged();
}

bool Settings::screenShareLossless() const
{
    return m_settings.value(QStringLiteral("screenShare/lossless"), false).toBool();
}

void Settings::setScreenShareLossless(bool on)
{
    if (on == screenShareLossless()) return;
    m_settings.setValue(QStringLiteral("screenShare/lossless"), on);
    emit screenShareLosslessChanged();
}

QString Settings::videoCodecPreference() const
{
    const QString v = m_settings.value(
        QStringLiteral("screenShare/codecPreference"),
        QStringLiteral("auto")).toString();
    // Normalise through the parser so an unknown/legacy value on disk
    // can only ever read back as one of the three, and always as the
    // spelling the UI compares against.
    return videocodec::preferenceToString(videocodec::preferenceFromString(v));
}

void Settings::setVideoCodecPreference(const QString& pref)
{
    const QString v = videocodec::preferenceToString(
        videocodec::preferenceFromString(pref));
    if (v == videoCodecPreference()) return;
    m_settings.setValue(QStringLiteral("screenShare/codecPreference"), v);
    emit videoCodecPreferenceChanged();
}

int Settings::cameraFps() const
{
    return std::clamp(m_settings.value(
        QStringLiteral("camera/fps"), 30).toInt(), 5, 60);
}

void Settings::setCameraFps(int fps)
{
    fps = std::clamp(fps, 5, 60);
    if (fps == cameraFps()) return;
    m_settings.setValue(QStringLiteral("camera/fps"), fps);
    emit cameraFpsChanged();
}

int Settings::cameraMaxWidth() const
{
    return std::clamp(m_settings.value(
        QStringLiteral("camera/maxWidth"), 1280).toInt(), 320, 1920);
}

void Settings::setCameraMaxWidth(int px)
{
    px = std::clamp(px, 320, 1920);
    if (px == cameraMaxWidth()) return;
    m_settings.setValue(QStringLiteral("camera/maxWidth"), px);
    emit cameraMaxWidthChanged();
}

int Settings::cameraTargetKbps() const
{
    return std::clamp(m_settings.value(
        QStringLiteral("camera/targetKbps"), 1500).toInt(), 150, 20000);
}

void Settings::setCameraTargetKbps(int kbps)
{
    kbps = std::clamp(kbps, 150, 20000);
    if (kbps == cameraTargetKbps()) return;
    m_settings.setValue(QStringLiteral("camera/targetKbps"), kbps);
    emit cameraTargetKbpsChanged();
}

// ── Received-video smoothing ──────────────────────────────────────
// A CEILING, not a fixed delay: the playout buffer adapts to measured
// arrival jitter and only holds frames as long as that requires (a few
// ms on a steady stream), up to this. See VideoPlayoutBuffer.h.
//
// Default 150 ms. Measured on the loopback probe (probe_video_cadence),
// with no network at all, the sender's RTP pacer alone delivers every
// keyframe of a 30 fps share ~165 ms late with the frames behind it
// queued up, i.e. a visible freeze-then-clump every keyframe. A 150 ms
// ceiling absorbs that to at most one frame of irregularity per
// keyframe (worst 16-30 ms); 100 ms still leaves a 65 ms freeze and
// 50 ms a 115 ms one; 250 ms absorbs it entirely but then holds ~170 ms
// all the time. At 60 fps the same bursts are ~96 ms, which 100 ms
// already covers. 150 ms is also where interactive
// latency starts to be felt (ITU-T G.114), and screen share — the
// stream people complained about — is watched more than conversed with.
// The camera stream is capped lower still for lip sync
// (VideoReceivePipeline::kCameraPlayoutCapMs).
//
// Envelope 0..400. Zero is the pre-buffer code path, for anyone who
// prefers latency to smoothness. 400 ms is G.114's "unacceptable for
// interactive use" line; jitter beyond it is a network problem that
// showing video half a second late would only disguise, and it is the
// most the buffer's picture cap (24 frames at 60 fps) can hold anyway.
int Settings::videoSmoothingMs() const
{
    return std::clamp(m_settings.value(
        QStringLiteral("video/smoothingMs"), 150).toInt(), 0, 400);
}

void Settings::setVideoSmoothingMs(int ms)
{
    ms = std::clamp(ms, 0, 400);
    if (ms == videoSmoothingMs()) return;
    m_settings.setValue(QStringLiteral("video/smoothingMs"), ms);
    emit videoSmoothingMsChanged();
}

bool Settings::autoUpdateCheck() const
{
    return m_settings.value(QStringLiteral("autoUpdateCheck"), true).toBool();
}

void Settings::setAutoUpdateCheck(bool v)
{
    if (v == autoUpdateCheck()) return;
    m_settings.setValue(QStringLiteral("autoUpdateCheck"), v);
    emit autoUpdateCheckChanged();
}

QString Settings::updateChannel() const
{
    // Normalised through channelFromString so a hand-edited or corrupt
    // value reads back as "stable" rather than as an unknown channel.
    return bsfchat::updates::channelToString(bsfchat::updates::channelFromString(
        m_settings.value(QStringLiteral("updateChannel"),
                         QStringLiteral("stable")).toString()));
}

void Settings::setUpdateChannel(const QString& channel)
{
    const QString norm = bsfchat::updates::channelToString(
        bsfchat::updates::channelFromString(channel));
    if (norm == updateChannel()) return;
    m_settings.setValue(QStringLiteral("updateChannel"), norm);
    // Updater reads this key through its own QSettings handle (it is not
    // wired to this object), so flush before returning — the very next
    // thing the UI does after a toggle is ask the Updater to re-check,
    // and it must not read the pre-toggle value.
    m_settings.sync();
    emit updateChannelChanged();
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

QString Settings::voiceRelayMode() const
{
    // Read through the parser, so whatever is on disk — an older build's value,
    // a typo, an empty string — resolves to one of the two the rest of the
    // client knows about. The alternative is a QML combo box with no matching
    // row and a policy decision that falls through to a default nobody chose.
    return voice::relayModeToString(voice::relayModeFromString(
        m_settings.value(QStringLiteral("voice/relayMode"),
                         QStringLiteral("auto")).toString()));
}

void Settings::setVoiceRelayMode(const QString& mode)
{
    const QString norm = voice::relayModeToString(voice::relayModeFromString(mode));
    if (norm == voiceRelayMode()) return;
    m_settings.setValue(QStringLiteral("voice/relayMode"), norm);
    emit voiceRelayModeChanged();
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
