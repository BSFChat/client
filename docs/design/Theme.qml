// Theme.qml — BSFChat design tokens as a QML singleton.
// Themeable: set `Theme.isDark = false` for light mode.
//
// USAGE
//   1. Put this file + qmldir in a folder (e.g. qml/theme/)
//   2. qmldir:
//          module BSFChat.Theme
//          singleton Theme 1.0 Theme.qml
//   3. In QML:
//          import BSFChat.Theme 1.0
//          Rectangle { color: Theme.bg1 }
//   4. Bind theme + accent + layout variant to your user settings:
//          Theme.isDark    = userSettings.darkMode
//          Theme.accentHue = userSettings.accentHue
//          Theme.variant   = "compact"   // "standard" | "compact" | "focus"
//
// All color properties auto-update when `isDark` changes (they're bindings).

pragma Singleton
import QtQuick

QtObject {
    id: theme

    // ─── Theme mode ──────────────────────────────────────────
    property bool isDark: true

    // ─── Layout density variant ──────────────────────────────
    // "standard" — default desktop layout
    // "compact"  — narrower rails + smaller participant tiles (see §1 of SPEC)
    // "focus"    — chat + members hidden, voice/screen share only
    property string variant: "standard"

    // ─── Surfaces ────────────────────────────────────────────
    readonly property color bg0:      isDark ? "#16171b"   : "#f4f4f6"
    readonly property color bg1:      isDark ? "#1c1d22"   : "#fbfbfc"
    readonly property color bg2:      isDark ? "#222329"   : "#ededf0"
    readonly property color bg3:      isDark ? "#2a2b32"   : "#e2e2e7"
    readonly property color bg4:      isDark ? "#32333c"   : "#d3d4da"
    readonly property color line:     isDark ? "#32333c99" : "#a8a9b247"
    readonly property color lineSoft: isDark ? "#32333c4d" : "#a8a9b224"

    // ─── Text ────────────────────────────────────────────────
    readonly property color fg0: isDark ? "#f5f5f7" : "#1a1b20"
    readonly property color fg1: isDark ? "#c8c9ce" : "#3a3b44"
    readonly property color fg2: isDark ? "#8e8f97" : "#6a6b73"
    readonly property color fg3: isDark ? "#6a6b73" : "#8f9099"
    readonly property color fg4: isDark ? "#50515a" : "#b1b2b9"

    // ─── Accent (parameterized by hue + theme) ───────────────
    property int accentHue: 180
    readonly property var _accentDark: ({
        180: { accent: "#36d6c7", dim: "#1fa89c", glow: "#36d6c740", glowStrong: "#36d6c780" },
        260: { accent: "#a28bff", dim: "#7a60e0", glow: "#a28bff40", glowStrong: "#a28bff80" },
        320: { accent: "#ec6dd6", dim: "#c04daa", glow: "#ec6dd640", glowStrong: "#ec6dd680" },
         30: { accent: "#ffa34a", dim: "#d07d26", glow: "#ffa34a40", glowStrong: "#ffa34a80" }
    })
    readonly property var _accentLight: ({
        180: { accent: "#1d9991", dim: "#4cbeb4", glow: "#1d999138", glowStrong: "#1d999170" },
        260: { accent: "#6547d0", dim: "#9079e2", glow: "#6547d038", glowStrong: "#6547d070" },
        320: { accent: "#b83da0", dim: "#d26dbb", glow: "#b83da038", glowStrong: "#b83da070" },
         30: { accent: "#c46a1a", dim: "#e0953f", glow: "#c46a1a38", glowStrong: "#c46a1a70" }
    })
    readonly property var _a: (isDark ? _accentDark : _accentLight)[accentHue]
                              || (isDark ? _accentDark : _accentLight)[180]
    readonly property color accent:           _a.accent
    readonly property color accentDim:        _a.dim
    readonly property color accentGlow:       _a.glow
    readonly property color accentGlowStrong: _a.glowStrong

    // Text/glyph color for use ON the accent (pills, bars, primary buttons).
    // White on the light-mode accent; near-black on the dark-mode accent.
    readonly property color onAccent: isDark ? "#0a0a0a" : "#ffffff"

    // ─── State colors ────────────────────────────────────────
    readonly property color danger:  isDark ? "#f04a5a" : "#d23040"
    readonly property color warn:    isDark ? "#e7c156" : "#b8842a"
    readonly property color online:  isDark ? "#2ecb8a" : "#1e9862"
    readonly property color idle:    isDark ? "#d9b64f" : "#b8842a"
    readonly property color dnd:     isDark ? "#f04a5a" : "#d23040"
    readonly property color offline: isDark ? "#73747c" : "#aeafb6"

    // ─── Shadow — numeric tokens for MultiEffect ─────────────
    // Feed these straight into MultiEffect { shadowColor; shadowBlur; shadowVerticalOffset; shadowOpacity }
    // Blur is normalised 0..1 for MultiEffect (rough mapping from CSS px).
    readonly property color shadowColor: isDark ? "#000000" : "#141628"
    readonly property QtObject shadow: QtObject {
        readonly property real blur1:    0.08   // ~2px CSS
        readonly property real blur2:    0.32   // ~16px CSS
        readonly property real blur3:    0.80   // ~48px CSS
        readonly property int  offsetY1: 1
        readonly property int  offsetY2: 4
        readonly property int  offsetY3: 12
        readonly property real opacity1: theme.isDark ? 0.30 : 0.06
        readonly property real opacity2: theme.isDark ? 0.40 : 0.08
        readonly property real opacity3: theme.isDark ? 0.55 : 0.12
    }
    // (legacy names kept so existing bindings don't break)
    readonly property real shadowAlpha1: shadow.opacity1
    readonly property real shadowAlpha2: shadow.opacity2
    readonly property real shadowAlpha3: shadow.opacity3

    // ─── Typography ──────────────────────────────────────────
    readonly property string fontSans: "Geist"
    readonly property string fontMono: "Geist Mono"

    readonly property QtObject fontSize: QtObject {
        readonly property int xs:    11
        readonly property int sm:    12
        readonly property int base:  13
        readonly property int md:    14
        readonly property int lg:    16
        readonly property int xl:    20
        readonly property int xxl:   24
        readonly property int title: 32
    }

    readonly property QtObject fontWeight: QtObject {
        readonly property int regular:  Font.Normal
        readonly property int medium:   Font.Medium
        readonly property int semibold: Font.DemiBold
        readonly property int bold:     Font.Bold
    }

    // em multipliers — multiply by font.pixelSize at the call site
    readonly property QtObject letterEm: QtObject {
        readonly property real tightest: -0.03
        readonly property real tight:    -0.02
        readonly property real normal:    0.00
        readonly property real wide:      0.08
        readonly property real widest:    0.12
    }

    // ─── Radii ───────────────────────────────────────────────
    readonly property int r1:   6
    readonly property int r2:   10
    readonly property int r3:   14
    readonly property int r4:   18
    readonly property int r5:   24
    readonly property int pill: 9999

    // ─── Spacing scale ───────────────────────────────────────
    readonly property QtObject sp: QtObject {
        readonly property int s1:  4
        readonly property int s2:  6
        readonly property int s3:  8
        readonly property int s4:  10
        readonly property int s5:  12
        readonly property int s6:  14
        readonly property int s7:  16
        readonly property int s8:  20
        readonly property int s9:  24
        readonly property int s10: 32
        readonly property int s11: 40
        readonly property int s12: 56
    }

    // ─── Avatar sizes ────────────────────────────────────────
    readonly property QtObject avatar: QtObject {
        readonly property int sm:  24   // inline chips
        readonly property int md:  32   // member list, DM list
        readonly property int lg:  40   // chat messages, server rail tiles
        readonly property int xl:  80   // voice room tiles
        readonly property int xxl: 132  // DM voice call hero
    }

    // ─── Layout constants (density-aware) ────────────────────
    // Read values off `Theme.layout.*`; it switches on `Theme.variant`.
    readonly property QtObject _layoutStandard: QtObject {
        readonly property int appMinW:           1280
        readonly property int appMinH:            800
        readonly property int serverRailW:         72
        readonly property int channelSidebarW:    240
        readonly property int chatPanelW:         320
        readonly property int memberListW:        220
        readonly property int voiceDockH:          64
        readonly property int titleBarH:           40
        readonly property int participantTileW:   220
        readonly property int participantTileH:   180
        readonly property int participantGap:      12
        readonly property int participantRadius:   14
        readonly property int dmListW:            280
        readonly property bool showChat:         true
        readonly property bool showMembers:      true
    }
    readonly property QtObject _layoutCompact: QtObject {
        readonly property int appMinW:           1280
        readonly property int appMinH:            800
        readonly property int serverRailW:         60
        readonly property int channelSidebarW:    200
        readonly property int chatPanelW:         280
        readonly property int memberListW:        180
        readonly property int voiceDockH:          56
        readonly property int titleBarH:           40
        readonly property int participantTileW:   180
        readonly property int participantTileH:   140
        readonly property int participantGap:      10
        readonly property int participantRadius:   12
        readonly property int dmListW:            240
        readonly property bool showChat:         true
        readonly property bool showMembers:      true
    }
    readonly property QtObject _layoutFocus: QtObject {
        readonly property int appMinW:           1280
        readonly property int appMinH:            800
        readonly property int serverRailW:         72
        readonly property int channelSidebarW:    240
        readonly property int chatPanelW:           0
        readonly property int memberListW:          0
        readonly property int voiceDockH:          64
        readonly property int titleBarH:           40
        readonly property int participantTileW:   260
        readonly property int participantTileH:   200
        readonly property int participantGap:      14
        readonly property int participantRadius:   14
        readonly property int dmListW:            280
        readonly property bool showChat:        false
        readonly property bool showMembers:     false
    }
    readonly property QtObject layout:
        variant === "compact" ? _layoutCompact :
        variant === "focus"   ? _layoutFocus   :
                                _layoutStandard

    // ─── Motion ──────────────────────────────────────────────
    // Easing is the exact cubic-bezier(0.2,0,0,1) from tokens — use the bezierCurve form.
    readonly property QtObject motion: QtObject {
        readonly property int fastMs:              120
        readonly property int normalMs:            180
        readonly property int slowMs:              280
        readonly property int speakingRingPulseMs: 900
        // Apply with: easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.motion.bezier
        readonly property var  bezier:             [0.2, 0, 0, 1, 1, 1]
        // Convenience fallback if a component needs a single enum value:
        readonly property int  easing:             Easing.BezierSpline
    }
}
