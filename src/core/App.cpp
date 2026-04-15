#include "core/App.h"
#include "net/ServerManager.h"

App::App(QObject* parent)
    : QObject(parent)
    , m_settings(new Settings(this))
    , m_serverManager(new ServerManager(m_settings, this))
{
}

App::~App() = default;
