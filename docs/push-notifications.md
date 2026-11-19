# Push notifications (Android FCM, iOS APNs)

How a message on a BSFChat server becomes a notification on a phone that is
not running BSFChat, what Google and Apple get to see on the way, and what
the owner and the integrator have to set up.

## Architecture

```
chat server ──POST /_matrix/push/v1/notify──► Sygnal (push.bsfchat.com)
 (PushService,                                  │
  one pusher per                                ├─ app_id com.bsfchat.app     → FCM (HTTP v1) → Android
  account+device)                               └─ app_id com.bsfchat.app.ios → APNs          → iOS
```

* **Client → server.** For every signed-in account the client registers a
  Matrix HTTP pusher (`POST /_matrix/client/v3/pushers/set`) whose `pushkey`
  is this device's FCM registration token (Android) or APNs device token as
  lowercase hex (iOS), and whose `data.url` is the gateway. Code:
  `src/net/PushRegistrar.*` (state machine, wire format, tests in
  `tests/test_push_registrar.cpp`), `src/core/PushManager.*` (glue to
  settings and connections), `src/core/PushBridge.*` (platform end).
* **Server → gateway.** The BSFChat server decides who gets pushed
  (`server/src/push/PushService.cpp`: DMs and mentions by default, the per-room
  `notify_level`, blocks) and POSTs the Matrix notify payload to the URL of
  each pusher — only if that URL is in its `push.allowed_gateway_prefixes`.
* **Gateway → Google/Apple → device.** Sygnal holds the FCM service account
  and the APNs key; the chat server holds no provider credentials at all.

### Privacy

The payload tier is chosen by the client per pusher, in `data.format`
(server `core/Config.h`, `push_payload`):

| Setting | `data.format` sent | What reaches Google/Apple |
| --- | --- | --- |
| default (off) | `"event_id_only"` | room id, event id, unread count, priority. No sender, no room name, no text. |
| "Show sender and room names in notifications" | `"com.bsfchat.names"` | the above + event type, sender id, sender display name, room name. Never the text. |

The client always sends `format` explicitly rather than omitting it: an
omitted format means "the operator's `push.default_payload`", which an
operator may have raised, and an explicit request always wins on the BSFChat
server. The client never asks for `com.bsfchat.full`. A server whose
`push.max_payload` is below "names" refuses that tier at `/pushers/set`; the
client falls back to `event_id_only` for that server and says so in
Settings > Notifications.

Room ids are `!opaque:server_name`, so the server's name is visible to the
gateway and to Google/Apple in every tier. The device token itself is shared
with the gateway operator (BSFChat), and with Google/Apple, who issued it.

