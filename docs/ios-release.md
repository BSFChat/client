# Shipping BSFChat on iOS

Everything in this file is work that has to happen **in a browser, in
Josh's Apple Developer account**. Nothing here can be automated from the
repo, and none of it has been done yet.

The automated half lives in `.github/workflows/ci.yml` (the `ios` job),
`ios/Info.plist.in`, `ios/ExportOptions.plist.in` and the `if(IOS)`
branches of `CMakeLists.txt`.

---

## 1. One-time setup in the Apple Developer account

### 1.1 Program membership

Apple Developer Program, **Organization** or **Individual**, $99/yr. An
Individual account publishes under a personal legal name; an
Organization account publishes under a company name and needs a D-U-N-S
number. Whichever is chosen is the seller name on the store listing and
is painful to change later.

### 1.2 Bundle identifier

Register **`com.bsfchat.app`** at
*Certificates, Identifiers & Profiles → Identifiers → App IDs → App*.

It must be exactly this string. It is what `CMakeLists.txt` produces for
a Release build (`BSFCHAT_BUNDLE_ID`), what the `ios` CI job passes
explicitly, and what the job's "Verify archived Info.plist" step fails
on if it ever drifts. It is also the same id the macOS build already
ships under, which is correct and intended — one app identity across
platforms.

**Capabilities to enable on the App ID: none.** The current feature set
needs no entitlement beyond the implicit ones. Specifically *not*
needed: Push Notifications (no APNs anywhere in the client), Associated
Domains (invite links use the `bsfchat://` scheme, not universal
links), App Groups, iCloud, Sign in with Apple. Turning one on
unnecessarily invalidates the provisioning profile and buys nothing.

### 1.3 Signing certificate

Create an **Apple Distribution** certificate and export it as a `.p12`
with a password.

This is *not* the Developer ID certificate the macOS job already uses.
Developer ID signs software distributed outside the store; it cannot
sign an App Store build, and the CI job fails with an explicit error if
the imported `.p12` has no `Apple Distribution` identity.

### 1.4 Provisioning profile

**Nothing to do.** The CI job passes `-allowProvisioningUpdates` with an
App Store Connect API key, so Xcode creates and downloads the App Store
profile itself. A hand-managed `.mobileprovision` expires every twelve
months and fails in a way that looks like a signing bug; this has
nothing to rotate.

### 1.5 App Store Connect API key

*App Store Connect → Users and Access → Integrations → App Store
Connect API → Team Keys → generate.* Role: **App Manager** (Developer is
not enough to upload builds).

Note the **Key ID** and the **Issuer ID** from that page. Download the
`.p8` — **it can only be downloaded once**, so keep a copy somewhere
safe before doing anything else with it.

### 1.6 The app record

*App Store Connect → Apps → + → New App.* Platform iOS, bundle id
`com.bsfchat.app`, SKU anything stable (`bsfchat-ios`), primary language
English.

The record has to exist before the first upload; Transporter has
nowhere to put a build otherwise.

---

## 2. GitHub secrets the `ios` job reads

Six secrets, all under *Settings → Secrets and variables → Actions* on
`BSFChat/client`. Without them the job still compiles the iOS binary
(which is the regression guard) and skips archive, export and upload —
it does not fail.

| Secret | What it is | How to produce it |
| --- | --- | --- |
| `APPLE_TEAM_ID` | 10-character Team ID | Top right of the Apple Developer site, or *Membership details* |
| `IOS_DIST_CERTIFICATE` | Apple Distribution cert, base64 | `base64 -i AppleDistribution.p12 \| pbcopy` |
| `IOS_DIST_CERTIFICATE_PWD` | Password set when exporting that `.p12` | — |
| `ASC_KEY_ID` | App Store Connect API Key ID | From §1.5 |
| `ASC_ISSUER_ID` | App Store Connect Issuer ID | From §1.5 |
| `ASC_PRIVATE_KEY_BASE64` | The `.p8`, base64 | `base64 -i AuthKey_XXXXXXXXXX.p8 \| pbcopy` |

