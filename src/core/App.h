#pragma once

#include <QObject>
#include "core/Settings.h"

class ServerManager;
class NotificationManager;

class App : public QObject {
    Q_OBJECT

public:
    explicit App(QObject* parent = nullptr);
    ~App() override;

    ServerManager* serverManager() const { return m_serverManager; }
    Settings* settings() const { return m_settings; }
    NotificationManager* notificationManager() const { return m_notificationManager; }

private:
    Settings* m_settings;
    ServerManager* m_serverManager;
    NotificationManager* m_notificationManager;
};
