# BSFChat store-submission runbook

The ordered, end-to-end path from here to "both packages are in their
review pipelines". Every step is labelled:

- **[YOU]** — only the owner can do it (credentials, a legal decision, a
  real device, an account).
- **[CI]** — a GitHub Actions job does it; you press a button or push a
  tag.
- **[DONE]** — already true; listed so you can see the gap, not so you
  can do it.

Times are wall-clock for one person who has not done it before. Things
that wait on somebody else (Apple's processing, Google's review) are
marked as waits and do not block the next step.

Companion documents, all in this repo:

| For | Read |
| --- | --- |
| Apple account setup, secrets, review-guideline notes | `docs/ios-release.md` |
| Play account setup, keystore, Console declarations | `docs/android-release.md` |
| Listing copy, Data Safety, nutrition labels, IARC | `web` repo, branch `docs/store-submission`, `STORE-LISTING.md` |

---

## 0. The one thing that is not paperwork

**[YOU] The server-side report endpoint is not deployed. 30 min + a
deploy.**

The client half of block / report / delete-account is merged. The server
half is not: `server` main has no `src/api/ReportHandler.cpp`, it lives
only on `feat/ugc-safety` (`d5d0fe6`). Against the deployed
`chat.bsfchat.com`, `POST /_matrix/client/v3/rooms/{roomId}/report/{eventId}`
returns 404, so the Report button in the app fails.

