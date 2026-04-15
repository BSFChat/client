#include "net/SyncLoop.h"
#include "net/MatrixClient.h"

SyncLoop::SyncLoop(MatrixClient* client, QObject* parent)
    : QObject(parent)
    , m_client(client)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &SyncLoop::doSync);

    connect(m_client, &MatrixClient::syncSuccess, this, &SyncLoop::onSyncSuccess);
    connect(m_client, &MatrixClient::syncError, this, &SyncLoop::onSyncError);
}

void SyncLoop::start()
{
    if (m_running) return;
    m_running = true;
    emit runningChanged();
    doSync();
}

void SyncLoop::stop()
{
    if (!m_running) return;
    m_running = false;
    m_retryTimer.stop();
    emit runningChanged();
}

void SyncLoop::doSync()
{
    if (!m_running) return;
    m_client->sync(m_since, 30000);
}

void SyncLoop::onSyncSuccess(const bsfchat::SyncResponse& response)
{
    if (!m_running) return;
    m_since = QString::fromStdString(response.next_batch);
    emit syncCompleted(response);
    // Immediately start next sync
    doSync();
}

void SyncLoop::onSyncError(const QString& error)
{
    if (!m_running) return;
    emit syncError(error);
    // Retry after 5 seconds
    m_retryTimer.start(5000);
}
