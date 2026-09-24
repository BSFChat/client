# Android release / Play Console runbook

Everything needed to turn this tree into something Google Play will
accept, plus the bits that must be done by hand because they involve
secrets or a browser.

Status at time of writing: the build is Play-*shaped* (correct
application id, targetSdk 36, 64-bit, .aab format, trimmed
permissions). It has **never been run on a real phone** — see
"Runtime follow-ups" at the bottom before promoting anything past
internal testing.

---

## 1. Identity: do not change these

| Thing | Value | Why it is frozen |
|---|---|---|
| Application id | `com.bsfchat.app` | Play locks it to the listing **permanently** at first upload. It cannot be renamed, only re-published as a new app with zero installs. |
| Upload key | generated once, see §3 | Play binds the upload certificate to the listing. Losing it means a support request to Google to reset it. |
| `versionCode` | derived, see §2 | Burned permanently the moment *any* track sees it, including internal testing. |

The application id is set in exactly two places, both derived from
`BSFCHAT_BUNDLE_ID` in `CMakeLists.txt`:

* `android/AndroidManifest.xml` → `package=` (Qt ≤ 6.7 / AGP 7.x)
* `QT_ANDROID_PACKAGE_NAME` target property (Qt ≥ 6.8 / AGP 8.x, where
  the manifest attribute is ignored)

CI asserts the built artefact's id with `aapt2 dump badging` and fails
the job if it is anything but `com.bsfchat.app`. Do not weaken that
check; it is the last thing standing between a typo and a permanently
wrong listing.

---

## 2. versionCode

`cmake/Version.cmake` derives it from the version string:

```
MAJOR * 1_000_000 + MINOR * 10_000 + PATCH * 100
```

so a final release always lands on a multiple of 100, and the 99-wide
band just below it carries that release's candidates:

```
0.0.51       -> 5100
0.0.52-rc.1  -> 5101
0.0.52-rc.3  -> 5103
0.0.52       -> 5200
```

That ordering is what lets an RC go to internal testing and then be
superseded by the final build. Two rules follow:

* **Never upload a `-dev.<sha>` build.** Those share the eventual
  release's code (there is no sane ordering for them), so uploading one
  burns the real release's `versionCode`. The build prints a warning
  saying so.
* More than 99 RCs for one patch is a hard error. Pass
  `-DBSFCHAT_VERSION_CODE=<n>` explicitly if you ever genuinely need to
  outrank an already-uploaded artefact without consuming a patch number.

---

## 3. What the owner must generate by hand

Claude deliberately did **not** create a keystore. Generate it
yourself, on your own machine, and keep it out of the repo:

```sh
keytool -genkeypair -v \
  -keystore ~/bsfchat-upload.jks \
  -storetype PKCS12 \
  -keyalg RSA -keysize 4096 -validity 10000 \
  -alias bsfchat-upload \
  -dname "CN=BSFChat, O=BSFChat, C=GB"
```

`keytool` prompts for the password interactively — do not pass
`-storepass` on the command line, it lands in your shell history.

Then:

1. **Back it up somewhere durable and offline.** A password manager
   attachment or an encrypted volume. If this file is lost, publishing
   updates requires a key-reset request to Google.
2. **Opt in to Play App Signing** when creating the listing (it is the
   default for new apps). Google then holds the *app signing* key and
   this one is only the *upload* key, which makes a future loss
   recoverable rather than terminal.
3. `deps/` and `*.keystore` / `*.jks` are gitignored, but the keystore
   should live outside the repo tree anyway.

### GitHub secrets to create

Repo → Settings → Secrets and variables → Actions → New repository
secret. Four of them, named exactly:

| Secret | Value |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | `base64 -i ~/bsfchat-upload.jks \| pbcopy` (macOS) |
| `ANDROID_KEYSTORE_PASSWORD` | the `-storepass` you chose above |
| `ANDROID_KEY_ALIAS` | `bsfchat-upload` |
| `ANDROID_KEY_PASSWORD` | the key password (same as store unless you set a separate one) |

`scripts/sign-android.sh` reads only these, decodes the keystore into a
temp dir, shreds it on exit, and passes every password through an
environment variable rather than argv (so it never shows up in `ps` on
a shared runner).

Behaviour without the secrets: branch and PR builds emit an **unsigned**
APK artifact and log a warning — forks get no secrets and must not turn
the job red. A **tag** build with no secrets is a hard failure, because
a tag that publishes an unsigned bundle is a release nobody can upload.

---

## 4. Play Console declarations required at first upload

None of these can be done from CI. Expect the first submission to bounce
at least once if any is missing.

