#pragma once

#include <QQuickImageProvider>
#include <QIcon>
#include <QPixmap>

// QQuickImageProvider that returns tinted SVG icons.
//
// Qt's QML Image can't decode SVG data: URIs on the Android arm64 GL
// backend (Image.source accepts the URL but the status never
// transitions past Null). QtQuick.Effects' MultiEffect and
// Qt5Compat.GraphicalEffects' ColorOverlay both also silently fail
// on the same backend. The only robust cross-platform path is to
// render the SVG ourselves with explicit stroke colour and hand Qt
// the rasterised pixmap.
//
// URL scheme: `image://tinted/<name>/<colorHex>`
//   e.g. image://tinted/hash/c1c4c9
// Output: SVG rendered at requestedSize, strokes painted in the
// requested hex colour. Cached by (name, hex, size).
class TintedIconProvider : public QQuickImageProvider {
public:
    TintedIconProvider();

    QImage requestImage(const QString& id,
                        QSize* size,
                        const QSize& requestedSize) override;
};
