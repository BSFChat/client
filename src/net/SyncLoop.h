#pragma once

#include <QObject>
#include <QTimer>
#include <bsfchat/MatrixTypes.h>

class MatrixClient;

class SyncLoop : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)

public:
    explicit SyncLoop(MatrixClient* client, QObject* parent = nullptr);

    void start();
    void stop();
    bool isRunning() const { return m_running; }

    void setSince(const QString& since) { m_since = since; }

signals:
    void syncCompleted(const bsfchat::SyncResponse& response);
    void syncError(const QString& error);
    void runningChanged();

private:
    void doSync();
    void onSyncSuccess(const bsfchat::SyncResponse& response);
    void onSyncError(const QString& error);

    MatrixClient* m_client;
    QTimer m_retryTimer;
    QString m_since;
    bool m_running = false;
};