* **App Bundle, not APK.** Play has required `.aab` for new apps since
  2021. CI builds one on tag pushes (`BSFChat-Android-AAB` artifact).
  The APK artifact is for sideloading only.
* **Foreground service types.** Every declared type needs a written
  justification plus, for some, a short screen recording of the feature
  in use:
  * `microphone` (VoiceService) — voice channels.
  * `camera` (VoiceService) — camera in a call.
  * `dataSync` (SyncService) — this is the one most likely to be
    challenged. Google's stated position is that `dataSync` is not for
    indefinite background work and pushes apps towards FCM. BSFChat
    deliberately does not use FCM (see the comment in
    `SyncService.java`: routing every user's push traffic through Google
    would contradict the self-hosting promise). Be ready to argue that,
    and to fall back to "sync only while the app is in the foreground /
    recently used" if the declaration is rejected.
  * `mediaProjection` (MediaProjectionService) — screen share.
* **Data safety form.** The app collects an account identifier and
  transmits message content, audio and video to a **user-chosen**
  homeserver. Declare it honestly; "data is not collected by the
  developer" is arguably true for a self-hosted server but the form asks
  about transmission too.
* **Privacy policy URL.** Mandatory for any app requesting camera or
  microphone. Needs to exist at a stable URL on BSFChat.com.
* **Sensitive permission justifications** for `CAMERA`, `RECORD_AUDIO`
  and `POST_NOTIFICATIONS`.
* **Target audience / content rating** questionnaire — a chat app with
  user-generated content and no age gate will be pushed towards a
  higher rating and may trigger the UGC policy checklist (reporting,
  blocking, moderation). Worth checking what the server side actually
  offers before answering.

---

## 5. Building locally

The local toolchain is older than CI's and that matters:

| | Local (this machine) | CI |
|---|---|---|
| Qt | 6.5.3 android_arm64_v8a | 6.10.3 |
| AGP / Gradle | 7.4.1 / 8.0 | 8.10.1 / 8.14.3 |
| compileSdk | 34 (newest platform installed) | 36 |
| NDK | 25.1.8937393 | 27.2.12479018 |

**JDK 17 is required**, not the system default. AGP 7.4.1's D8 throws
`NullPointerException` while dexing anonymous inner classes compiled by
JDK 26 — which is how a perfectly healthy C++ build fails at the very
last step with no useful message. Qt 6.10 documents JDK 17 too, so CI
pins the same.

```sh
export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
export PATH="$JAVA_HOME/bin:$PATH"

cmake -B build-android -G Ninja -Wno-dev \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Qt/6.5.3/android_arm64_v8a/lib/cmake/Qt6/qt.toolchain.cmake \
  -DQT_HOST_PATH=$HOME/Qt/6.5.3/macos \
  -DANDROID_SDK_ROOT=$HOME/Library/Android/sdk \
  -DANDROID_NDK_ROOT=$HOME/Library/Android/sdk/ndk/25.1.8937393 \
  -DCMAKE_BUILD_TYPE=Release \
  -DQT_ANDROID_ABIS=arm64-v8a -DANDROID_ABI=arm64-v8a \
  -DGAMECHAT_CLIENT_BUILD_TESTS=OFF \
  -DFETCHCONTENT_SOURCE_DIR_BSFCHAT_PROTOCOL=$HOME/dev/gamechat/protocol \
  -DBSFCHAT_VERSION=0.0.52

cmake --build build-android -j2                          # APK
cmake --build build-android --target bsfchat-app_make_aab  # AAB
```

Output lands at
`build-android/android-build/build/outputs/{apk,bundle}/release/`.

A local build produces `targetSdk 36` against `compileSdk 34`. That is
fine for sideloading and is what the checked-in APK was built with, but
it is **not** what should ever be uploaded: compiling against an older
platform than you target means Android 15/16 behaviour changes apply to
code that was never compiled against their headers. Install
`platforms;android-36` (and a Qt ≥ 6.8 Android kit) before treating a
local build as release-grade, or just let CI produce the bundle.

### Sideloading

The release APK is unsigned. To install it on your own phone, sign it
with the standard debug key:

```sh
BT=$HOME/Library/Android/sdk/build-tools/34.0.0
APK=build-android/android-build/build/outputs/apk/release/android-build-release-unsigned.apk
export DBG_PW=android
"$BT/zipalign" -p -f 4 "$APK" /tmp/aligned.apk
"$BT/apksigner" sign --ks ~/.android/debug.keystore \
  --ks-pass env:DBG_PW --ks-key-alias androiddebugkey --key-pass env:DBG_PW \
  --out BSFChat-debugsigned.apk /tmp/aligned.apk
```