`MACOS_CERTIFICATE` and the notarisation secrets are unrelated and stay
as they are.

---

## 3. Export compliance — do this once, before the first upload

`ios/Info.plist.in` declares `ITSAppUsesNonExemptEncryption = true`.
The long comment there explains why; the short version is that the
voice path uses DTLS-SRTP from libdatachannel built against an OpenSSL
we cross-compile ourselves, which is encryption Apple's OS does not
provide, so the "only uses the operating system's HTTPS" exemption does
not apply.

Consequences, in order:

1. App Store Connect will ask for encryption documentation on the first
   submission. BSFChat uses **industry-standard, non-proprietary**
   algorithms (TLS, DTLS-SRTP, AES), not proprietary ones, so **no
   CCATS is required** — only the **French encryption declaration**, and
   only if the app is distributed in France (it will be, unless France
   is deselected in the territory list).
2. Apple reviews that documentation once and issues a code. Put it in
   `ios/Info.plist.in` as `ITSEncryptionExportComplianceCode` and the
   questionnaire never appears again, on any version.
3. Because the answer is "non-exempt **with** documentation provided to
   Apple", the separate BIS year-end self-classification report is *not*
   additionally required. (It is the *exempt* path that can still owe
   one.)

**Update 2026-09-23: this is no longer hypothetical.**
`BSFCHAT_ENABLE_VOICE` now defaults **ON** for iOS
(`CMakeLists.txt:57-59`) and voice has run on a real iPhone, so the
shipped binary genuinely does link DTLS-SRTP against our own OpenSSL.
The declaration was written to be true in advance of that day; it is now
true in the ordinary way. Leave it `true`.

Two things to be clear-eyed about before the first upload:

- **`true` does not make the questionnaire go away — `false` is what
  does that.** An earlier comment in `ios/Info.plist.in` had this
  backwards and has been corrected. Expect to answer the export
  questions once for the first version, and expect the build to sit in
  "Missing Compliance" in App Store Connect until you do. That state
  blocks external TestFlight testers; it does not block internal ones,
  so you can start testing while you sort it out.
- Point 3 above is the reading this project is going on, not a
  certainty. If in doubt, the belt-and-braces move is to file the BIS
  year-end self-classification report anyway — it is an email, it costs
  nothing, and it is the cheapest possible resolution of an argument
  with an export-control regulator.

---

## 4. What a release looks like

1. Merge to `main`. The `ios` job archives nothing on a branch push — it
   compiles, and uploads the `.ipa` as a CI artifact if signing secrets
   are present.
2. Wait for main's CI to be green (all three desktop platforms **and**
   now iOS) before tagging — the existing rule, unchanged.
3. Push `vX.Y.Z` or `vX.Y.Z-rc.N`. The `ios` job archives, exports and
   uploads to TestFlight via `iTMSTransporter`.
4. TestFlight processes the build (10–60 minutes), then it appears under
   *TestFlight → iOS builds*.
5. Internal testing needs no review. **External** testing needs a
   Beta App Review, which is a lighter version of the real thing but
   checks the same 1.2 / 5.1.1 items in §5.

`CFBundleShortVersionString` comes from the tag; `CFBundleVersion` is
the GitHub Actions run number, which is monotonic and never reused —
this matters because a build number is burned permanently even by an
upload that is later rejected.

---

## 5. Before the first submission — App Review items

These are product gaps, not configuration, and none of them can be
closed from this repo alone. They are ordered by how likely they are to
cause a rejection.

### 5.1 Guideline 1.2 — user-generated content

An app where users post content to each other must provide **a way to
report objectionable content** and **a way to block an abusive user**,
plus a published method of contacting the developer.

The client has neither today. Server-side blocking/reporting is being
built separately; the client needs the UI on top of it — at minimum a
"Report message" entry in the existing `MessageBubble` context menu and
a "Block user" entry in the member context menu.

This is the single most common rejection for chat apps and it is
checked every time.

### 5.2 Guideline 5.1.1(v) — account deletion

