#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariantList>

class Settings : public QObject {
    Q_OBJECT
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)

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

    // Category collapse state
    QStringList collapsedCategories() const;
    void setCollapsedCategories(const QStringList& categories);

signals:
    void fontSizeChanged();
    void themeChanged();

private:
    mutable QSettings m_settings;
};