A debug-signed APK installs fine after enabling "install unknown apps"
for your file manager or browser. It is *not* upgradeable to a
Play-installed build later (different signing certificate) — uninstall
first.

---

## 6. Runtime follow-ups

The list below started as "places where a build that compiles is still
most likely to misbehave". Most of them turned out to be real and are
now fixed on `fix/android-runtime`; what remains is what still needs a
phone. Structural regressions are caught by
`tests/test_android_runtime.cpp`, which scans the manifest and the Java
for the exact shapes below — it does not, and cannot, prove any of this
works on a device.

### Fixed

1. **MediaProjection consent flow.** Confirmed broken, and it could
   never have worked on Android 14+. `ScreenCaptureHelper.startCapture`
   called `startForegroundService()` and then `getMediaProjection()` in
   the same method — but `startForegroundService()` only *enqueues*
   `onStartCommand` on the main looper, so `startForeground()` always
   ran after `getMediaProjection()`, which is the exact order 14+
   answers with `SecurityException`. The consent result is now passed to
   `MediaProjectionService` as intent extras and the projection is
   opened from inside `onStartCommand`, after `startForeground()`
   returns. Consent was never cached (every start builds a fresh
   `createScreenCaptureIntent()`), so the single-use rule was already
   satisfied; the helper now holds no `Intent` field at all so it cannot
   regress. Failures report themselves through a new `nativeOnError`
   bridge into `lastError`, which VoiceDock already toasts.
2. **Foreground service start window.** `AndroidNotifier::startSyncService`
   now records the *intent* to sync and starts the service only while
   `QGuiApplication::applicationState() == Qt::ApplicationActive`,
   retrying on the next foreground transition. Its call site is
   `ServerManager::serverAdded`, which fires during a restore — before
   the activity is resumed on a cold start. Also: nothing in the tree
   ever called `stopSyncService()`, so a signed-out app kept the service
   (and the Android 15 budget burn) forever. `NotificationManager` now
   stops it when the last connection goes.
3. **`dataSync` runtime cap.** `SyncService` implements
   `onTimeout(int, int)` (API 35) and `onTimeout(int)` (API 34), stops
   itself inside the grace period — missing it is a
   `ForegroundServiceDidNotStopInTimeException` — and reports through
   `nativeOnSyncServiceStopped`. The app then degrades to
   foreground-only sync: `backgroundSyncActive` goes false,
   `backgroundSyncUnavailable` fires, and exactly one further attempt is
   made the next time the user foregrounds the app, so a recovered 24h
   window is picked up without turning a spent one into a restart loop.
   FCM remains refused. Note the overrides carry no `@Override`: the
   two-arg form does not exist below compileSdk 35 and the annotation
   would not compile locally.
4. **Camera foreground-service type.** Not on the original list, and the
   most likely crash of the lot. The manifest declares
   `microphone|camera` on `VoiceService` and the service passed both to
   `startForeground()` — but Android 14 throws `SecurityException` for
   any FGS type whose runtime permission is not held, and a user joining
   voice has granted `RECORD_AUDIO` and nothing else. Every plain voice
   join on 14+ would have taken the process down. The type set is now
   computed from `checkSelfPermission` at each `onStartCommand`, and
   `CameraController` re-triggers it when the camera actually starts.
5. **`minSdk` 23 → 28.** Nothing depended on the removed range. One
   thing was wrong in the other direction:
   `FOREGROUND_SERVICE_TYPE_MICROPHONE`/`_CAMERA` are API **30**
   constants, not 29, and were being passed on a `VERSION_CODES.Q` gate
   — so API 28/29 devices, which minSdk 28 admits, got type bits their
   platform does not define. Now gated on `R`. (`dataSync` and
   `mediaProjection` are genuinely API 29 and keep the `Q` gate.) The
   `catch (NoSuchFieldError)` guards around those constants were dead
   code: javac inlines compile-time `int` constants, so no field lookup
   ever happened at runtime.
