# BSFChat — Qt Handoff Kit

Everything Claude Code needs to port `BSFChat.html` to Qt/QML. **Dark + light themes, 34 icons, density variants, font instructions, and model stubs — all included.**

| Path | Purpose |
|---|---|
| **`SPEC.md`** | Porting guide — read first. One section per screen, plus models, effects, traps. |
| **`Theme.qml`** + `qmldir` | Drop-in QML singleton. `Theme.isDark`, `Theme.accentHue`, `Theme.variant` all live-bindable. |
| **`tokens.json`** | Same values as raw data — for code-gen or C++ consumers. |
| **`icons/*.svg`** | 34 line icons pre-extracted from the mock, `stroke="currentColor"` ready to tint. |
| **`fonts/README.md`** | Where to get Geist + how to wire FontLoader. |
| **`BSFChat.html`** | Self-contained bundled mock (opens offline — takes ~2s to unpack on first load). |

## Quick start

1. Copy this whole folder into your Qt repo at `docs/design/`.
2. Copy `Theme.qml` + `qmldir` into a QML module directory — e.g. `qml/theme/`.
3. Copy `icons/*.svg` into `resources/icons/` and reference from a `.qrc`.
4. Follow `fonts/README.md` to pull Geist and load it via `FontLoader`.
5. From any QML file: `import BSFChat.Theme 1.0` → `Theme.bg1`, `Theme.accent`, `Theme.layout.serverRailW`, `Theme.onAccent`, etc.
6. Point Claude Code at `SPEC.md` and port one screen at a time.

## What's in Theme.qml

- **Themeable:** `Theme.isDark = false` swaps surfaces, text, state colors, accent lightness, shadow color.
- **Accent-switchable:** `Theme.accentHue = 320` (preset hues 30 / 180 / 260 / 320).
- **Density-aware:** `Theme.variant = "compact" | "focus" | "standard"` — `Theme.layout.*` re-resolves automatically.
- **On-accent text:** `Theme.onAccent` — white in light, near-black in dark.
- **Shadow numerics:** `Theme.shadow.blur1/2/3`, `offsetY1/2/3`, `opacity1/2/3` — plug straight into `MultiEffect`.
- **Exact bezier easing:** `Theme.motion.bezier` is `[0.2, 0, 0, 1, 1, 1]` — pair with `Easing.BezierSpline`.

## Suggested Claude Code prompt

```
Read docs/design/SPEC.md end to end, then open the mock at
docs/design/BSFChat.html for visual reference.

Port <ComponentName> (SPEC §X.Y) to QML. Rules:
  • Use the Theme singleton — never hardcode colors or sizes.
  • Bind to the model roles listed in SPEC §2 (create a stub
    ListModel with matching roles if the C++ model doesn't exist yet).
  • One QML file; commit path: qml/views/<ComponentName>.qml
  • Match hover/active/speaking states described in the spec.
  • Ask me before inventing behavior not in the mock or spec.
```

## Keep this in sync

When the design mock changes:
- **New token** → edit `Theme.qml` **and** `tokens.json`
- **New icon** → re-export from `components/icons.jsx` into `icons/`
- **New screen or layout change** → update `SPEC.md`
- **Never let the Qt implementation silently diverge from the mock.** If you need to diverge, update the mock first, then regenerate this kit.
