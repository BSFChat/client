#pragma once

#include <QObject>
#include <QString>

class LocalCache : public QObject {
    Q_OBJECT

public:
    explicit LocalCache(QObject* parent = nullptr);
    ~LocalCache() override;

    bool open(const QString& userId);
    void close();
    bool isOpen() const { return m_open; }

    // Stub interface for future SQLite caching
    void cacheSyncToken(const QString& token);
    QString syncToken() const;

private:
    bool m_open = false;
    QString m_syncToken;
};
