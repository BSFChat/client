#pragma once

#include <QObject>
#include "core/Settings.h"

class ServerManager;

class App : public QObject {
    Q_OBJECT

public:
    explicit App(QObject* parent = nullptr);
    ~App() override;

    ServerManager* serverManager() const { return m_serverManager; }
    Settings* settings() const { return m_settings; }

private:
    Settings* m_settings;
    ServerManager* m_serverManager;
};