6. **Sensor permission prompts at launch.** Observed on a device: camera
   and microphone prompts fired on the sign-in screen, before the user
   touched anything. Two separate causes, both fixed; the target is iOS,
   where the mic prompt correctly appears at first voice join. Play
   treats an unprompted sensitive-permission request as a policy
   problem, so this was a submission blocker as well as a bad first run.

   *Camera*: `CameraController`'s constructor built
   `QCamera` + `QMediaCaptureSession`, which brings the platform camera
   up. Now lazy on Android/iOS, created at the first `startForCamera()`
   — after VoiceDock has asked for CAMERA. (`Settings` likewise no
   longer constructs `QMediaDevices` eagerly, though that turned out not
   to be the prompt: `QMediaDevices`' constructor only connects signals.)

   *Microphone*: listing audio inputs asks for the microphone by
   construction, which is worth spelling out because it is not
   obvious —

   ```
   QMediaDevices::audioInputs()
     -> QOpenSLESEngine::availableDevices(Input)
        -> QOpenSLESDeviceInfo(..., Input)      [the CONSTRUCTOR]
           -> supportedSampleRates(Input)
              -> checkSupportedInputFormats()
                 -> inputFormatIsSupported()
                    -> requestPermission(RECORD_AUDIO)
   ```

   Qt probes thirteen sample rates by opening a real AudioRecorder, and
   it cannot do that unasked. The caller was
   `ClientSettings.qml`'s input combo — `MobileMain` instantiates that
   Popup eagerly, so `model: appSettings.audioInputDevices` evaluated at
   QML load, ~450ms into a cold start. The guard went into
   `Settings::audioInputDevices()` rather than the QML, so any future
   binding or caller is covered too: with no microphone permission the
   list is just "System default" (you cannot pick an input you may not
   open), and the real devices appear once a voice join has asked
   properly. `checkPermission()` does not prompt. Audio OUTPUTS are
   deliberately NOT gated — `QOpenSLESDeviceInfo` only probes for
   `Mode::Input`.
7. **OIDC redirect `launchMode`.** Was `singleTop`, now `singleTask`.
   `singleTop` only routes to `onNewIntent` when the instance is already
   on top of the *same* task; a browser redirect carries
   `FLAG_ACTIVITY_NEW_TASK`, and a second task was observed being
   created while the app was already running. A second instance has no
   PKCE verifier — it is memory-only, deliberately — so the sign-in
   would fail its CSRF check or hang. `singleTask` guarantees one
   instance, which is a property Qt requires anyway (one
   `QGuiApplication` per process).
8. **Process-fatal JNI edges.** `BSFChatActivity`'s forwards threw
   `UnsatisfiedLinkError` if an intent arrived before the native bridges
   were registered, and indexed `grantResults` against
   `permissions.length` — `grantResults` comes back *empty* when a
   permission dialog is cancelled, which also left the QML continuation
   waiting forever. Both fixed. Every `startForeground()` is now inside
   a `try`.
9. **Rotation.** Not a bug: `configChanges` already carries Qt's full
   template list, so a rotate does not recreate the activity. Added
   `fontWeightAdjustment` (API 31) — the accessibility "Bold text"
   toggle is a configuration change Qt's list predates, and it *did*
   recreate the activity.
10. **Wake lock.** `VoiceService` acquired a one-hour bounded partial
    wake lock with a comment saying it was "renewed below". Nothing
    renewed it, so a call past an hour lost it silently. Now renewed
    every 30 minutes and cancelled in `onDestroy`.
11. **`allowBackup` `true` → `false`.** Unchanged and still right; a
    user restoring a phone has to sign in again. Worth a release note.

### Still needs a phone

* **Everything above.** None of it has been observed working on
  hardware. The structural tests prove the shapes, not the behaviour.
* **The real Chrome redirect.** `singleTask` is the standard answer and
  the reasoning is sound, but the observation that prompted it came from
  an `am start`, which adds `NEW_TASK` itself. Worth confirming against
  an actual browser round trip.
* **Cold-start sign-in.** If the process dies while the browser has the
  foreground, the redirect relaunches us and the PKCE verifier is gone
  with the old process.
  `IdentityClient::deliverCallbackUrl` consumes the callback with a
  warning rather than mistaking it for a room link — correct, but
  *silent*: the user lands back on the sign-in screen with no
  explanation. Needs a line of UI, which belongs with whoever owns the
  mobile sign-in QML.
* **POST_NOTIFICATIONS denial.** The foreground service is not killed
  when the permission is denied on 13+ — the notification is suppressed
  and the service runs. That is the documented behaviour, unverified
  here. The voice-join path already toasts "Allow notifications to keep
  voice running in the background"; the sync path has no equivalent, but
  `AndroidNotifier::backgroundSyncUnavailable` is now there for a UI to
  bind to.
* **External room deep links are not claimed on Android.** The
  intent-filter is host-scoped to `oauth` on purpose, so
  `bsfchat://room/...` opened from a browser does nothing here, unlike
  desktop and iOS. In-app notification taps are unaffected — they use an
  explicit intent. Deliberate, but it is a platform difference, not a
  universal truth.
* **`screenOrientation="unspecified"` and small-screen layout.** Left
  alone; cross-platform mobile QML is somebody else's change.
* **API 28/29 devices.** The gates are right on paper. Nobody has run
  the app on one.
