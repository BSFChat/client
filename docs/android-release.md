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

## 6. Runtime follow-ups (not verified on hardware)

Nothing below has been observed on a device. They are the places where
an Android build that compiles is still most likely to misbehave, in
rough order of how likely they are to bite.

1. **MediaProjection consent flow.** `ScreenCaptureHelper` launches the
   consent intent and converts the result into a token. From Android 14
   the consent is single-use: every new projection needs a fresh
   `createScreenCaptureIntent()`, and reusing a token throws
   `SecurityException`. Also verify
   `MediaProjectionService.startForeground(..., mediaProjection)` runs
   **before** `getMediaProjection()` — the reverse order throws on 14+.
2. **Foreground service start window.** `SyncService` is started from
   C++. If that ever happens while the app is backgrounded, Android 12+
   throws `ForegroundServiceStartNotAllowedException` and kills the
   process. Needs checking against the app's actual
   start/stop call sites.
3. **`dataSync` runtime cap.** Android 15 caps `dataSync` foreground
   services at roughly 6 hours per 24, after which the system stops
   them. A long-lived `/sync` will hit this. There is currently no
   handling for the stop callback.
4. **POST_NOTIFICATIONS flow.** The permission is requested lazily
   before first voice-join. If it is denied, the foreground service
   still starts but posts no visible notification — on Android 13+ that
   is allowed, but verify the service is not silently killed and that
   the UI says something useful rather than failing mutely.
5. **`minSdk` moved 23 → 28.** Nothing in the tree obviously depends on
   API 23-27, but it has not been tested on a 28/29 device.
6. **`allowBackup` changed `true` → `false`.** Auto Backup was copying
   the app's private data dir — which holds the OIDC refresh token and
   session state — into the user's Google Drive. Turning it off is the
   right call, but it means a user restoring a phone now has to sign in
   again. Worth a line in the release notes.
7. **Screen orientation is `unspecified`** and the QML tree was built
   desktop-first. Rotation and small-screen layout are untested.
