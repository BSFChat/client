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
import BSFChat

QtObject {
    id: theme

    // ─── Theme mode ──────────────────────────────────────────
    // Bound to the AppSettings singleton (registered from C++ in main.cpp)
    // so flipping theme/accent in settings propagates across every binding
    // that reads Theme.* — no manual refresh needed.
    property bool isDark: AppSettings.theme !== "light"

    // Mobile form factor — true on iOS / Android. Components that render
    // differently on touch (hide desktop-only affordances, cap dialog
    // widths, swap hover rails for long-press) key off this flag. Only
    // checks the OS rather than a viewport-size heuristic so a narrow
    // desktop window still gets the desktop UX.
    readonly property bool isMobile:
        Qt.platform.os === "ios" || Qt.platform.os === "android"

    // ─── Layout density variant ──────────────────────────────
    // Bound to the AppSettings singleton so the Appearance pane can flip
    // between "standard" / "compact" / "focus" at runtime.
    //
    // "standard" — default desktop layout
    // "compact"  — narrower rails + smaller participant tiles (see §1 of SPEC)
    // "focus"    — chat + members hidden, voice/screen share only
    property string variant: AppSettings.layoutVariant

    // ─── Surfaces ────────────────────────────────────────────
    // These are re-derived from the kit's oklch() source of truth —
    // NOT the tokens.json hex comments, which are incorrect (their
    // "hex approximations" are ~1 step lighter than the oklch values
    // they claim to represent). The mock renders in a browser that
    // interprets oklch() natively; to match it we convert the oklch
    // values to sRGB ourselves:
    //
    //   bg0 oklch(16% 0.008 260) → #0b0d11   (kit said #16171b)
    //   bg1 oklch(19% 0.008 260) → #121417   (kit said #1c1d22)
    //   bg2 oklch(22% 0.009 260) → #181b1f   (kit said #222329)
    //   bg3 oklch(26% 0.010 260) → #212429   (kit said #2a2b32)
    //   bg4 oklch(30% 0.012 260) → #2a2e34   (kit said #32333c)
    //
    // Using the actual oklch→sRGB conversions here so the app finally
    // matches the perceived darkness of the mock.
    readonly property color bg0:      isDark ? "#0b0d11"   : "#f3f5f8"
    readonly property color bg1:      isDark ? "#121417"   : "#fbfcfe"
    readonly property color bg2:      isDark ? "#181b1f"   : "#ebedef"
    readonly property color bg3:      isDark ? "#212429"   : "#dfe1e5"
    readonly property color bg4:      isDark ? "#2a2e34"   : "#cfd1d5"
    // Line = bg4 at 60% alpha / lineSoft = bg4 at 30% alpha.
    // Hex in Qt is #AARRGGBB; the kit's tokens.json mistakenly wrote
    // CSS-style #RRGGBBAA, so we move the alpha pair to the front.
    readonly property color line:     isDark ? "#992a2e34" : "#47cfd1d5"
    readonly property color lineSoft: isDark ? "#4d2a2e34" : "#24cfd1d5"

    // ─── Text ────────────────────────────────────────────────
    // Dark text derived from oklch(L 0.005..0.010 260) with the same
    // fix applied. Light text kept at kit values — those were closer.
    readonly property color fg0: isDark ? "#f3f5f9" : "#1a1b20"
    readonly property color fg1: isDark ? "#c1c4c9" : "#3a3b44"
    readonly property color fg2: isDark ? "#83868c" : "#6a6b73"
    readonly property color fg3: isDark ? "#5a5e63" : "#8f9099"
    readonly property color fg4: isDark ? "#404247" : "#b1b2b9"

    // ─── Accent (parameterized by hue + theme) ───────────────
    // Bound to the AppSettings singleton so swatches in the Appearance
    // pane flip every view's accent live.
    property int accentHue: AppSettings.accentHue
    // Alpha pairs written #AARRGGBB (Qt's convention), not #RRGGBBAA.
    readonly property var _accentDark: ({
        180: { accent: "#36d6c7", dim: "#1fa89c", glow: "#4036d6c7", glowStrong: "#8036d6c7" },
        260: { accent: "#a28bff", dim: "#7a60e0", glow: "#40a28bff", glowStrong: "#80a28bff" },
        320: { accent: "#ec6dd6", dim: "#c04daa", glow: "#40ec6dd6", glowStrong: "#80ec6dd6" },
         30: { accent: "#ffa34a", dim: "#d07d26", glow: "#40ffa34a", glowStrong: "#80ffa34a" }
    })
    readonly property var _accentLight: ({
        180: { accent: "#1d9991", dim: "#4cbeb4", glow: "#381d9991", glowStrong: "#701d9991" },
        260: { accent: "#6547d0", dim: "#9079e2", glow: "#386547d0", glowStrong: "#706547d0" },
        320: { accent: "#b83da0", dim: "#d26dbb", glow: "#38b83da0", glowStrong: "#70b83da0" },
         30: { accent: "#c46a1a", dim: "#e0953f", glow: "#38c46a1a", glowStrong: "#70c46a1a" }
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

    // Precomputed letter-spacing in pixels, keyed by font-size token.
    //
    // Call sites used to write
    //   font.letterSpacing: Theme.letterEm.widest * font.pixelSize
    // which reads `font.pixelSize` from the same `font` group property that
    // owns `letterSpacing` — Qt evaluates the group as a whole, so each
    // edit marks itself dirty and you get "Binding loop detected for
    // property font.letterSpacing" on every label in the app. Resolving
    // the product against a `Theme.fontSize` token here instead breaks the
    // self-reference.
    readonly property QtObject trackWide: QtObject {
        readonly property real xs:    fontSize.xs    * letterEm.wide
        readonly property real sm:    fontSize.sm    * letterEm.wide
        readonly property real base:  fontSize.base  * letterEm.wide
        readonly property real md:    fontSize.md    * letterEm.wide
        readonly property real lg:    fontSize.lg    * letterEm.wide
        readonly property real xl:    fontSize.xl    * letterEm.wide
        readonly property real xxl:   fontSize.xxl   * letterEm.wide
        readonly property real title: fontSize.title * letterEm.wide
    }
    readonly property QtObject trackWidest: QtObject {
        readonly property real xs:    fontSize.xs    * letterEm.widest
        readonly property real sm:    fontSize.sm    * letterEm.widest
        readonly property real base:  fontSize.base  * letterEm.widest
        readonly property real md:    fontSize.md    * letterEm.widest
        readonly property real lg:    fontSize.lg    * letterEm.widest
        readonly property real xl:    fontSize.xl    * letterEm.widest
        readonly property real xxl:   fontSize.xxl   * letterEm.widest
        readonly property real title: fontSize.title * letterEm.widest
    }
    readonly property QtObject trackTight: QtObject {
        readonly property real xs:    fontSize.xs    * letterEm.tight
        readonly property real sm:    fontSize.sm    * letterEm.tight
        readonly property real base:  fontSize.base  * letterEm.tight
        readonly property real md:    fontSize.md    * letterEm.tight
        readonly property real lg:    fontSize.lg    * letterEm.tight
        readonly property real xl:    fontSize.xl    * letterEm.tight
        readonly property real xxl:   fontSize.xxl   * letterEm.tight
        readonly property real title: fontSize.title * letterEm.tight
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

    // ─── Local sizing constants ──────────────────────────────
    // Ours — not in the Designer kit but used by our own components for
    // buttons, icon buttons, and header bars. Kept separate so it's
    // obvious what's kit-standard and what's BSFChat-specific.
    readonly property int   headerHeight:    48
    readonly property int   buttonHeight:    32
    readonly property int   iconButtonSize:  28

    // ─── Accessibility borders (our extension) ───────────────
    // Accessibility mode draws thick, accent-colored borders between panels
    // so the major regions are unambiguous. Driven from AppSettings.
    readonly property bool  accessibility:     AppSettings.accessibilityMode
    readonly property color panelBorder:       accessibility ? accent : bg0
    readonly property int   panelBorderWidth:  accessibility ? 3 : 1

    // ─── Sender color hash (our extension) ───────────────────
    // Hue-stable palette for per-user chat colors; pick by a cheap string
    // hash so the same @user always gets the same hue in any session.
    readonly property var senderColors: isDark
        ? ["#f47067", "#e0823d", "#c4a000", "#57ab5a", "#39c5cf",
           "#6cb6ff", "#dcbdfb", "#f69d50", "#768390", "#e5534b"]
        : ["#b42318", "#9a4a00", "#6b5200", "#2d6930", "#0a6c73",
           "#1c5fb5", "#6a3fa0", "#a1570a", "#3f4650", "#8b1a14"]
    function senderColor(name) {
        var hash = 0;
        for (var i = 0; i < name.length; i++) {
            hash = name.charCodeAt(i) + ((hash << 5) - hash);
        }
        return senderColors[Math.abs(hash) % senderColors.length];
    }
}