Both stores make in-app reporting a hard requirement for a chat app
(Apple guideline 1.2; Google's UGC policy), and both review notes in
`STORE-LISTING.md` tell the reviewer exactly where the Report button is.
A reviewer following those instructions will watch it fail.

Block and account deletion both work today — only reporting is broken.

Merge `feat/ugc-safety` into `server` main, let its CI go green, deploy,
and then tap Report on a real build against production once. Nothing
else in this runbook depends on it, so it can run in parallel with
everything below — but do not submit to either store until it is done.

---

## 1. Credentials — the actual blocker

Everything from §2 onwards is waiting on these and nothing else. Doing
them in this order means no step waits on a step below it.

### 1.1 [YOU] Google Play upload keystore — 10 min

You generate this on your own machine. Nothing in CI can, and nothing
should: if this key is lost after a bundle signed with it has reached
Play, publishing updates needs a key-reset request to Google.

```sh
keytool -genkeypair -v \
  -keystore ~/bsfchat-upload.jks \
  -storetype PKCS12 \
  -keyalg RSA -keysize 4096 -validity 10000 \
  -alias bsfchat-upload \
  -dname "CN=BSFChat, O=BSFChat, C=GB"
```

`keytool` prompts for the password — do not pass `-storepass` on the
command line, it lands in your shell history. `-validity 10000` is
~27 years; Play requires the key to be valid past 22 October 2033, so
anything much shorter is rejected later, at upload, with a message that
does not mention validity.

Back it up somewhere durable and offline before you do anything else.

Then base64 it for the secret:

```sh
base64 -i ~/bsfchat-upload.jks | pbcopy
```

**Verified offline**: `scripts/sign-android.sh` was run end-to-end
against a throwaway keystore generated exactly this way, on a real
release `.apk` and a real release `.aab` from `wt/client-android-release`.
The APK came out `zipalign -c 4`-clean and `apksigner verify`-clean; the
AAB came out `jarsigner -verify`-clean with the signature block written
and no pre-existing signature to conflict with. The script's decode,
shred-on-exit and env-var password handling all behaved. Nothing in that
path needs changing.

### 1.2 [YOU] Play secrets in GitHub — 5 min

`github.com/BSFChat/client` → Settings → Secrets and variables →
Actions → New repository secret. Four of them:

| Secret | Value |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64` | the base64 from §1.1, no newlines |
| `ANDROID_KEYSTORE_PASSWORD` | the keystore password |
| `ANDROID_KEY_ALIAS` | `bsfchat-upload` |
| `ANDROID_KEY_PASSWORD` | the key password (usually the same) |

If the base64 is pasted with line breaks, `sign-android.sh` catches it
and says so rather than producing something that looks signed.

### 1.3 [YOU] Apple Distribution certificate — 15 min

Full walkthrough in `docs/ios-release.md` §1.3. Summary: Keychain Access
→ Certificate Assistant → Request a Certificate from a Certificate
Authority → save to disk; upload the CSR at
developer.apple.com/account/resources/certificates → **Apple
Distribution** (not Developer ID, not Apple Development — neither can
sign an App Store build, and the CI job checks and refuses); download,
double-click to install, then right-click it in Keychain Access → Export
→ `.p12` **with a passphrase**.

```sh
base64 -i ~/bsfchat-dist.p12 | pbcopy
```

### 1.4 [YOU] App Store Connect API key — 5 min

appstoreconnect.apple.com → Users and Access → Integrations → App Store
Connect API → **+** → Access: **App Manager**.

**The `.p8` downloads exactly once.** Save it, then:

```sh
base64 -i ~/AuthKey_XXXXXXXXXX.p8 | pbcopy
```

Note the **Key ID** (from the filename) and the **Issuer ID** (shown
above the key list, one per account).

### 1.5 [YOU] Apple secrets in GitHub — 5 min

| Secret | Value |
| --- | --- |
| `IOS_DIST_CERTIFICATE` | base64 of the `.p12` from §1.3 |
| `IOS_DIST_CERTIFICATE_PWD` | its passphrase |
| `APPLE_TEAM_ID` | 10 characters, from developer.apple.com → Membership |
| `ASC_KEY_ID` | from §1.4 |
| `ASC_ISSUER_ID` | from §1.4 |
| `ASC_PRIVATE_KEY_BASE64` | base64 of the `.p8` from §1.4 |

The job now fails with a clear message if `IOS_DIST_CERTIFICATE` is set
and `IOS_DIST_CERTIFICATE_PWD` is not — previously that combination
died several steps later with "MAC verification failed during PKCS12
import", which reads like a corrupt certificate.

### 1.6 [YOU] The two app records — 20 min

Neither store lets an upload create the app. Both must exist first, and
both lock the identifier permanently.

**Apple.** developer.apple.com → Identifiers → **+** → App IDs → App →
Bundle ID **`com.bsfchat.app`** (explicit, not wildcard). Capabilities:
leave everything off — the app uses no entitlement-gated features. Then
appstoreconnect.apple.com → Apps → **+** → New App:

| Field | Value |
| --- | --- |
| Platform | iOS |
| Name | `BSFChat` |
| Primary language | English (UK) or (US) |
| Bundle ID | `com.bsfchat.app` |
| SKU | anything private and stable, e.g. `bsfchat-ios` |
| User Access | Full Access |

**Google.** play.google.com/console → Create app:

| Field | Value |
| --- | --- |
| App name | `BSFChat` |
| Default language | English |
| App or game | App |
| Free or paid | Free (**cannot be changed to paid later**) |

The application id `com.bsfchat.app` is bound at your first upload, not
here. The `android` CI job asserts it on the built artefact before
signing, so a wrong one cannot reach the Console by accident.

### 1.7 [YOU] Mailboxes and demo accounts — 20 min

- `support@bsfchat.com` and `security@bsfchat.com` must receive mail.
  Both stores email support; a bounce is a rejection. Send a test to
  each.
- A **password** demo account on `chat.bsfchat.com` (not OIDC — a
  reviewer cannot complete your sign-in provider's flow), a **second**
  account, and a seeded conversation with a few messages, an image and
  a link. Reviewers fail guideline 1.2 for "nothing to demonstrate
  against" more often than for missing features.
- Paste both sets of credentials into `STORE-LISTING.md` §5.1 and §5.2
  before you copy that text into the consoles.

---

## 2. Rehearse both pipelines without burning anything

**[CI] 40 min of machine time, 2 min of yours.**

Every signing, packaging and upload step in `.github/workflows/ci.yml`
has never executed. Until this change they could only execute on a tag
push — which meant the first real run of the most expensive, least
reversible part of the system would be a release. A `workflow_dispatch`
trigger has been added so you can rehearse it from a branch.

Actions → CI → Run workflow, on your integration branch:

| Input | Rehearsal value | Why |
| --- | --- | --- |
| `version` | **leave blank** | a derived `-dev.<sha>` version is fine when nothing is uploaded |
| `android_aab` | **true** | exercises AAB build + `jarsigner` |
| `ios_upload` | **false** | do not consume a TestFlight build number yet |

What to check when it finishes:

1. **android** job green, and its step summary shows
   `application id com.bsfchat.app`, a `versionCode`, and
   `minSdk / targetSdk 28 / 36`.
2. Artefacts `BSFChat-Android-APK` and `BSFChat-Android-AAB` both
   present. The `.aab` being present is the whole point: it proves the
   `bsfchat-app_make_aab` target, the release-bundle path resolution and
   the `jarsigner` invocation all work before a release depends on them.
3. **ios** job green through "Export .ipa", with a `BSFChat-iOS`
   artefact. Read the "Minimum iOS version (chosen by the Qt toolchain)"
   line out of the log and keep it — you need that number for the
   listing and nothing else will tell you.

> **Do not set `version` to something real and `android_aab` true unless
> you intend to upload the result.** An Android `versionCode` is burned
> permanently the moment an artefact carrying it reaches any Play track,
> including internal testing, and a branch build's derived version maps
> to the same `versionCode` as the eventual release (`cmake/Version.cmake`
> documents this and the manual input exists so that the hazard is a
> decision rather than an accident).

### 2.1 [YOU] Optional: validate the .ipa without uploading — 5 min

Download the `BSFChat-iOS` artefact, and from a Mac with Xcode:

```sh
xcrun altool --validate-app -f BSFChat.ipa \
  --api-key "$ASC_KEY_ID" --api-issuer "$ASC_ISSUER_ID"
```

with the `.p8` at `~/.appstoreconnect/private_keys/AuthKey_<KEYID>.p8`.
This runs Apple's full upload validator — icons, `Info.plist` keys,
entitlements, architectures — and reports everything that would reject
the real upload, without consuming a build number. Worth doing once.

On Xcode 16 the option spelling is `--apiKey` / `--apiIssuer`; on Xcode
26 it is `--api-key` / `--api-issuer`. `xcrun altool --help` will tell
you which one you have.

---

## 3. What would have failed on the first real run

Recorded because these were found by reading and by offline testing, not
by running the jobs — and because the runbook is less useful than the
reasons behind it.

### 3.1 The TestFlight upload could not have worked (fixed)

The step ran:

```sh
xcrun iTMSTransporter -m upload -assetFile "$IPA_PATH" \
  -apiKey "$ASC_KEY_ID" -apiIssuer "$ASC_ISSUER_ID" -v informational
```

On current Xcode that binary is a stub. Running it prints:

```
iTMSTransporter is now part of Transporter.
Please install Transporter from the Mac App Store ... and try again.
```

and exits. Verified against Xcode 26.6. The failure would have landed at
the very last step of a tag build, after a twenty-minute archive, on the
day of a release.

Replaced with `xcodebuild -exportArchive` using an ExportOptions render
whose `destination` is `upload`. That is documented in
`xcodebuild -help`, has been stable across Xcode 16 and 26, and reuses
the same `-authenticationKeyPath` / `-authenticationKeyID` /
`-authenticationKeyIssuerID` arguments the archive and export steps
already pass. `altool --upload-package` also works, but its option
spelling changed between those two Xcodes, so pinning it would make the
job depend on the runner image.

### 3.2 `ExportOptions.plist` carried two keys it should not (fixed)

`xcodebuild -help` on Xcode 26.6 documents `thinning` as **"For
non-App Store exports"**, and `uploadBitcode` is no longer a listed key
at all.

`thinning` was set to `<thin-for-all-variants>`. For a non-store method
that makes `exportArchive` emit a *directory of per-variant `.ipa`
files*; combined with the step's `find -name '*.ipa' | head -1`, the
job could have picked an arbitrary thinned variant and uploaded it. An
App Store submission must be one universal package — Apple thins per
device at download time. Both keys removed, and the `.ipa` is now
resolved with a **exactly-one** assertion rather than `head -1`, the
same discipline the Android job already applies to its `.apk` and
`.aab`.

The comment on `manageAppVersionAndBuildNumber` was also wrong — it
claimed the key stops Xcode downgrading the bundle id. It controls
whether Xcode rewrites the version and build number, which matters here
because the build number is deliberately set from the workflow run
number. The value (`false`) was right; the reasoning is now too.

### 3.3 The keychain could have hung the runner for six hours (fixed)

```sh
security set-key-partition-list -S apple-tool:,apple: -k "$PWD" "$KEYCHAIN"
```

Apple's own sample omits `codesign:` from that list. With it omitted,
`codesign` is not on the imported key's ACL — and instead of failing it
blocks on a GUI keychain prompt that a headless runner can never answer.
The job then hangs until the job timeout. `codesign:` added.

### 3.4 `mapfile` is not available on the macOS runner (avoided)

The Android job's unique-artefact assertions use `mapfile`, which is
correct on `ubuntu-24.04`. The same idiom was about to be copied into
the iOS job, where `/bin/bash` is still 3.2 and `mapfile` does not
exist. The iOS step counts with `find | wc -l` instead.

### 3.5 Everything that was already right (verified, not changed)

Checked against a real artefact —
`wt/client-android-release/.../android-build-release-unsigned.apk` and
the matching `.aab` — with the SDK's own `aapt2`, `zipalign` and
`apksigner`, plus a throwaway keystore generated in a temp directory:

| Assertion in CI | Result against the real artefact |
| --- | --- |
| `package: name='com.bsfchat.app'` | holds |
| `targetSdkVersion >= 36` | holds (36) |
| a native lib under `lib/arm64-v8a/libbsfchat-app*.so` | holds — the real name is `libbsfchat-app_arm64-v8a.so`, which is why the check is a prefix match and not an exact one |
| no 32-bit ABI present | holds (only `lib/arm64-v8a/`) |
| release-APK path glob resolves to exactly one file | holds |
| release-AAB path glob resolves to exactly one file | holds |
| `zipalign -p -f 4` before `apksigner` | correct order; result verifies aligned |
| `apksigner verify` after signing | passes, v3 scheme (minSdk is 28, so v1/v2 are not needed) |
| the `.aab` is unsigned before `jarsigner` | holds — no `META-INF/*.RSA` beforehand, so there is no double-signature hazard |
| `jarsigner -verify` after signing | passes |

And the versionCode derivation in `cmake/Version.cmake` is sound. Hand-
checked and confirmed on a built artefact: `0.0.52` → `5200`,
`0.0.52-rc.1` → `5101`, `0.0.51` → `5100`. Release candidates sit in the
99-wide band *below* their own release and *above* the previous one, so
an RC can go to internal testing and be promoted over by the final build
without a collision. `CFBundleShortVersionString` comes from
`BSFCHAT_VERSION_DOTS`, so an `-rc.N` tag still produces a strict
`M.N.P` — Apple rejects anything else, and this would have been an easy
mistake to make.

### 3.6 Two things left as they are, deliberately

- **There is no upload-to-Play step, and there should not be one yet.**
  Google will not accept an app's *first* bundle through the Play
  Developer API. The first upload is a human in the Console (§5).
  Automating later uploads means adding a service-account JSON secret;
  worth doing once the manual path has worked at least once.
- **`CFBundleVersion` is `github.run_number`.** Monotonic per repository
  and never reused, which is what Apple needs. The one way to break it
  is renaming or replacing the workflow file, which resets the counter
  to 1 and would make every subsequent upload non-monotonic. If that
  ever happens, set the build number explicitly for a few releases.

---

## 4. TestFlight

### 4.1 [YOU] Cut the build — 2 min, then 25 min of waiting

Two routes:

- **Rehearsal route** (recommended for the first one): Actions → CI →
  Run workflow with `version` set to the version you mean (e.g.
  `0.0.53-rc.1`) and `ios_upload` **true**.
- **Release route**: push a `v*` tag. Wait for main's three-platform CI
  to be green first — that is the house rule and it exists because a
  tag is the only thing here that cannot be taken back.

### 4.2 [CI] What happens

Imports the distribution certificate into a throwaway keychain; writes
the App Store Connect key to `~/.appstoreconnect/private_keys`;
configures with `-G Xcode`; archives with automatic signing and
`-allowProvisioningUpdates`, which mints the provisioning profile on the
fly so there is no `.mobileprovision` secret to rotate; asserts on the
**archived** `Info.plist` that `ITSAppUsesNonExemptEncryption`,
`NSMicrophoneUsageDescription`, `NSLocalNetworkUsageDescription`,
`CFBundleIconName` and the bundle id are all present and right; exports
a single `.ipa`; uploads it; and deletes the API key whatever happened.

The `CFBundleIconName` check is worth knowing about: an iOS bundle whose
asset catalog was listed but never *compiled* is rejected on upload as
ITMS-90713 with no other symptom.

### 4.3 [YOU] Export compliance — 10 min, and it is a gate

`ios/Info.plist.in` declares `ITSAppUsesNonExemptEncryption = true`,
which is the honest answer: call media is DTLS-SRTP from libdatachannel
built against an OpenSSL we cross-compile ourselves, which is not
Apple's OS crypto.

**`true` does not skip the questionnaire — `false` is what does that.**
A comment in the plist claimed otherwise and has been corrected. Expect:

- The build to appear in App Store Connect as **"Missing Compliance"**.
- Questions to answer once per version, not per build.
- The **French encryption declaration**, if France is in your territory
  list (it is, by default).
- No CCATS, because the algorithms are industry-standard rather than
  proprietary. `docs/ios-release.md` §3 has the reasoning; if you want
  belt and braces, file the BIS year-end self-classification report too
  — it is an email.

**Missing Compliance blocks external TestFlight testers. It does not
block internal ones**, so you can start testing on your own devices
immediately and sort this out in parallel.

Once Apple issues a compliance code, paste it into `ios/Info.plist.in`
as `ITSEncryptionExportComplianceCode` and the questions stop for good.

### 4.4 [YOU] Internal vs external testers — 5 min, then a 24–48h wait

- **Internal** (up to 100, must be users on your App Store Connect
  team): available as soon as processing finishes. **No beta review.**
  This is how you get the app onto a phone today.
- **External** (up to 10,000): needs **Beta App Review**, which is a
  real review by a person — usually 24–48 hours — and needs Test
  Information filled in: what to test, the demo account, your contact
  email. Export compliance must be resolved first.

Start internal today; submit for external review at the same time.

### 4.5 [YOU] Test Information — 10 min

App Store Connect → TestFlight → Test Information. Reuse the review
notes from `STORE-LISTING.md` §5.1 — they already contain the sign-in
walkthrough, the demo credentials and the explanation of the background
audio mode. Beta App Review reads the same text App Review will.

---

## 5. Play closed testing

### 5.1 [YOU] Build the bundle — 2 min, then 30 min of waiting

Actions → CI → Run workflow, `version` set to what you mean,
`android_aab` **true**. Download the `BSFChat-Android-AAB` artefact.

Read the versionCode out of the android job's step summary and write it
down. It is burned the moment you upload.

### 5.2 [YOU] The Console forms, before any upload — 60–90 min

Play will not let you create a release until these are complete. They
are the bulk of the work and none of them needs the artefact.

| Form | Source |
| --- | --- |
| Store listing (short + full description, icon, feature graphic, screenshots) | `STORE-LISTING.md` §2 |
| **Data safety** | `STORE-LISTING.md` §3 — every row, with the reasoning, and the free text at §3.3 |
| **Content rating (IARC)** | `STORE-LISTING.md` §6 — a questionnaire, answers and reasoning given |
| **App access** | the demo account from §1.7; tick "All functionality is restricted" and give the credentials |
| **Ads** | No |
| **Target audience** | 13+; not directed at children |
| **Data deletion** | `STORE-LISTING.md` §5.3 — in-app path plus `https://bsfchat.com/support` as the web route |
| **Government apps / financial features / health** | No to all |

### 5.3 [YOU] Foreground-service declarations — 45 min, four videos

This is the part people underestimate. Each declared
`foregroundServiceType` needs a Console declaration **and a short screen
recording** demonstrating the feature. There are **four**, not three —
`VoiceService` declares `microphone|camera`, and camera is its own
declaration:

| Type | Service | What the video must show |
| --- | --- | --- |
| `microphone` | `VoiceService` | Join a voice channel, speak, then background the app while the call continues |
| `camera` | `VoiceService` | Turn the camera on inside a voice channel |
| `dataSync` | `SyncService` | A message arriving as a notification with the app backgrounded |
| `mediaProjection` | `MediaProjectionService` | The system screen-capture consent dialog, then a shared screen |

`adb shell screenrecord` is the cheapest way to capture these — see §8.3
— but they must come off a device or emulator where the feature really
works, which for the call videos means real audio.

`dataSync` is the one Google pushes back on hardest. Say plainly in the
declaration that it maintains the Matrix sync connection so messages can
raise a **local** notification, that there is no Firebase Cloud
Messaging anywhere in the app, and that no notification content is sent
to Google.

### 5.4 [YOU] The first upload is manual — 15 min

Play Console → Testing → Closed testing → Create new release → upload
the `.aab` by hand. This is not a limitation of the CI job: Google will
not accept an app's first bundle through the Developer API.

Opt in to **Play App Signing** when prompted (it is the default). Google
then holds the app signing key and yours is only the *upload* key, which
makes a future loss recoverable rather than terminal.

### 5.5 [YOU] Testers — 10 min, then possibly a 14-day wait

Create an email list with your testers and add it to the closed track.

**Check which rule your account falls under** (Play Console → Setup →
Advanced settings). A developer account registered as an **individual
after 13 November 2023** must run a closed test with **at least 12
testers opted in for 14 continuous days** before it can apply for
production access. An organisation account, or an older individual
account, is exempt. This decides whether production is two weeks away or
available whenever you like, and it is the single fact most likely to
change your plan for the next fortnight.

Either way, closed testing itself starts as soon as the release is
reviewed — usually hours to a couple of days for a first submission.

---

## 6. Order of play for today

| # | Who | What | Time |
| --- | --- | --- | --- |
| 1 | YOU | Generate the Play keystore, back it up (§1.1) | 10 min |
| 2 | YOU | Add the four Android secrets (§1.2) | 5 min |
| 3 | YOU | Apple Distribution certificate → `.p12` (§1.3) | 15 min |
| 4 | YOU | App Store Connect API key (§1.4) | 5 min |
| 5 | YOU | Add the six Apple secrets (§1.5) | 5 min |
| 6 | CI | Rehearsal run — `android_aab: true`, `ios_upload: false` (§2) | 40 min machine |
| 7 | YOU | *(during the wait)* create both app records (§1.6) | 20 min |
| 8 | YOU | *(during the wait)* mailboxes + demo accounts (§1.7) | 20 min |
| 9 | YOU | *(during the wait)* merge and deploy `feat/ugc-safety` (§0) | 30 min + deploy |
| 10 | YOU | iPhone screenshots on a real device (§8.1) | 30 min |
| 11 | CI | iOS build with `ios_upload: true` (§4.1) | 25 min machine |
| 12 | CI | Android build with `android_aab: true` (§5.1) | 30 min machine |
| 13 | YOU | Export compliance answers (§4.3) | 10 min |
| 14 | YOU | TestFlight Test Information; add internal testers; submit for external review (§4.4, §4.5) | 15 min |
| 15 | YOU | Play Console forms — listing, Data Safety, IARC, app access (§5.2) | 60–90 min |
| 16 | YOU | Four foreground-service videos (§5.3) | 45 min |
| 17 | YOU | Upload the `.aab`, create the closed-testing release (§5.4) | 15 min |
| 18 | YOU | Tester list; check the 12/14 rule (§5.5) | 10 min |

Roughly **four to five hours** of your attention, plus waits. Steps 7–10
fit inside the machine time of step 6.

---

## 7. Decisions that are yours and are not blocking

None of these stops a submission today; all of them will be asked about
eventually.

1. **The licence.** No `LICENSE` file exists in any of the six repos and
   none ever has. `STORE-LISTING.md` §0.6 lays out the evidence and
   recommends MIT plus a separate trademark note — the recommendation is
   not applied, because adding one is an irrevocable grant of rights
   over your own property and it would contradict what
   `bsfchat.com/terms` currently tells the public. The first question to
   answer is whether `org.opencontainers.image.licenses=MIT` in the two
   Dockerfiles was deliberate. Neither listing may say "open source"
   until this is settled; "the source is public" is what the copy says
   now, and it is true.
2. **Qt on iOS and LGPL-3.0.** Qt for iOS is static-only and you are on
   the open-source build. LGPLv3 expects a user to be able to relink
   against a modified Qt, and an App Store binary cannot be relinked.
   This is independent of your own licence choice and needs a lawyer.
   It has not stopped other Qt apps shipping on the App Store.
3. **Whether to claim echo cancellation.** It is implemented on macOS
   and iOS and has never been heard on a device. `STORE-LISTING.md` §0.2
   has the wording that is safe either way.
4. **Link previews.** The app fetches arbitrary third-party pages from
   the reader's device, with no setting to turn it off, which discloses
   the reader's IP to any site a sender links. Honestly disclosed in the
   privacy policy and the Data Safety free text. A setting would be
   better. Not a blocker; nobody in review will find it.

---

## 8. Screenshots

Both stores need them and they are the last thing that tends to get
done. Apple's must come off a real iPhone; Play's can come off an
emulator.

### 8.1 [YOU] iPhone — only you can capture these

**What Apple requires.** One set at **6.9"** — 1320 × 2868 or
1290 × 2796 portrait — which is an iPhone 16 Pro Max, 17 Pro Max or
equivalent. Apple scales that set down for every smaller device, so a
6.5" set is optional. Up to 10 per size; 3 is thin, 5–6 reads as
finished. No status-bar edits, no device frames, no "coming soon"
overlays, and nothing in a screenshot that the build does not do.

**Capture**: volume-up + side button on the device. AirDrop them to the
Mac. Do not resize or crop — App Store Connect wants the native
resolution.

**Before you start**, set the demo account up so the screenshots look
like a used product rather than an empty one: a server with 3–4
channels in two categories, 15–20 messages of real conversation, at
least one image attachment, one thread with a couple of replies, and a
second person present so the member list is not a list of one.

**The shot list**, in the order they should appear in the listing —
Apple shows the first two in search results, so those two carry the
listing:

| # | Screen | State it must be in | Why |
| --- | --- | --- | --- |
| 1 | Channel list + a busy channel | Conversation visible, an image attachment in view, member list showing more than one person | This is the product. It is also the search-results thumbnail. |
| 2 | **A voice channel, connected** | Joined, at least two participants, mute and camera controls visible, the persistent voice dock showing | **New.** The earlier draft of the listing told you not to screenshot voice on iOS because it was not built. It is built now, it is the strongest differentiator on the page, and a reviewer comparing screenshots to the binary will find it. |
| 3 | The server list / multi-server sidebar | Two or more servers added | The "one account, every server" claim, shown rather than asserted |
| 4 | Search results | A real query with several hits across channels | Full-text search is a headline feature and is invisible otherwise |
| 5 | A thread | Parent message plus 2–3 replies | Shows the app is not a flat chat |
| 6 | Profile card with the Block control visible | Another user's card open, the lock button clearly in frame | Doubles as evidence for guideline 1.2 |

Optional 7th if you want one: settings showing "Hide my IP address" —
it is the most concrete expression of the privacy pitch.

**Do not** screenshot: screen sharing (does not exist on iOS), any
notification (there are none on iOS), or anything with real people's
names or real message content in it.

### 8.2 [ME, on request] Play — an emulator is acceptable

Google accepts emulator captures. Requirements: 2–8 phone screenshots,
PNG or JPEG, 16:9 or 9:16, each side between 320 px and 3840 px. A
1080 × 1920 AVD is the easy default. Also needed: a 512 × 512 icon and a
1024 × 500 feature graphic, neither of which is a screenshot.

**The recipe**, once an AVD is running with the APK installed:

```sh
# 1. Confirm exactly one device, and that it is the emulator.
adb devices

# 2. Set a clean status bar — no carrier junk, full battery, fixed clock.
#    Google does not require this; it is the difference between a
#    screenshot that looks shipped and one that looks like a dev build.
adb shell cmd statusbar overlay-icons false
adb shell settings put global sysui_demo_allowed 1
adb shell am broadcast -a com.android.systemui.demo -e command enter
adb shell am broadcast -a com.android.systemui.demo -e command clock -e hhmm 0900
adb shell am broadcast -a com.android.systemui.demo -e command battery -e level 100 -e plugged false
adb shell am broadcast -a com.android.systemui.demo -e command network -e wifi show -e level 4
adb shell am broadcast -a com.android.systemui.demo -e command notifications -e visible false

# 3. Capture, one per screen. Straight to the host, no /sdcard round trip.
adb exec-out screencap -p > play-01-channel.png

# 4. Check the dimensions are what you think they are.
sips -g pixelWidth -g pixelHeight play-01-channel.png

# 5. When finished, drop the demo status bar.
adb shell am broadcast -a com.android.systemui.demo -e command exit
```

For the foreground-service demo videos in §5.3:

```sh
adb shell screenrecord --time-limit 30 /sdcard/fgs-microphone.mp4
# ... drive the app ...  Ctrl-C stops it early
adb pull /sdcard/fgs-microphone.mp4
adb shell rm /sdcard/fgs-microphone.mp4
```

**The Play shot list.** Same six as iOS, plus the two things Android has
and iPhone does not — which is the whole reason the two listings are not
unified:

| # | Screen | State |
| --- | --- | --- |
| 1 | Channel list + busy channel | As §8.1 #1 |
| 2 | Voice channel, connected | Two participants, controls visible |
| 3 | **Screen sharing active** | Sharing into a voice channel, the system capture indicator visible. **Android only — this shot must not appear in the Apple listing.** |
| 4 | **A notification** | A message notification on the lock screen or shade, generated on-device. Evidence for the `dataSync` declaration. |
| 5 | Server list / multi-server | Two or more servers |
| 6 | Search results | A query with several hits |
| 7 | Thread | Parent plus replies |
| 8 | Profile card with Block visible | Another user's card |

Eight is the maximum, so if something has to go, drop #7.

**I can drive this** — say the word and I will start the emulator,
install the APK and capture the set. The one thing I will not do is
anything that touches your Mac's microphone, camera or screen, so the
voice and screen-share shots (#2, #3) and all four demo videos need you
or a device I am not driving.

---

## 9. After both are in

- TestFlight internal: minutes after processing. External: 24–48h for
  Beta App Review.
- Play closed testing: hours to a couple of days for the first review.
- Neither is the App Store or the Play production listing. Full App
  Review and Play production access are separate submissions, and the
  12-testers-for-14-days rule (§5.5) may gate the second of them.
- The next upload's `versionCode` must be strictly greater than this
  one's. The CI step summary tells you what this one was.