An app that lets users create an account must let them **delete** it
from inside the app, not merely sign out. The client creates accounts
two ways (password registration and implicit OIDC first-sign-in) and
offers only "Sign out", which removes local credentials.

Needs a real account-deletion path — server endpoint plus a confirmable
entry in per-server user settings.

### 5.3 Guideline 2.1 — the reviewer must be able to sign in

Give App Review a **demo account on a public BSFChat server** in the
App Review Information notes: server URL, username, password. Use the
password login path, not OIDC — the reviewer will not enrol in an
identity provider, and see §5.4 for why the OIDC path may not work on
their device at all.

Also put a sentence in the review notes explaining that BSFChat is
self-hosted and the demo server is one instance of it, or the reviewer
may treat the empty server field as a broken first-run.

### 5.4 The OIDC sign-in flow is desktop-shaped and will not work on iOS

`src/identity/IdentityClient.cpp` implements the RFC 8252 *loopback*
redirect: it opens a `QTcpServer` on `127.0.0.1`, sends the user to
Safari with `QDesktopServices::openUrl`, and waits for the browser to
call back into that local port. On iOS the app is suspended once Safari
takes the foreground, and a suspended process does not accept
connections — the callback is lost and sign-in hangs on "Waiting for
browser login…".

The fix is `ASWebAuthenticationSession` with a **custom-scheme**
redirect (`bsfchat://oauth/callback`), which is why `CFBundleURLTypes`
is declared in `ios/Info.plist.in`. It also needs an iOS branch in
`src/core/UrlHandler.cpp`, which currently has Android `#ifdef`s and no
iOS path, and the redirect URI registered on the identity server.

Until that lands, **the only sign-in that works on iOS is username +
password**, which is another reason the demo account must use it.

### 5.5 Guideline 4.8 — not triggered

4.8 applies when an app offers a *third-party* login service. BSFChat
offers none: there is no Google/Facebook/Apple/GitHub button anywhere,
and the OIDC provider is always either `id.bsfchat.com` (first-party) or
whatever the homeserver the user typed advertises. Sign in with Apple is
therefore **not** required.

Worth writing into the review notes anyway, because "Sign in with
BSFChat ID" reads like a third-party button at a glance.

### 5.6 Screen sharing is absent on iOS, by design

Screen sharing exists on desktop (ScreenCaptureKit / `QScreenCapture`)
and Android (MediaProjection). On iOS it requires ReplayKit plus a
**Broadcast Upload Extension** — a second binary target with its own
bundle id, its own provisioning profile, an App Group to pass frames
back, and a hard ~50 MB memory ceiling in the extension process.

The UI already degrades correctly: `qml/components/VoiceDock.qml` gates
the button on `typeof screenShare !== "undefined"`, the context property
is never set on iOS (`src/main.cpp`), so the control is simply absent
rather than present-and-broken. A reviewer sees no screen-share button
and nothing to fail. **No work is needed here before submission.**

Rough scope if it is ever wanted: 2–3 weeks for one person — extension
target and CMake plumbing (3–4 days), App Group + frame IPC (2–3 days),
sample-buffer → encoder bridge reusing the existing H.264 pipeline
(4–5 days), memory-budget work inside the extension (3–5 days, the part
that actually bites), UI and the `RPSystemBroadcastPickerView` entry
point (2 days). It is a genuinely separate project, not a flag.

---

## 6. Privacy nutrition label answers

Filled in once per app record under *App Privacy*. BSFChat has **no
telemetry, no analytics, no crash reporter and no third-party SDK that
phones home** — verified by grep across the client — which makes almost
all of this "not collected".

**Data collected and linked to the user's identity, used for App
Functionality only, never for tracking:**

| Category | What | Why |
| --- | --- | --- |
| Contact Info → Name | Display name, nickname | Shown beside the user's messages |
| User Content → Other | Messages, attachments, avatars, reactions | The product |
| User Content → Audio/Video | Voice and camera streams | Only when voice is enabled — **not in the current iOS build** |
| Identifiers → User ID | Matrix account id, device id | Authentication and multi-device |

