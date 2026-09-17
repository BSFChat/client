#include "net/AuthError.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AuthError {

bool indicatesDeadAccessToken(const QString& errorBody)
{
    const QJsonDocument doc = QJsonDocument::fromJson(errorBody.toUtf8());
    if (!doc.isObject()) return false;
    const QString code = doc.object().value(QStringLiteral("errcode")).toString();
    return code == QStringLiteral("M_UNKNOWN_TOKEN")
        || code == QStringLiteral("M_MISSING_TOKEN");
}

} // namespace AuthError
