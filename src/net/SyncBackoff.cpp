#include "net/SyncBackoff.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>

namespace SyncBackoff {

int baseDelayMs(int consecutiveFailures)
{
    if (consecutiveFailures <= 0) return kBaseDelayMs;
    // Clamp the shift well below the width of qint64 so a runaway failure
    // counter can't turn into undefined behaviour or a negative delay.
    const int shift = qMin(consecutiveFailures, 20);
    const qint64 delay = static_cast<qint64>(kBaseDelayMs) << shift;
    return static_cast<int>(qMin<qint64>(delay, kMaxDelayMs));
}

int applyJitter(int base, double jitter01)
{
    if (base <= 0) return 0;
    jitter01 = qBound(0.0, jitter01, 1.0);
    const int half = base / 2;
    return half + static_cast<int>(jitter01 * (base - half));
}

int delayForFailure(int consecutiveFailures, double jitter01)
{
    return applyJitter(baseDelayMs(consecutiveFailures), jitter01);
}

bool indicatesRejectedSinceToken(const QString& errorBody)
{
    // MatrixClient::syncError forwards the raw response body, which for a
    // Matrix error is {"errcode": "...", "error": "..."}. Anything else
    // (a transport failure, an HTML error page from a proxy) is not the
    // server rejecting our token and must not cost us the sync position.
    const QJsonDocument doc = QJsonDocument::fromJson(errorBody.toUtf8());
    if (!doc.isObject()) return false;
    const QString code = doc.object().value(QStringLiteral("errcode")).toString();
    return code == QStringLiteral("M_UNKNOWN")
        || code == QStringLiteral("M_INVALID_PARAM")
        || code == QStringLiteral("M_BAD_JSON");
}

} // namespace SyncBackoff
