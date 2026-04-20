import QtQuick
import QtQuick.Effects
import BSFChat

// Icon — tintable SVG line-icon helper.
//
// The kit ships 24×24 line icons that use `stroke="currentColor"`. Qt's
// SVG renderer treats currentColor as black, so we need to post-tint.
//
// The canonical Qt 6 pattern is `layer.enabled: true` on the source Item
// with `layer.effect` attached — that funnels the source through an
// implicit FBO texture before the effect runs, which gives clean alpha
// and a reliable colorization result. Avoiding the MultiEffect-with-
// hidden-source pattern (which in some Qt builds produced the dim /
// black-tinged rendering we had before).
//
// Usage:  Icon { name: "mic"; size: 20; color: Theme.fg1 }
Item {
    id: root
    property string name: ""
    property int size: 20
    property color color: Theme.fg1

    implicitWidth: size
    implicitHeight: size

    Image {
        id: src
        anchors.fill: parent
        // Render at 2× so tinting stays sharp on HiDPI displays.
        sourceSize.width: root.size * 2
        sourceSize.height: root.size * 2
        source: root.name.length > 0
                ? "qrc:/qt/qml/BSFChat/qml/icons/" + root.name + ".svg"
                : ""
        smooth: true
        mipmap: true
        fillMode: Image.PreserveAspectFit
        visible: true

        // Layer into an FBO so MultiEffect has a clean source texture
        // (keeps alpha pristine instead of blending into the underlying
        // default black render of the SVG).
        layer.enabled: true
        layer.effect: MultiEffect {
            // brightness: 1.0 pushes source toward white, then colorization
            // replaces the RGB with `root.color`, keeping the source alpha
            // for stroke pixels. Combined, the icon comes out at EXACTLY
            // root.color at the stroke pixels, transparent elsewhere.
            brightness: 1.0
            colorization: 1.0
            colorizationColor: root.color
        }
    }
}
