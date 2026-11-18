#include "core/App.h"
#include "core/NotificationManager.h"
#include "core/PushManager.h"
#include "net/ServerManager.h"

App::App(QObject* parent)
    : QObject(parent)
    , m_settings(new Settings(this))
    , m_serverManager(new ServerManager(m_settings, this))
    // NotificationManager must be constructed AFTER ServerManager — it
    // wires into every already-restored connection in its constructor.
    , m_notificationManager(new NotificationManager(m_serverManager, m_settings, this))
    // Mobile push (docs/push-notifications.md); inert where not built in.
    , m_pushManager(new PushManager(m_serverManager, m_settings, this))
{
    // Android: the persistent background connection runs when the user
    // chose it, or when push cannot work here.
    m_notificationManager->setBackgroundSyncPolicy(
        [pm = m_pushManager]() { return pm->backgroundSyncWanted(); });
    connect(m_pushManager, &PushManager::backgroundSyncWantedChanged,
            m_notificationManager, &NotificationManager::refreshBackgroundSync);
}

App::~App() = default;
