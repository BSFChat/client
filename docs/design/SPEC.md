# BSFChat — Qt/QML Port Spec

This document is the bridge between `BSFChat.html` (the design mock) and your Qt/QML implementation. Read this, then implement screen-by-screen with `BSFChat.html` open for pixel reference.

**Pair with:**
- `Theme.qml` + `qmldir` — ready-to-use design-token singleton (dark + light)
- `tokens.json` — the same values in raw form
- `BSFChat.html` in the mock — the source of truth for layout & behavior

When the mock and this doc disagree, **the mock wins.** Update this doc.

**One exception, and it is absolute: the mock never wins on a security claim.** The mock was drawn before the transport existed and shows properties BSFChat does not have — an `MLS · 256` badge, a "Verified cross-server" shield, key fingerprints. BSFChat has already shipped one of those (`MLS · 256`, against a DTLS+SCTP reality) and had to correct it. Anything the UI says about encryption, verification or keys comes from `src/voice/VoiceEncryption.{h,cpp}` and nowhere else — not from the mock, not from a QML literal, not from this doc. Read that header before writing such a string; `tests/test_voice_encryption.cpp` fails the build's test suite if one appears in QML.

---

## 0 · Ground rules

1. **Never hardcode colors, sizes, or font names in a QML view.** Use `Theme.*`. If a value is missing from Theme, add it there first.
2. **The Theme singleton is themeable.** `Theme.isDark = false` swaps the whole surface/text/state palette. All color properties are bindings — views auto-update, don't snapshot colors at construction.
3. **On-accent text uses `Theme.onAccent`** (not a hardcoded `#fff` or `#000`) — it's white in light mode, near-black in dark.
4. **Views are dumb; models are smart.** Every list — servers, channels, members, DMs, messages — is backed by a `QAbstractListModel` in C++.
5. **One QML file per screen**, matching the JSX filenames.
6. **Accent is live-bindable.** `Theme.accentHue = ...` updates every view; same rules.
7. **No hand-rolled window chrome** — use a native frameless window or the platform default.

---

## 1 · App shell

```
┌─────────────────────────────────────────────────────────────────┐
│ title bar (native, 40h)                                          │
├──────┬─────────┬────────────────────────────┬──────────┬────────┤
│      │         │                            │          │        │
│ rail │ channel │         main content       │   chat   │ member │
│ 72w  │ sidebar │ (VoiceRoom / ScreenShare / │  panel   │  list  │
│      │  240w   │  DMScreen / Settings)      │  320w    │  220w  │
│      │         │                            │ (toggle) │        │
│      │         ├────────────────────────────┤          │        │
│      │         │      voice dock  64h       │          │        │
└──────┴─────────┴────────────────────────────┴──────────┴────────┘
```

- Min window size: **1280 × 800**
- All column widths come from `Theme.layout.*` — **which is a density-aware view.** Read e.g. `Theme.layout.chatPanelW`; it's whichever of `_layoutStandard` / `_layoutCompact` / `_layoutFocus` matches `Theme.variant`. You don't branch in views.
- `SplitView` with `orientation: Qt.Horizontal`; the chat panel is a child that flips `visible` based on `Theme.layout.showChat` (false in focus mode)
- When `settings.screen === 'dms'`, the ServerRail's DM icon is active and the channel sidebar is hidden (DMScreen brings its own list)
- When `settings.screen === 'settings'`, Settings covers main + dock (full-width modal inside the app)

**Layout variants** (set via `Theme.variant`):
- `'standard'` (default) — wide rails, 220×180 participant tiles, chat + members both shown
- `'compact'` — narrower rails (server 60 / channel 200 / chat 280 / members 180), 180×140 tiles
- `'focus'` — chat + member list hidden (`showChat`/`showMembers` false), 260×200 tiles for voice/screen-share-only viewing
- `'focus'` — chatPanel + memberList hidden; VoiceRoom fills

---

## 2 · Models (C++, `QAbstractListModel`)

All exposed via `qmlRegisterType` under `BSFChat.Models 1.0`.

### Stub model snippet

Every model below follows the same pattern. Here's the minimum viable skeleton — copy, rename, add roles:

