#include "core/TintedIconProvider.h"

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QSvgRenderer>
#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QDebug>

namespace {
// Simple per-process cache keyed by "<name>|<hex>|<w>x<h>". SVG
// rasterisation isn't cheap and the same icons get requested over
// and over as list delegates recycle.
QHash<QString, QImage> s_cache;
QMutex s_cacheMutex;

QByteArray tintSvg(const QString& name, const QString& hex)
{
    QFile f(QStringLiteral(":/qt/qml/BSFChat/qml/icons/") + name + QStringLiteral(".svg"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray body = f.readAll();
    body.replace(QByteArrayLiteral("currentColor"), hex.toUtf8());
    return body;
}
} // namespace

TintedIconProvider::TintedIconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage TintedIconProvider::requestImage(const QString& id,
                                         QSize* size,
                                         const QSize& requestedSize)
{
    // id format: "<name>/<hex>" (hex without leading '#').
    int slash = id.indexOf('/');
    if (slash <= 0) return {};
    QString name = id.left(slash);
    QString hex  = id.mid(slash + 1);
    if (!hex.startsWith('#')) hex = "#" + hex;

    QSize wanted = requestedSize.isValid() && requestedSize.width() > 0
        ? requestedSize
        : QSize(48, 48);

    QString key = name + "|" + hex + "|"
        + QString::number(wanted.width()) + "x"
        + QString::number(wanted.height());
    {
        QMutexLocker lock(&s_cacheMutex);
        auto it = s_cache.find(key);
        if (it != s_cache.end()) {
            if (size) *size = it->size();
            return *it;
        }
    }

    QByteArray svg = tintSvg(name, hex);
    if (svg.isEmpty()) return {};
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return {};

    // QImage with explicit ARGB32_Premultiplied so Qt's scene graph
    // doesn't silently reject the surface format (which is what was
    // happening with the QPixmap-based provider — pixmap was valid
    // on CPU but rendered transparent on the Android GL backend).
    QImage out(wanted, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    renderer.render(&p, QRectF(0, 0, wanted.width(), wanted.height()));
    p.end();

    if (size) *size = wanted;
    QMutexLocker lock(&s_cacheMutex);
    s_cache.insert(key, out);
    return out;
}
