#include "store/LocalCache.h"

LocalCache::LocalCache(QObject* parent)
    : QObject(parent)
{
}

LocalCache::~LocalCache()
{
    close();
}

bool LocalCache::open(const QString& /*userId*/)
{
    // Stub: will use QSqlDatabase with SQLite in the future
    m_open = true;
    return true;
}

void LocalCache::close()
{
    m_open = false;
    m_syncToken.clear();
}

void LocalCache::cacheSyncToken(const QString& token)
{
    m_syncToken = token;
}

QString LocalCache::syncToken() const
{
    return m_syncToken;
}