```cpp
// servermodel.h
#pragma once
#include <QAbstractListModel>
#include <QVector>

struct ServerRow {
    QString id, name, abbr;
    QColor  color;
    int     unread = 0;
    int     notif  = 0;
    bool    active = false;
    bool    home   = false;
};

class ServerListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole+1, NameRole, AbbrRole, ColorRole,
                 UnreadRole, NotifRole, ActiveRole, HomeRole };

    int rowCount(const QModelIndex& = {}) const override { return m_rows.size(); }
    QVariant data(const QModelIndex& i, int role) const override;
    QHash<int, QByteArray> roleNames() const override {
        return { {IdRole,"id"},{NameRole,"name"},{AbbrRole,"abbr"},{ColorRole,"color"},
                 {UnreadRole,"unread"},{NotifRole,"notif"},{ActiveRole,"active"},{HomeRole,"home"} };
    }
    Q_INVOKABLE void setRows(QVector<ServerRow> rows) {
        beginResetModel(); m_rows = std::move(rows); endResetModel();
    }
private:
    QVector<ServerRow> m_rows;
};
```

Register in `main.cpp`:
```cpp
qmlRegisterType<ServerListModel>("BSFChat.Models", 1, 0, "ServerListModel");
```

Use in QML:
```qml
import BSFChat.Models 1.0
ListView {
    model: ServerListModel { id: servers; Component.onCompleted: servers.setRows(...) }
    delegate: ServerTile { /* bind to id, name, abbr, … via roleNames */ }
}
```

### `ServerListModel` → ServerRail
Roles: `id`, `name`, `abbr` (2-letter), `color` (hex), `unread` (int), `notif` (int), `active` (bool), `home` (bool).

### `ChannelListModel` → ChannelSidebar
Roles: `id`, `name`, `kind` (`"text"`|`"voice"`), `topic`, `unread`, `memberCount`, `active`.
Grouped by category; use `section.property: "category"` in QML.

### `VoiceMemberModel` → VoiceRoom, ScreenShare tiles
Roles: `id`, `name`, `hue` (int, for avatar color), `muted`, `deafened`, `speaking`, `level` (0..1 audio level, emitted at ~20Hz), `sharing` (bool), `isSelf`, `status` (free text).

### `ChatMessageModel` → ChatPanel & DMConversation
Roles: `id`, `authorId`, `authorName`, `authorHue`, `timestamp` (ISO), `kind` (`"text"`|`"voice"`|`"screenshot"`|`"gameInvite"`|`"day"`|`"system"`), `body`, `durationSec`, `thumbUrl`, `gameData` (var), `isMe`, `pending`.

### `DMListModel` → DMScreen left column
Roles: `id`, `name`, `handle` (`"user@server"`), `hue`, `presence` (`"online"|"idle"|"dnd"|"offline"|"voice"|"playing"`), `statusLine`, `unread`, `lastActive`, `inVoiceRoom` (bool), `gameActivity` (string|null), `serverDomain`.

### `FriendRequestModel`, `ServerMemberModel` — similar shape.

