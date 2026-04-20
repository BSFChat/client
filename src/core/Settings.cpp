#include "core/Settings.h"

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
