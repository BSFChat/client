import QtQuick
import BSFChat

// Icon — tintable SVG line-icon helper.
//
// Routes through the `tinted` QQuickImageProvider (registered in
// main.cpp) which reads the qrc SVG, substitutes `currentColor` with
// the caller-supplied hex, and rasterises with QSvgRenderer at the
// requested size. Cached per-pixmap so the same icon in the same
// colour doesn't re-render on every list delegate.
//
// Avoids every QML tinting path (MultiEffect, ColorOverlay,
// data-URI Image source) — all of which silently fail on the
// Qt 6.5 Android arm64 GL backend.
//
// Usage:  Icon { name: "mic"; size: 20; color: Theme.fg1 }
Item {
    id: root
    property string name: ""
    property int size: 20
    property color color: Theme.fg1

    implicitWidth: size
    implicitHeight: size

    // Convert the QML `color` to a "rrggbb" string for the URL. Strip
    // the leading '#' and any alpha so the cache key is stable.
    readonly property string _hex: {
        var s = root.color.toString();
        if (s.length === 9) s = "#" + s.substr(3);
        return s.charAt(0) === '#' ? s.substr(1) : s;
    }

    Image {
        id: src
        anchors.fill: parent
        // Request the icon at exactly our display size — no resampling
        // step in QML. Avoids Qt's internal Image-scaling path which
        // seems to drop the texture on the Qt 6.5 Android scene graph.
        sourceSize.width: root.size
        sourceSize.height: root.size
        source: root.name.length > 0
            ? "image://tinted/" + root.name + "/" + root._hex
            : ""
    }
}
