pragma Singleton
import QtQuick

QtObject {
    // Background colors (dark to light)
    readonly property color bgDarkest: "#1e1f22"
    readonly property color bgDark: "#2b2d31"
    readonly property color bgMedium: "#313338"
    readonly property color bgLight: "#383a40"

    // Text colors
    readonly property color textPrimary: "#f2f3f5"
    readonly property color textSecondary: "#b5bac1"
    readonly property color textMuted: "#949ba4"

    // Accent colors
    readonly property color accent: "#5865f2"
    readonly property color accentHover: "#4752c4"
    readonly property color danger: "#ed4245"
    readonly property color success: "#57f287"
    readonly property color warning: "#fee75c"

    // Font settings
    readonly property string fontFamily: "Segoe UI, Helvetica Neue, Helvetica, Arial, sans-serif"
    readonly property int fontSizeSmall: 12
    readonly property int fontSizeNormal: 14
    readonly property int fontSizeLarge: 18

    // Spacing
    readonly property int spacingSmall: 4
    readonly property int spacingNormal: 8
    readonly property int spacingLarge: 16

    // Radius
    readonly property int radiusSmall: 4
    readonly property int radiusNormal: 8
    readonly property int radiusLarge: 16

    // Consistent sizing
    readonly property int headerHeight: 48
    readonly property int buttonHeight: 32
    readonly property int iconButtonSize: 28

    // Sender colors for chat
    readonly property var senderColors: [
        "#f47067", "#e0823d", "#c4a000", "#57ab5a", "#39c5cf",
        "#6cb6ff", "#dcbdfb", "#f69d50", "#768390", "#e5534b"
    ]

    function senderColor(name) {
        var hash = 0;
        for (var i = 0; i < name.length; i++) {
            hash = name.charCodeAt(i) + ((hash << 5) - hash);
        }
        return senderColors[Math.abs(hash) % senderColors.length];
    }
}