**Answer "No" / not collected for:** Analytics, Product Interaction,
Advertising Data, Purchases, Financial Info, Location (precise or
coarse), Contacts, Health, Sensitive Info, Browsing History, Search
History, Diagnostics, Crash Data, Performance Data, Device IDs
(no IDFA, no `identifierForVendor`, no hardware identifiers anywhere),
Email Address, Phone Number, Physical Address.

**"Used to Track You": No, for everything.** There is no ad network, no
attribution SDK and no data sharing with anyone.

Two honest footnotes worth having ready:

- Data goes to **the server the user chooses**, which in a self-hosted
  product is frequently the user's own machine. Apple's questionnaire
  has no category for that; answer as though the operator is a third
  party, because sometimes they are.
- Crash reports reach Apple via TestFlight/App Store symbolication
  (`uploadSymbols` in `ios/ExportOptions.plist.in`). That is Apple
  collecting them, not us, and it is not declarable as our collection.
  It is also the *only* diagnostic channel that exists, precisely
  because the client ships no crash reporter.

---

## 7. Known iOS gaps that are not blockers but will be noticed

Ordered by how visible they are to a reviewer holding a phone.

1. **Safe-area insets are not handled.** `qml/mobile/MobileMain.qml`
   hardcodes `topInset: 0` (with a comment explaining it was measured on
   Android) and declares a `bottomInset: 16` that nothing reads. On any
   iPhone with a notch or Dynamic Island the 48 pt header renders under
   the status bar. Qt 6.9+ has the `SafeArea` attached property and
   `QQuickWindow.safeAreaMargins`; the Qt pin is now 6.10.3 everywhere,
   so the API is available.
2. **No on-screen-keyboard avoidance.** The strategy is Android's
   `windowSoftInputMode=adjustResize`, which has no iOS equivalent. The
   `keyboardHeight` property in `MobileMain.qml` is computed and then
   never used — the `Binding` its own comment promises does not exist.
   Expect the composer to sit behind the keyboard.
3. **Context menus are right-click only outside the message list.**
   `MessageBubble` has a real `TapHandler.onLongPressed` path; channel
   rows, categories, the member list and the server rail do not, and
   there is no `onPressAndHold` anywhere else in the tree. Those menus
   are unreachable by touch on iOS. (Android gets away with it because
   Qt's Android QPA synthesises a right-click from a long press.)
4. **Touch targets below 44 pt** on controls that are reachable on
   mobile: the three primary buttons of the mobile header (40×40,
   `MobileMain.qml`), the composer attach and emoji buttons (40),
   member-list rows (40), `ThreadPanel` header buttons (28),
   `ClientSettings` close (28). The composer send button, channel rows
   and server rail tiles are already correct at 44.
5. **Hover-gated controls with no touch equivalent** in
   `ServerSettings.qml` (category and channel action buttons),
   `ChannelList.qml` (category header), `MessageView.qml` (pinned-row
   actions) and `UserSettings.qml` (the change-avatar overlay). Admin
   surfaces mostly, but `UserSettings` is on the mobile overflow menu.
6. **`ClientSettings` and `ServerSettings` are unmodified desktop
   panes** shown in a popup sized to the viewport. `ClientSettings` has
   a fixed 180 pt nav column, which leaves ~157 pt of content on a
   375 pt phone.
7. **Haptics are a no-op on iOS.** Every method in
   `src/core/Haptics.cpp` is wrapped in `#ifdef Q_OS_ANDROID`, so the
   long-press feedback the mobile UI asks for silently does nothing.
   `UIImpactFeedbackGenerator` is the iOS counterpart.
8. **Voice and video are compiled out.** `BSFCHAT_ENABLE_VOICE` defaults
   `OFF` for iOS in `CMakeLists.txt`. The first iOS build is a
   text-chat client — which is a coherent thing to ship, but the store
   listing and screenshots must not promise voice, or it is a 2.3.1
   ("accurate metadata") rejection.