On the device the notification text is **"New message"** (or "N unread
messages"), titled "BSFChat", unless the user opted into names. The lock
screen shows only "BSFChat / New message" in either case (Android
`VISIBILITY_PRIVATE` with a public version; iOS per the user's preview
setting).

### Fetching the real content on the device

* **Android, app process alive** (backgrounded, not killed): the FCM service
  hands the event to C++, which kicks that server's own `/sync`. If the event
  arrives within ~7 s, the regular notification path shows it with sender and
  text from local data — none of which went through Google — and applies the
  room's notification mode. Otherwise the FCM service posts the generic one.
  The service thread waits for the answer (up to 8 s of Firebase's ~10 s
  budget) so Android does not freeze the process mid-fetch.
* **Android, process not running:** the generic notification is posted. A
  cold-process fetch would need the session token outside Qt's keychain
  wrapper; not built.
* **iOS:** the system shows the gateway's alert as is. Content fetching needs
  a **Notification Service Extension** (separate app-extension target, a
  shared keychain group for the session, its own provisioning profile). Out of
  scope for now; the pusher's `default_payload` already sets
  `mutable-content: 1` so an extension can be added without a server or
  gateway change.

### Background connection vs push (Android)

Before push, Android kept a persistent `dataSync` foreground service
(`SyncService`) to hold `/sync` open. Now:

| Build / device | Default |
| --- | --- |
| FCM configured, Google Play services present | **push**; SyncService off |
| FCM configured, no Play services (token request fails) | SyncService (automatic fallback) |
| FCM not configured at build time | SyncService, as before (no setting shown) |

Settings > Notifications > "Keep a background connection instead of push
(uses more battery, no Google)" restores the old behaviour and also stops
Firebase and deletes the FCM token. Firebase auto-init is off in the
manifest, so nothing contacts Firebase until the app wants a token.

### When nothing is pushed

* A self-hosted server that does not list the gateway in
  `push.allowed_gateway_prefixes` refuses the pusher. The client logs it,
  shows "… does not allow this push gateway, so it can only notify you while
  BSFChat is open" in the push status row, and does not retry until the
  gateway URL changes.
* Notifications turned off in BSFChat: every pusher is deleted.
* Sign-out / remove server: the pusher is deleted on the session before the
  logout request (the server also drops pushers at logout).
* Token rotation: the new pusher is registered first, then the old one
  deleted; a rotation that happened while the app was closed is caught from
  the last registered key kept in settings.

## Exact pusher bodies

Android (release build), default tier:

```json
{
  "kind": "http",
  "app_id": "com.bsfchat.app",
  "pushkey": "<FCM registration token>",
  "app_display_name": "BSFChat",
  "device_display_name": "Google Pixel 8",
  "lang": "en-GB",
  "data": {
    "url": "https://push.bsfchat.com/_matrix/push/v1/notify",
    "format": "event_id_only"
  }
}
```

iOS (release build), default tier:

```json
{
  "kind": "http",
  "app_id": "com.bsfchat.app.ios",
  "pushkey": "<APNs device token, lowercase hex>",
  "app_display_name": "BSFChat",
  "device_display_name": "iPhone",
  "lang": "en-GB",
  "data": {
    "url": "https://push.bsfchat.com/_matrix/push/v1/notify",
    "format": "event_id_only",
    "default_payload": {
      "aps": {
        "alert": { "title": "BSFChat", "body": "New message" },
        "sound": "default",
        "mutable-content": 1
      }
    }
  }
}
```

With names on, `format` is `"com.bsfchat.names"`; nothing else changes.
Deletion: `{"kind": null, "app_id": "<same>", "pushkey": "<old token>"}`.
`append` is omitted (false): the token belongs to this account alone.

`app_id` follows the bundle id: Debug builds (bundle id
`com.bsfchat.app.dev`) register as `com.bsfchat.app.dev` /
`com.bsfchat.app.ios.dev`, which the production gateway does not know, so
Debug builds get no pushes unless the gateway gets entries for them (and,
for Android, a Firebase app registration for the `.dev` package).

### Gateway behaviour the client relies on (Sygnal, from its source)

* **FCM (`gcm` pushkin, `api_version: v1`)**: sends a data message whose data
  are the notification fields flattened: `room_id`, `event_id`, `prio`,
  `unread` (from `counts.unread`), and for names `type`, `sender`,
  `sender_display_name`, `room_name`; all values as strings. The Android
  client reads exactly those and treats `"None"`/`"null"` as absent.
* **APNs**: a notification with an `event_id` but no `type` (our
  `event_id_only`) takes Sygnal's event-id-only path: the APNs payload is
  `default_payload` plus top-level `room_id`, `event_id`, `unread_count`. A
  notification with a `type` (names tier) takes the full path: an
  `aps.alert` with `loc-key`/`loc-args` (`MSG_FROM_USER_IN_ROOM` etc.,
  translated by `ios/en.lproj/Localizable.strings`), `aps.badge` = unread,
  sound for high priority, and the same top-level `room_id` / `event_id`.
  A tap opens the room from `room_id`.
* **Badge on iOS**: set by APNs only in the names tier (Sygnal's full path).
  In the default tier Sygnal puts the count in `unread_count`, which only a
  Notification Service Extension could turn into a badge. While running, the
  app reconciles the badge itself: `PushManager` sets it to the sum of every
  room's unread count whenever that changes, including back to 0.
* **`mutable-content` in the names tier is uncertain.** Our
  `default_payload` (which sets `mutable-content: 1`) is only what Sygnal's
  event-id-only path sends. The names tier takes Sygnal's full path, which
  builds its own `aps` and, from its source, does not carry
  `mutable-content` (or anything else from `default_payload`). Today that
  costs nothing: there is no Notification Service Extension. Whoever adds
  one must check a live names-tier payload first; if the flag is missing,
  the extension simply never runs for names-tier pushes (they still show
  Sygnal's localised alert), and the fix is on the gateway side.

These are assumptions about Sygnal written from its source, not tested
against a live gateway here. If the deployed Sygnal differs, the symptoms are:
an iOS default-tier push with no visible alert (event-id-only path not taken),
or an Android notification that opens the app but not the room (field
names).

## Owner setup

### Firebase (Android)

1. Firebase console → project **bsfchat-5074b** → Add app → Android, package
   **`com.bsfchat.app`**. (Done: app id `1:774185082041:android:50c7681a99ec9665d54f68`.)
2. Read the four values off Project settings → General (or the downloaded
   `google-services.json`; the file itself is not used):

   | GitHub repository variable | Where | Value |
   | --- | --- | --- |
   | `FCM_PROJECT_ID` | `project_info.project_id` | `bsfchat-5074b` |
   | `FCM_APP_ID` | `client[].client_info.mobilesdk_app_id` | `1:774185082041:android:50c7681a99ec9665d54f68` |
   | `FCM_SENDER_ID` | `project_info.project_number` | `774185082041` |
   | `FCM_API_KEY` | `client[].api_key[].current_key` (`AIza…`) | *pending* |

   Set them under GitHub → Settings → Secrets and variables → Actions →
   **Variables** (not secrets — they are public client config and end up in
   the APK anyway). CI passes them to CMake as `BSFCHAT_FCM_*`; a local
   build passes `-DBSFCHAT_FCM_PROJECT_ID=…` etc. or sets the same-named
   environment variables. With any of the four missing the build has no push
   (CI prints a warning) and behaves as before.
3. In Google Cloud console, restrict the API key to the Android app
   (package `com.bsfchat.app` + the **Play app-signing** SHA-1 from Play
   Console → App integrity, plus the upload key's SHA-1 for sideloaded CI
   APKs) and to the Firebase Installations and FCM Registration APIs.
4. Project settings → Service accounts → generate a key for the gateway
   (FCM HTTP v1). That JSON **is** a secret; it goes to the integrator for
   Sygnal only.

### APNs (iOS)

1. Apple Developer → Identifiers → `com.bsfchat.app` → enable **Push
   Notifications** → Save, then regenerate the App Store provisioning profile
   (keep the distribution certificate that matches `IOS_DIST_CERTIFICATE`).
   Until this is done the `ios` CI job stops at the profile check with a
   message naming the capability: `ios/BSFChat.entitlements` now asks for
   `aps-environment = production`. (Escape hatch: `-DBSFCHAT_IOS_PUSH=OFF`
   builds without push; the CI check reads the checked-in file, so it would
   also need that key dropped.)
2. Keys → create an **APNs auth key** (.p8), note its Key ID and the Team ID.
   Secret; to the integrator for Sygnal.

### Sygnal (integrator, push.bsfchat.com)

```yaml
apps:
  com.bsfchat.app:
    type: gcm
    api_version: v1
    project_id: bsfchat-5074b
    service_account_file: /etc/sygnal/fcm-service-account.json
  com.bsfchat.app.ios:
    type: apns
    keyfile: /etc/sygnal/AuthKey_<KEYID>.p8
    key_id: <KEYID>
    team_id: <TEAMID>
    topic: com.bsfchat.app
    platform: production
```

Exposed as `https://push.bsfchat.com/_matrix/push/v1/notify`. Sygnal logs
should not be configured to record request bodies.

### Chat servers

Every server that should push to BSFChat's apps needs, in its config:

```toml
[push]
enabled = true
allowed_gateway_prefixes = ["https://push.bsfchat.com/"]
```

The official server's deployment gets this from the integrator. **Self-hosted
servers must add it themselves**; without it the client shows the server as
"does not allow this push gateway" and only notifies while the app is open
(Android users can turn on the background connection for that). Operators who
run their own gateway can point users at it in Settings > Advanced > Push
gateway — but a different gateway needs its own FCM/APNs credentials for the
BSFChat app ids, which in practice only BSFChat has.

## Build variables

| Variable | Platform | Default | Effect |
| --- | --- | --- | --- |
| `BSFCHAT_FCM_PROJECT_ID`, `_APP_ID`, `_API_KEY`, `_SENDER_ID` | Android | unset | all four set → FCM compiled in (`cmake/AndroidPush.cmake`) |
| `BSFCHAT_FCM_MESSAGING_VERSION` | Android | `24.1.1` | `com.google.firebase:firebase-messaging` version |
| `BSFCHAT_IOS_PUSH` | iOS | `ON` | APNs code, `aps-environment` entitlement, `Localizable.strings` |

Android without the google-services plugin: `AndroidPush.cmake` writes
`PushConfig.java` and `res/values/bsfchat_push.xml` (the string resources the
plugin would have generated, so Firebase's own init provider configures the
default app on a cold start caused by a push), stages
`android-push/fcm/*.java` (or the stub), adds the messaging service to the
manifest, and appends the dependency to a copy of Qt's `build.gradle`
template.

### Status line, multiple accounts and failures (audit 3)

* **Android background connection per server.** `SyncService` runs not
  only when the user chooses it or push is unavailable, but whenever any
  signed-in server has no registered pusher: it refuses the gateway (the
  default for a self-hosted server), refuses the pusher, keeps failing, or
  the gateway URL is invalid; and while the FCM token is being retried.
  The status line names the servers that rely on it. The decision and the
  text are `summariseStatus()` in `src/net/PushRegistrar.cpp`, unit-tested.
* **Two accounts, one homeserver.** Matrix `pushers/set` without
  `append: true` removes other users' pushers with the same app id and
  pushkey. The registrar sends `append: true` when another account of this
  device is on the same homeserver, and re-registers a sibling that an
  earlier `append: false` request may have evicted. A lone account keeps the
  default, so a sign-out whose deletion failed still stops delivery here.
* **FCM token failures** are retried with backoff (10 s up to 30 min);
  only a device without FCM or a broken Firebase config is final. After six
  failures the native side is told push is unavailable, but the slow retry
  continues. Firebase is not contacted until the first server is added.
* **Blocked notifications** (Android 13+ permission denied, iOS alerts
  denied) are reported as such, with a way to the system settings.
* **Gateway URL**: must be `https://`; `http://` only for localhost.

## Not verified here

No device, emulator, Gradle or Xcode build was run for this change: the C++ was
syntax-checked (Android/iOS branches with stubbed platform headers where
possible), Java and Objective-C++ were reviewed by reading. To hand-test:
token registration on both platforms, a push with the app killed / in the
background / in the foreground, tap-to-room on cold and warm start, the
names tier, token rotation (clear app data), sign-out deleting the pusher,
and a server without the gateway in its allowlist.
