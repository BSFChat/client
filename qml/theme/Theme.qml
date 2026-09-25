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

    // The four accent hues, in picker order, for any surface that offers the
    // CHOICE of accent rather than using the current one. Client Settings'
    // Appearance swatches had these four hex values written out again, which is
    // how a fifth accent gets added in one place and not the other.
    readonly property var accentHues: [
        { hue: 180, label: "Cyan"    },
        { hue: 260, label: "Violet"  },
        { hue: 320, label: "Magenta" },
        { hue:  30, label: "Amber"   }
    ]
    // The swatch colour for one of those hues, resolved for the current
    // light/dark mode exactly as `accent` is.
    function accentFor(hue) {
        var table = isDark ? _accentDark : _accentLight;
        return (table[hue] || table[180]).accent;
    }

    // ON A SCRIM. Some surfaces are always dark regardless of theme — the
    // image viewer's 88%-black backdrop, a video thumbnail, a letterboxed
    // frame — because the content behind them is a photo, not a panel. Text and
    // glyphs there are white in both themes, and `onAccent`/`fg0` are the wrong
    // tokens for it: both flip with the theme and would turn near-black on a
    // black backdrop. Named so the next person can tell "white because it sits
    // on a scrim" from "white because someone typed white".
    readonly property color scrim:   "#000000"
    readonly property color scrimFg: "#ffffff"

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

    // ─── Phone chrome gutter ─────────────────────────────────
    // The single left/right margin every full-width surface on a phone
    // keeps clear. It is NOT a safe-area inset (the window is already
    // inside the safe area — Qt lays it out there, and on an iPhone in
    // portrait UIKit's horizontal safe-area insets are 0 even on a
    // Dynamic Island device). It is the margin that keeps content out of
    // the display's ROUNDED CORNERS, which no inset describes.
    //
    // Why 16 and not the 8 the composer used to use. An iPhone 15's
    // display corner is a ~55 pt radius. Take the composer: a r3 (14 pt
    // radius) rounded rect, so the rightmost point of its border sits one
    // radius up from its own bottom edge. With the software keyboard up
    // its bottom margin is a handful of points, which puts that point at
    // roughly (x = gutter, y = 20) measured from the screen's
    // bottom-right corner. It is inside the glass iff
    //
    //     (55 − x)² + (55 − 20)² ≤ 55²
    //
    // x = 8  → 2209 + 1225 = 3434 > 3025   clipped — the reported bug
    // x = 16 → 1521 + 1225 = 2746 < 3025   clear, with room to spare
    //
    // The bottom margin in that sum is not stable — it changes with the
    // keyboard, and the shell may take it over entirely — so the gutter is
    // the half of this worth pinning: at 16 the composer clears the corner
    // at ANY bottom margin, including zero, and stops being the outermost
    // thing on the screen regardless.
    //
    // 16 is also plain iOS/Material body margin, so nothing looks odd for
    // having been derived this way. Anything laid out full-width on a
    // phone — the header row, the timeline, the composer — uses this one
    // number, because three different gutters (10, 16 and 8) is what made
    // the composer the outermost thing on the screen in the first place.
    readonly property int mobileGutter: 16

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

    // Interactive-control heights. The dialogs had grown a scatter of literal
    // 32 / 36 / 40 / 44 pixel heights, which is how a "Save" button ends up
    // 36px on one page and 40px on the next with nothing recording which was
    // the decision and which was the typo. Four named steps, so a control's
    // height is a choice from a scale rather than a number someone typed:
    //   sm — compact controls: icon buttons, chips, category tabs
    //   md — the default for buttons, text fields and combo boxes
    //   lg — primary actions and the taller inputs that anchor a pane
    //   xl — full-width search / filter bars, which want a comfortable target
    // Header BARS are not controls and keep their own `headerHeight` above.
    // Declared as a NAMED inline component rather than the anonymous
    // `QtObject { ... }` the older groups use. It is the same object at
    // runtime, but qmllint can only see the members of a group whose type it
    // knows: an anonymous QtObject is typed as bare QObject, so every
    // `Theme.sp.s3` in the tree is reported as MissingProperty — which
    // .qmllint.ini promotes to an error. This shape resolves cleanly, so the
    // new token adds nothing to that backlog, and it is the pattern the older
    // groups can be converted to for the same benefit.
    component ControlHeights: QtObject {
        readonly property int sm: 32
        readonly property int md: 36
        readonly property int lg: 40
        readonly property int xl: 44
    }
    readonly property ControlHeights controlHeight: ControlHeights {}

    // ─── Minimum touch target ────────────────────────────────
    // Apple's HIG asks for 44×44 pt and Material for 48×48 dp; 44 is the
    // number an App Store reviewer measures against, so it is the floor
    // every `Theme.isMobile ? … : …` branch in the tree uses for anything
    // a finger has to hit. It is deliberately a separate token from
    // controlHeight.xl (which happens to be 44 too): that one is a
    // typographic choice about how tall a button looks, this one is a
    // hard accessibility minimum, and they must be free to diverge.
    readonly property int touchTarget: 44

    // ─── Accessibility borders (our extension) ───────────────
    // Accessibility mode draws thick, accent-colored borders between panels
    // so the major regions are unambiguous. Driven from AppSettings.
    readonly property bool  accessibility:     AppSettings.accessibilityMode
    readonly property color panelBorder:       accessibility ? accent : bg0
    readonly property int   panelBorderWidth:  accessibility ? 3 : 1

    // ─── Sender color hash (our extension) ───────────────────
    // Hue-stable palette for per-user chat colors; pick by a cheap string
    // hash so the same @user always gets the same hue in any session. Used
    // for sender names and for the fill behind avatar initials.
    //
    // ── Why these ten values and not the previous ten ──
    //
    // The store screenshots showed four different people in one channel and
    // all four read as "red": two of them #e5534b and two #f47067. That was
    // not bad luck and it was not the hash. Converting the OLD palette to
    // OKLCH says what it was:
    //
    //   #f47067 h= 26°    #e5534b h= 27°     <- 0.6° apart
    //   #e0823d h= 54°    #f69d50 h= 59°     <- 5° apart
    //   #768390 C= 0.025                     <- grey, not a colour at all
    //
    // Ten slots holding seven distinguishable colours, four of them packed
    // into the 26°–59° red-orange arc, and one entry so desaturated it was
    // indistinguishable from `fg2` secondary text. Two of the pairs differed
    // only in LIGHTNESS at the same hue, which is the one axis a reader does
    // not name: both members of such a pair are just "red". Smallest gap
    // between any two entries was ΔE(OKLab) 0.067 dark / 0.035 light.
    //
    // The hash was measured before the palette was touched, because it was
    // the first suspect. It is fine: 20k synthetic OIDC mxids
    // (`@oidc_<32 hex>:host`, the shape this server issues) spread over the
    // ten buckets at 9.7%–10.5% each. `sender` is the Matrix event sender,
    // i.e. the full mxid (MessageModel::SenderRole -> msg.sender), and every
    // other call site passes a userId or peerId, so the whole tree hashes
    // the same kind of string. Nothing to fix there.
    //
    // So these are generated rather than picked by eye: ten hues at exactly
    // 36° around the OKLCH circle, each one's lightness solved by bisection
    // for a FIXED WCAG contrast against the surface it is read on (`bg0`,
    // which is what the timeline and the drawer both paint). Constant OKLab
    // lightness would NOT have given constant contrast — WCAG relative
    // luminance weights green about ten times as heavily as blue, so a
    // perceptually level palette is a legibility cliff between the two.
    //
    // Measured, both themes, against bg0:
    //   dark   contrast 6.96–7.06  (was 5.02–11.77)
    //   light  contrast 5.55–5.66  (was 4.95–8.73), and 4.85–4.94 against
    //          the hovered bubble, which darkens bg0 by 6% black
    //   smallest hue gap  35.1°    (was 0.4°)
    //   smallest ΔE OKLab  0.059   (was 0.035)
    //
    // The avatar fill has to carry `onAccent` initials as well: #0a0a0a on
    // the dark fills is 7.09–7.19, #ffffff on the light fills is 6.06–6.18.
    //
    // Still TEN entries, deliberately. Widening to 12 or 16 was tried on
    // paper and rejected: with ten colours and six people in a channel some
    // pair collides 85% of the time, with sixteen it is still 66%, so no
    // reachable palette size makes collisions rare and chasing them only
    // buys a tighter hue spacing (30° at twelve, 22.5° at sixteen) — paying
    // in the one property that was actually broken. Two users sharing a
    // colour is a hash doing its job; two colours nobody can tell apart is
    // the bug.
    //
    // A collision is also not silent: the sender's NAME is next to the
    // swatch everywhere this is used. The colour is a scanning aid, never
    // the identifier.
    //
    // Regenerating: hue_i = 25° + i*36°, bisect OKLab L for contrast 7.0
    // (dark, on #0b0d11) or 5.6 (light, on #f3f5f8), chroma = min(cap,
    // in-gamut max at that L) with cap 0.135 dark / 0.145 light. Entries
    // that fall short of the cap are gamut-limited, not hand-edited: sRGB
    // simply has no more chroma at that hue and lightness.
    readonly property var senderColors: isDark
        ? ["#e87b74", "#da883a", "#b39a1d", "#76aa4d", "#00b087",
           "#00acb9", "#3ba3e5", "#8994f0", "#bd85dc", "#de7cae"]
        : ["#aa3c3a", "#935200", "#736100", "#3e6e00", "#007055",
           "#006c76", "#00679c", "#5259b6", "#8349a1", "#a03d74"]
    function senderColor(name) {
        var hash = 0;
        for (var i = 0; i < name.length; i++) {
            hash = name.charCodeAt(i) + ((hash << 5) - hash);
        }
        return senderColors[Math.abs(hash) % senderColors.length];
    }
}
