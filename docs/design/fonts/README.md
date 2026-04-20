# Fonts — Geist & Geist Mono

The mock uses **Geist** (sans) and **Geist Mono**. They're not bundled in this kit — grab them once from the canonical source:

- **Repo:** https://github.com/vercel/geist-font
- **Release files needed:**
  - `Geist[wght].ttf` (variable, recommended) **or** `Geist-{Regular,Medium,SemiBold,Bold}.ttf`
  - `GeistMono[wght].ttf` (variable) **or** the static weights
- **License:** SIL Open Font License 1.1 — redistribution OK; include the OFL.txt alongside.

## Drop into your Qt resource bundle

```
resources/
  fonts/
    Geist-Variable.ttf
    GeistMono-Variable.ttf
    OFL.txt
```

Add to your `qml.qrc`:

```xml
<qresource prefix="/fonts">
  <file>fonts/Geist-Variable.ttf</file>
  <file>fonts/GeistMono-Variable.ttf</file>
</qresource>
```

## Load once in `main.qml`

```qml
import QtQuick

Window {
    FontLoader { id: geist;     source: "qrc:/fonts/Geist-Variable.ttf" }
    FontLoader { id: geistMono; source: "qrc:/fonts/GeistMono-Variable.ttf" }

    // Everything below references Theme.fontSans / Theme.fontMono —
    // which resolve to "Geist" / "Geist Mono", matching the loaded family names.
}
```

If you skip this, Qt silently falls back to the platform default sans — layout still works but the type feel is wrong (Geist is noticeably wider and lower-contrast than e.g. Inter or SF).

## Static-weight fallback

If your Qt build chokes on variable fonts, load 4 static weights instead and map them:

```qml
FontLoader { source: "qrc:/fonts/Geist-Regular.ttf" }
FontLoader { source: "qrc:/fonts/Geist-Medium.ttf" }
FontLoader { source: "qrc:/fonts/Geist-SemiBold.ttf" }
FontLoader { source: "qrc:/fonts/Geist-Bold.ttf" }
```

Qt will pick the closest match off `font.weight` at render time.
