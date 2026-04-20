#include "core/App.h"
#include "core/NotificationManager.h"
#include "net/ServerManager.h"

App::App(QObject* parent)
    : QObject(parent)
    , m_settings(new Settings(this))
    , m_serverManager(new ServerManager(m_settings, this))
    // NotificationManager must be constructed AFTER ServerManager — it
    // wires into every already-restored connection in its constructor.
    , m_notificationManager(new NotificationManager(m_serverManager, m_settings, this))
{
}

App::~App() = default;