### Controllers (not list models)
- `CallController` — `Q_PROPERTY`: `muted`, `deafened`, `inCall`, `pttActive`, `latencyMs`, `sharingScreen`, `annotateMode`. Slots: `toggleMute()`, `toggleDeafen()`, `startShare()`, `stopShare()`, `leave()`, `pushToTalk(bool)`. (The mock's `cryptoLabel` is **not** on this list. The protection label already exists as `ServerConnection.voiceProtectionBadge` / `voiceProtectionDetail`, fed by `voice::protectionBadge()`. A second property producing the same label is a second place for it to be wrong — bind to the existing one.)
- `AppSettings` — persisted via `QSettings`. Properties: `accentHue`, `layout`, `chatPanel`, `dmSubScreen`, `dmDensity`, `dmGameActivity`, `screen`. Bound two-way. (`dmFingerprint` deleted — it gated a banner claiming a verification BSFChat does not perform; see §3.8.)
- `NetworkClient` — WebSocket + signalling. Emits `connected`, `disconnected`, `latencyChanged`, exposes `probeServer(host)` for onboarding.

---

## 3 · Screen specs

Each section lists: **source file in mock**, **purpose**, **layout**, **key bindings**, **behavior**, **translation notes**.

---

### 3.1 ServerRail
**Source:** `components/ServerRail.jsx`
**Purpose:** left-most 72px column; server switcher + DM entry.

**Layout (top→bottom):**
1. DM icon (44×44, radius `Theme.r3`) — active when `settings.screen === 'dms'`
2. Thin divider (`Theme.lineSoft`)
3. Server icons (44×44 rounded-square, become squircle when active) — from `ServerListModel`
4. "+" add-server button (ghost style, dashed border `Theme.line`)

**Per-icon details:**
- Unread dot: 8×8 circle, `Theme.fg0`, bottom-right, 2px `Theme.bg0` border-halo
- Notif count pill: `Theme.danger` background, white text, min 16×16
- Active indicator: 4×28 `Theme.accent` bar on the LEFT edge (animates height on hover/active via `Behavior on height`)
- Hover: icon scales 1.04, border-radius tween from `Theme.r3` → `r2`

**Translation notes:**
- Use `ListView` (vertical) with `delegate` as a component, OR `Repeater` if model is small.
- The left-edge active bar is a sibling `Rectangle` positioned at `x: 0` of the row.
- Pulsing unread ring (for voice-room-active servers): `SequentialAnimation` on `scale` between 1.0 and 1.15, `loops: Animation.Infinite`.

---

### 3.2 ChannelSidebar
**Source:** `components/ChannelSidebar.jsx`
**Purpose:** 240px. Server header, channel list grouped by category, self-user panel docked bottom.

**Top (48h):** server name (fg0, 16px semibold) + chevron menu button.

**List:** `ListView` with `section.property: "category"`, `section.delegate` = collapsible group header (uppercase, letterSpacing 0.12em, fg3).

**Channel row (28h, 6px horiz pad, r1 radius on hover):**
- Icon: `#` for text, speaker for voice (14px, fg2)
- Name: 13px, fg1 (fg0 when active, fg2 for read)
- Trailing: unread pill OR voice member count
- Active state: bg `Theme.bg3`, text `Theme.fg0`, a 2px left accent stripe

**Voice channels:** when active, row expands to show nested member list (16px indent, 22px rows w/ 16×16 avatar + speaking ring).

**Bottom (VoiceStatusCard, 72h, r2, bg `Theme.bg2`):**
Shown only when in a voice room. Contains: server/channel name, latency (mono, fg2), disconnect button (danger ghost).

**Bottom (SelfUserPanel, 52h, bg `Theme.bg1`):**
Always shown. Avatar + handle + mute/deafen/settings icon buttons. Mute/deafen are bound to `CallController`.

---

### 3.3 VoiceRoom (main content, default)
**Source:** `components/VoiceRoom.jsx`
**Purpose:** active voice call view — participant grid + header + scene chrome.

**Header (56h):**
- Left: channel name (20px, semibold, fg0) + member count (fg2) + room topic (fg3, italic)
- Right: invite button, region/latency chip (mono, fg2), more-menu

**Participant grid:**
- `GridLayout` with `columns: Math.max(1, floor((width - gap) / (tileW + gap)))`
- Tile: `Theme.layout.participantTileW × H`, `gap: participantGap`, radius `participantRadius`
- Background: `Theme.bg2`
- Compact layout uses the `-Compact` dimensions

**ParticipantTile contents:**
- Avatar centered — 80×80 (compact: 64). Initial letter on a colored square (hue from model).
- Name below avatar (14px fg0) + status line (12px fg2)
- Bottom-left: muted icon (danger bg `danger22`) OR speaking-ring around avatar
- Bottom-right: latency mono fg3
- Sharing-screen tile: replace avatar with live thumbnail (use `Image` or `VideoOutput`); border becomes `Theme.accent` + `accentGlow` shadow

**Speaking ring:**
- Outer `Rectangle` ring, `border.width: 2 + level*4`, `border.color: Theme.accent`
- `Rectangle.layer.enabled: true` + `MultiEffect { shadowEnabled: true; shadowColor: Theme.accentGlowStrong; shadowBlur: 0.6 }` so it glows
- `Behavior on border.width` with `NumberAnimation { duration: 80 }` — don't over-animate

**Scene chrome (top-right):**
- Transport badge: bound to `ServerConnection.voiceProtectionBadge` (mono, 11px, `accent` fg, `accentGlow` bg, r1). Hidden while that string is empty — i.e. until a transport has actually started, so the badge never describes a call that has not connected. Hover shows `voiceProtectionDetail`, which states the limitation next to the guarantee. **Do not write the label in QML.** It depends on which transport carried the call: the mesh transport yields `DTLS · SCTP` (peer to peer, one DTLS 1.2 handshake per peer pair; Opus audio rides an SCTP data channel with no SRTP, video RTP rides SRTP keyed by that same handshake — and none of it is end-to-end in the sense users mean), a LiveKit SFU call yields `Encrypted · shared key` (a key the BSFChat server generates and holds — the relay cannot read the media, but the server can, so it is **not** end-to-end). The mock's `MLS · 256` is fiction: we do not implement MLS.
- Connection quality bars (5 mini rects)

---

### 3.4 ScreenShare (main content, alternate)
**Source:** `components/ScreenShare.jsx`
**Purpose:** when `CallController.sharingScreen === true`. Replaces the participant grid.

**Layout:**
- Big stream area (fills main minus 96h for mini-tile strip below)
- Strip of mini participant tiles (100w × 64h) along the bottom, horizontally scrollable
- Floating toolbar top-center (r5 pill, bg `Theme.bg2`, shadow3): cursor, annotate, pointer, clear, stop-share (danger)
- When `annotateMode === true`: a `Canvas` or `Shape` overlay takes pointer events; strokes are drawn as `PathPolyline` with the accent color

**Translation notes:**
- Annotation strokes should live in a `StrokeModel` (C++) with roles `points`, `color`, `width`. Pen input goes via `MouseArea` → `controller.addStrokePoint(x,y)`.
- Use `QtQuick.Shapes` for smooth strokes; `Canvas` is a fallback.

---

### 3.5 VoiceDock (sticky bottom, 64h)
**Source:** `components/VoiceDock.jsx`
**Purpose:** persistent call controls below main content.

**Contents (left→right):**
- Self avatar + handle + "Connected to #raid-ops" (fg2)
- Center cluster (absolute-centered): mute, deafen, share-screen, video (disabled/ghost), disconnect (danger)
- Right cluster: output device chip, input-meter (6 vertical bars, accent-tinted, driven by audio level)

**Dock button spec:**
- 40×40, r2, icon 20px
- Idle: bg `Theme.bg2`, icon `Theme.fg1`
- Hover: bg `Theme.bg3`
- Active (toggled on, e.g. muted): bg `Theme.danger22` (or `accent22` for positive-actives), icon `Theme.danger`/`accent`
- Primary (disconnect): bg `Theme.danger`, icon white; hover brightens

---

### 3.6 ChatPanel (right side, 320w, toggleable)
**Source:** `components/ChatPanel.jsx`
**Purpose:** text chat for the current channel/DM.

**Header (40h):** `#channel-name` + pinned count + search.

**Message list:** `ListView` bottom-anchored (`verticalLayoutDirection: ListView.BottomToTop` with reversed model, OR anchor scroll to bottom on append).

**Message row:**
- 40×40 avatar (only on first message of a run)
- Author (14px fg0 semibold) + timestamp (11px fg3)
- Body (13px fg1) — supports markdown-like bold/italic/code; keep to a `TextEdit` with `textFormat: Text.RichText` if safe, otherwise a minimal custom formatter
- Reaction pills below: r5, bg `Theme.bg3`, fg2, count badge; hover shows `+` add-reaction
- Hover actions (top-right, absolute): react, reply, more

**Composer (bottom, 56h):**
- `TextArea` auto-grow up to ~120h
- Left button: attach; right: emoji, send (accent, only visible when input non-empty)
- Slash commands: detect `/` prefix and show a floating popover

---

### 3.7 DMScreen
**Source:** `components/DMScreen.jsx`
**Purpose:** shown when `settings.screen === 'dms'`. Replaces channel sidebar + main.

**Two columns:**
- Left (280w): tabbed list — `direct` | `requests` | `add`
- Right: either `DMConversation` or `DMVoiceCall` (by `settings.dmSubScreen`)

**Tabs (40h top bar):** segmented control style, active tab underlined in `Theme.accent`.

**DMListItem (56h, r2 on hover):**
- 40×40 avatar with presence dot (10px, bottom-right, border `Theme.bg1`)
- Name (14px fg0) + handle (`you@server`, 11px mono fg3) — if from another server, handle gets an `accentDim` tint
- Status line (12px fg2) — sometimes with a small play icon if playing a game (toggleable by `settings.dmGameActivity`)
- Unread pill on right

---

### 3.8 DMConversation
**Source:** `components/DMConversation.jsx`

**Header (56h):** avatar + name + handle + HeaderBtns (call, video, squad-up, more).

**Cross-server fingerprint banner — DELETED, do not implement.** The mock has one; it describes a product BSFChat is not. There is no key material anywhere in this client: no device keys, no cross-signing, no Olm/Megolm, no SAS or QR verification, and therefore no fingerprint to show and nothing a "Verify in person" flow could compare. A shield reading "Verified cross-server" would be asserting a property that does not exist and cannot be computed — the worst class of security claim, because the user changes their behaviour on the strength of it.

What is actually true of a cross-server DM: the message travels over HTTPS between the two homeservers and is stored in plaintext on both. If a banner is ever wanted here, that is the sentence it gets to say. Adding a verification UI means first building verification.

(The `settings.dmFingerprint` toggle that gated this banner is deleted with it — see §2.)

**Messages:** `ChatMessageModel` with density from `settings.dmDensity`:
- `dense` — 13px, 2px message gap, avatar only on run boundary
- `standard` — 14px, 6px gap, avatar every ~3 messages
- `cinema` — 16px, 14px gap, avatar on every message, more whitespace

**Message kinds** (all in `components/DMMessages.jsx`):
- **Text** — bubble, `isMe` → accent bg, right-aligned; other → `Theme.bg2`
- **VoiceMessage** — waveform (array of bar heights) + play button + duration; playback animates progress fill
- **ScreenshotCard** — thumbnail (r2, max 420w), caption row, "Open" button
- **GameInviteCard** — accent-border panel with game title, map, server, "Join squad" accent button

**Composer (DMComposer, 64h):**
- Input + attach/emoji/voice/send
- `⚡ Squad up` button (accent, top-left of composer row) opens a small matchmaking popover

---

### 3.9 DMVoiceCall
**Source:** `components/DMVoiceCall.jsx`
**Purpose:** 1:1 call, shown when `settings.dmSubScreen === 'call'`.

Two giant avatars (132px) side by side on `Theme.bg0`, name labels, speaking rings driven by level. Bottom row of CallBtns: mute, deafen, share, video, end-call (danger, larger). Timer centered above avatars in mono, 20px fg1.

Top-right readout: `ServerConnection.voiceProtectionBadge` + ` · <ms>` in mono, fg2 — the *same* binding as the VoiceRoom badge (§3.3), hidden while the badge string is empty. **Do not hard-code the transport half of that readout**, and note that a 1:1 DM call is protected exactly like a channel call — being a two-person call confers no extra guarantee, so this readout must not say anything the group one does not. The mock's `MLS · 256 · 14ms · EU-West` is fiction on three counts: no MLS, no 256-bit group key, and no region routing.

---

### 3.10 Settings
**Source:** `components/Settings.jsx` + `SettingsPanes.jsx`
**Purpose:** cover the main region when `settings.screen === 'settings'`.

**Two-column:**
- Left (240w): nav — Audio, Voice & Activation, Video & Screen, Keybinds, Notifications, Appearance, Network, Servers, Identity, Account, About

**"Security & Keys" is deleted from that nav — do not implement it.** There are no keys to manage. BSFChat has no device keys, no cross-signing, no key backup, no per-user key material of any kind; the voice shared key (LiveKit path only) is minted and held by the server and never surfaced to or controllable by the client. A pane under that name would have to invent content, and invented content in a security pane is read as a guarantee.

If a security surface is ever wanted, the *only* thing this client can honestly show is the live call's protection state — `ServerConnection.voiceProtectionDetail`, verbatim, which already says both what it protects against and what it does not. That is a row in **Voice & Activation**, not a pane called "Keys".
- Right: scrollable pane

**Panes are listed in `SettingsPanes.jsx`.** Each pane follows the same structure:
- `SectionHeader` (title 24px fg0 + subtitle 13px fg2 + 1px divider below)
- Body of `Row`s — label on left (14px fg1, 220w column), control on right

**Reusable controls (`SettingsControls.jsx`):**
- `Select` (custom dropdown, r2 bg `Theme.bg2`)
- `Slider` (accent fill, optional live meter strip)
- `SettingsToggle` (40×22 pill switch)
- `Segment` (connected r2 row)
- `KeybindChip` — click to capture; while capturing, border pulses accent
- `InputMeter` — 6 vertical bars, animated to fake real mic levels
- `Button` — primary (accent bg), danger (danger bg), default (ghost)

**Translation notes:**
- `KeybindChip`: in Qt, capture with `Keys.onPressed` on a focused item; store `event.key` + modifiers. Use `QKeySequence` for display.
- `InputMeter`: bind bar heights to `AudioEngine.inputLevel` (a `Q_PROPERTY` updated at 20Hz).

---

### 3.11 Onboarding
**Source:** `components/Onboarding.jsx`
**Purpose:** first-run flow when no server configured.

5 steps (StackLayout):
1. **Welcome** — explains self-hosted model
2. **Auth mode** — Identity server vs Direct server (big radio cards)
3. **Credentials** — identity path shows `user@server.tld`; direct path shows `user:host:port` + "Remember this server"
4. **Connecting** — terminal-style log (`Theme.fontMono`, fg2) with animated lines: DNS → TLS → auth → sync. Every line must correspond to a step that actually ran; a log is read as a record, not as decoration. The mock's "MLS key exchange" line is removed — we don't do MLS, and faking the step is worse than omitting it. Likewise print `TLS` and only name a version if you read it back from `QSslConfiguration::sessionProtocol()`; hard-coding `TLS 1.3` states a fact about a handshake nobody inspected.
5. **Complete** — summary card: handle, latency; "Enter app" button. **No transport label here.** Onboarding finishes before any call exists, so a protection badge on this card describes nothing — `voiceProtectionBadge` is empty by design at this point, and hard-coding one would be a claim about a voice transport that has not run. The badge belongs in the call views (§3.3, §3.9) and only while a call is up.

All panels: max 520w, centered, `Theme.bg1` card on `Theme.bg0` backdrop. Title 32px semibold fg0, subtitle 14px fg2.

---

## 4 · Effects & motion

- **Shadows:** Qt 6.5+ → `MultiEffect { shadowEnabled: true; shadowBlur; shadowColor; shadowVerticalOffset }`. Shadow color + alpha comes from `Theme.shadowColor`/`Theme.shadowAlpha*` — they're softer and bluer in light mode.
- **Glow (speaking rings, accent-bordered cards):** `MultiEffect` with `shadowColor: Theme.accentGlowStrong`, `shadowBlur: 0.8`.
- **Blur backdrops:** `MultiEffect { source; blurEnabled: true; blurMax: 32 }`. Use sparingly — popovers and the onboarding backdrop only.
- **Transitions:** `Behavior on X { NumberAnimation { duration: Theme.motion.normalMs; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.motion.bezier } }`. Do not animate geometry inside ListView delegates. Using `Theme.motion.easing` (which is `Easing.BezierSpline`) without `bezierCurve` gives a linear tween — you want both.
- **Pulse:** `SequentialAnimation { loops: Animation.Infinite }` with two `NumberAnimation`s on opacity or scale.

---

## 5 · Icons

**Pre-extracted for you.** All 34 line icons are in `handoff/qt/icons/` as standalone SVGs with `stroke="currentColor"` so they tint for free:

```
annotate  at         bolt         chevron-down  chevron-right  crosshair
expand    eye        gamepad      gift          hash           headphones
headphones-off       inbox        lock          mic            mic-off
minus     paperclip  phone-off    pin           plus           screen
screen-share         search       send          settings       shield
signal    smile      users        video         volume         x
```

**Wire them up:**
1. Copy `icons/*.svg` into your Qt project (e.g. `resources/icons/`) and add to a `.qrc`.
2. Render: `Image { source: "qrc:/icons/mic.svg"; sourceSize: Qt.size(20, 20) }`.
3. Tint via `MultiEffect { colorizationColor: Theme.fg1; colorization: 1 }` (Qt 6.5+).

Alternative: reimplement as `Shape { ShapePath }` in a reusable `Icon.qml` — cleaner for recoloring but more upfront work. The SVG path data is what you'd paste in.

---

## 6 · Suggested build order

Work vertical slices, not horizontal layers. After each slice you have something runnable.

1. **Theme + one window** — `MainWindow.qml` with title bar, min size, `Theme.bg0` background, and three empty `Rectangle`s where rail/sidebar/main will go.
2. **ServerRail + ChannelSidebar** with static `ListModel`s. Get the hover/active visuals right — these set the vocabulary for everything else.
3. **VoiceRoom** with 5 fake participants (static model) and the speaking-ring effect. This validates `MultiEffect` glow and your tile layout.
4. **VoiceDock** — now the app *looks* like the mock even though nothing works.
5. **Wire `AppSettings`** for accent hue, layout variant, chatPanel toggle. Verify rebinding works.
6. **Wire the first real C++ model** (probably `VoiceMemberModel`) behind a `NetworkClient` stub that pushes fake level updates.
7. **ChatPanel + ChatMessageModel** + composer.
8. **DMScreen** (list + DMConversation) — reuses ChatPanel patterns.
9. **ScreenShare** (including annotation canvas).
10. **Settings**.
11. **Onboarding**.

---

## 7 · Common traps

- **Hex alpha:** Qt `color` accepts `#AARRGGBB` OR `#RRGGBBAA`. `Theme.qml` uses `#RRGGBBAA`.
- **Letter-spacing:** QML's `font.letterSpacing` is **pixels**, not em. Use `Theme.letterEm.widest * font.pixelSize`.
- **Avatar initial:** the mock strips leading non-alphanumerics before `charAt(0)`.
- **Don't animate inside a `ListView` delegate's geometry.** Use an inner `Rectangle` or `Item` and animate `scale`/`opacity`/`color`.
- **Font fallback:** bundle Geist via `FontLoader { source: "qrc:/fonts/Geist-Variable.ttf" }`.
- **Dark-mode-only backgrounds:** any `rgba(0,0,0,X)` overlay on a SURFACE will look wrong in light mode — use `Theme.bg*` + alpha or an inverted rgba. Overlays on GAME SCREENSHOTS or photography stay dark (captions, HUD chrome) regardless of theme.
- **On-accent text:** use `Theme.onAccent`. `#fff` will be illegible on the light-mode accent.

---

## 8 · File map

| Mock file | → | QML target |
|---|---|---|
| `BSFChat.html` (root) | | `MainWindow.qml` |
| `components/tokens.jsx` | | `Theme.qml` (provided) |
| `components/ServerRail.jsx` | | `ServerRail.qml` |
| `components/ChannelSidebar.jsx` | | `ChannelSidebar.qml` |
| `components/VoiceRoom.jsx` | | `VoiceRoom.qml` (+ `ParticipantTile.qml`) |
| `components/VoiceDock.jsx` | | `VoiceDock.qml` (+ `DockButton.qml`) |
| `components/ScreenShare.jsx` | | `ScreenShare.qml` |
| `components/ChatPanel.jsx` | | `ChatPanel.qml` (+ `MessageRow.qml`) |
| `components/DMScreen.jsx` | | `DMScreen.qml` |
| `components/DMConversation.jsx` | | `DMConversation.qml` |
| `components/DMMessages.jsx` | | `MessageRow.qml` + message kind components |
| `components/DMVoiceCall.jsx` | | `DMVoiceCall.qml` |
| `components/Settings.jsx` | | `Settings.qml` |
| `components/SettingsPanes.jsx` | | `settings/*.qml` (one per pane) |
| `components/SettingsControls.jsx` | | `controls/*.qml` |
| `components/Onboarding.jsx` | | `Onboarding.qml` |
| `components/icons.jsx` | | `resources/icons/*.svg` |
| `components/data.jsx`, `dmData.jsx` | | real C++ models — these are mock fixtures only |
| `components/TweaksPanel.jsx` | | DO NOT PORT — design-time only |

---

## 9 · Using this with Claude Code

Drop `handoff/qt/` into your Qt repo as `docs/design/`. Then:

```
> Read docs/design/SPEC.md. Port components/VoiceRoom.jsx from the mock
> to VoiceRoom.qml following SPEC §3.3. Use the Theme singleton. Bind
> to VoiceMemberModel (see roles in SPEC §2). Commit as a single file.
```

Iterate screen-by-screen. When you change the mock, regenerate this doc or patch it by hand — both are cheap.
