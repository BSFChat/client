# Voice and camera video on iOS

`BSFCHAT_ENABLE_VOICE` defaults `OFF` for iOS. This file is the plan for
turning it on: what actually has to change, what it costs, in what order,
and — kept strictly separate — which claims here have been verified and
which are reasoning that a build has not yet tested.

Nothing on the iOS voice path has ever been compiled. Not once. Every
estimate below is therefore an estimate about unexplored code, and the
error bars are wide on purpose.

**The companion documents are `docs/ios-release.md` (the App Store
mechanics) and `docs/android-release.md` (the platform this port copies
from).** Android is the template: voice is `ON` there, libdatachannel and
opus cross-compile through the NDK, and mic capture goes through Qt
Multimedia's `QAudioSource`. Most of what follows is the list of places
where iOS is *not* Android.

---

## 0. How confident each claim is

Three labels are used throughout, and they mean exactly this:

| Label | Meaning |
|---|---|
| **Verified** | Checked against a file, a binary, or a tool on this machine, and the check is reproducible from what is written here. |
| **Reasoned** | Follows from documented Apple/Qt behaviour plus the code as read. Not tested. Most likely right; not something to plan a date on alone. |
| **Unknown** | Genuinely open. Only a build answers it. |

No estimate in this file should be read as "we know". The gate that
turns Reasoned into Verified is always the same: compile it.

---

## 1. Gap analysis, component by component

### 1.1 OpenSSL — **done already**

**Verified.** `scripts/build-openssl-ios.sh` cross-compiles OpenSSL
3.5.7 static (`no-shared`) for either `simulator` (`iossimulator-xcrun`)
or `device` (`ios64-xcrun`), and both `scripts/build-ios.sh` and the
`ios` CI job already build it, cache it, and pass the full `OPENSSL_*`
quadruple. This is not voice-gated and never was — `bsfchat_protocol`
needs OpenSSL unconditionally for jwt-cpp — so it is **already proven by
the green iOS CI job today**.

One thing was missing and is now added: `CMakeLists.txt` had no
`if(IOS)` OpenSSL hint mirroring the Android one, so a hand-rolled
`qt-cmake` that did not pass the quadruple failed at
`find_package(OpenSSL)` with an error that reads like a missing system
package. There is now a non-forcing fallback that points at
`deps/openssl-ios-{device,simulator}` and, when that directory is
absent, says which script to run.

**Effort: 0 days.** Optionally 0.25 to add a simulator OpenSSL cache
entry to CI alongside the existing device one.

### 1.2 libdatachannel — **Unknown, and the first thing to find out**

Pinned to `v0.24.5` via FetchContent with `NO_MEDIA OFF` (so libsrtp
comes along), `NO_WEBSOCKET ON`, and a mandatory patch step
(`cmake/patch-libdatachannel.cmake`). Under the hood that is libjuice,
libsrtp, usrsctp and plog.

Nothing here is *known* to be a problem. Nothing here is known to work
either. The specific things to watch, in the order they are likely to
bite:

- **`try_compile` under the iOS toolchain.** Third-party CMake that
  probes with `check_include_file` / `check_c_source_compiles` builds a
  full executable by default, which cannot link or sign for iOS. The fix
  is `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`; Qt's iOS toolchain
  normally sets it, but a `FetchContent` subproject that resets it will
  fail in a way that reads like a missing header. **Reasoned.**
- **usrsctp.** It has Apple support, but its CMake is old and its
  platform detection is `APPLE`-shaped rather than `iOS`-shaped, which
  is exactly the family of bug where a macOS-only path gets taken.
  **Unknown.**
- **libsrtp warnings-as-errors.** Already defused for three different
  option spellings in `cmake/Dependencies.cmake`; that work carries over.
- **The patch step.** `patch-libdatachannel.cmake` fails the configure
  if its anchor stops matching. Platform-independent, so it either works
  everywhere or nowhere — no new iOS risk.
- **Simulator vs device.** Two separate dependency trees, and the
  simulator is where day-to-day work happens while the device is what
  ships. Both have to build.

**Effort: 1–2.5 days.** Could genuinely be half a day if it just works;
the upper bound is for a usrsctp or try_compile fight.

### 1.3 opus — **Reasoned, low risk**

`v1.5.2` via FetchContent. arm64 always has NEON, so opus takes the
`OPUS_PRESUME_NEON` path rather than the runtime-detection path that
needs `getauxval` on Linux/Android. There is no obvious iOS-shaped hazard
in its CMake.

**Effort: 0.25–0.5 days.** Highest-confidence item on the list after
OpenSSL.

### 1.4 The H.264 encoder — **use VideoToolbox, not openh264**

> **Done, 2026-09-23. See §10.** The assessment below was right on
> every count and is kept as the reasoning; §10 records what was
> actually built and which of its predictions a build confirmed.

The tree vendors openh264 for Linux and desktop macOS and deliberately
excludes it from both mobile platforms
(`cmake/Dependencies.cmake`: `if(NOT WIN32 AND NOT ANDROID AND NOT IOS)`).
That exclusion is correct and should stay. Honest assessment:

**openh264 on iOS would be a mistake.** It is a software encoder. On a
phone that means encoding 720p in software on the CPU, which costs
battery, produces heat, and on sustained use hits thermal throttling —
which degrades the *whole* call, not just the video. It also needs a
make-based out-of-tree build (`scripts/build-openh264.sh`) rather than
FetchContent, so it adds a vendoring step for every architecture. There
is no scenario where this is the right answer on iOS.

**VideoToolbox is the right answer,** and most of the work is already
written. `src/voice/video/MacVTEncoder.mm` and `MacVTDecoder.mm` include
only CoreMedia, CoreVideo and VideoToolbox — no AppKit, no CGDisplay, no
ScreenCaptureKit. They are gated out of iOS purely by
`if(APPLE AND NOT IOS)` in `CMakeLists.txt`. What actually needs doing:

1. **Drop `kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder`**
   (`MacVTEncoder.mm`, two sites). It is `ios(17.4)` in the SDK, the Qt
   6.10.3 iOS deployment target is **17** (**Verified**:
   `~/Qt/6.10.3/ios/lib/cmake/Qt6/qt.toolchain.cmake` sets
   `CMAKE_OSX_DEPLOYMENT_TARGET "17"`), and on iOS it is meaningless
   anyway because hardware is the only encoder. Delete it on iOS rather
   than availability-guarding it.
2. **Change the encoder's input pixel format.** It currently allocates a
   fresh triplanar `kCVPixelFormatType_420YpCbCr8Planar` `CVPixelBuffer`
   per frame with no pool and no
   `kCVPixelBufferIOSurfacePropertiesKey`. iOS hardware encoders want
   NV12 (`kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`) from an
   IOSurface-backed `CVPixelBufferPool`. Triplanar-without-IOSurface will
   at best force a CPU conversion and at worst be refused. **This is the
   most likely functional (not compile) failure on iOS.** **Reasoned.**
3. **Handle session invalidation across backgrounding.** Neither file
   handles `kVTInvalidSessionErr` / `kVTVideoDecoderMalfunctionErr`,
   which iOS produces after the app has been backgrounded and the
   hardware session torn down. The session must be rebuilt on
   foregrounding. Not needed on macOS, mandatory on iOS.
4. Consider dropping the per-frame
   `VTCompressionSessionCompleteFrames` — it drains the hardware
   pipeline every frame. Works, but wasteful on a phone.

Everything downstream is portable: libyuv is *not* excluded on iOS, so
`FrameConverter` comes for free, and `LatencyCriticalActivity` already
compiles to inline no-ops off macOS.

**Consequence of doing nothing:** an iOS build advertises no video codecs
at all (`VoiceEngine::localCapsJson`), so `PeerCaps` negotiation puts
every peer on the legacy JPEG path. That is today's Android behaviour, so
it degrades correctly rather than breaking — it is just bad.

**Effort: 1–3 days.**

### 1.5 Qt Multimedia audio capture and playback — **works in theory, three real hazards**

`AudioWorker` uses `QAudioSource` (push mode, `readyRead`) and
`QAudioSink` (`bytesFree()`-metered pump at 10 ms), 48 kHz mono int16,
20 ms frames. All of it is platform-neutral C++ with exactly one
`#ifdef` (`kPlatformVoiceProcessing`, Android-only). There is every
reason to think it compiles for iOS unchanged.

Whether it *works* is a different question, and there are three
specific hazards, all **Verified** by inspecting the Qt 6.10.3 iOS
binaries shipped with this project:

**(a) Qt never configures `AVAudioSession`, and the iOS default category
cannot record.** `qdarwinaudiosource.mm.o` references
`AVAudioSession` for exactly one thing — reading `sampleRate`. Its
entire string table contains `sharedInstance` and `sampleRate` and no
category, mode or activation selector. Meanwhile
`qcoreaudiosessionmanager.mm.o` — Qt's AVAudioSession wrapper, which
*does* know about `PlayAndRecord` and `VoiceChat` — **is dead code in
this build**: no other object in QtMultimedia has an undefined reference
to any `CoreAudioSessionManager` symbol.

The iOS default category is `SoloAmbient`, which is playback-only. So
without the app configuring the session itself, `QAudioSource` has
nothing to record from. **This is why nothing else on this list matters
until §2.1 is done.**

**(b) There is no acoustic echo cancellation.** See §2.3 — it has its
own section because it is the largest single piece of work in this
document.

**(c) `QMediaPlayer` will steal the session mid-call.** Qt's
`avfmediaplayer.mm.o` calls
`[[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback …]`
and `setActive:error:`. `qml/components/VideoPlayerCard.qml` instantiates
a QML `MediaPlayer`. So **a user who taps a video attachment while in a
voice channel switches the process's audio session to a playback-only
category and their microphone goes dead** — with no error, no log, and
no UI indication. It will be reported as "voice randomly stops working".

This needs an explicit guard: either refuse/defer inline media playback
while a voice session is active, or re-assert the voice category when
the player stops. There is no Qt-level setting for it; the category is
process-global and last-writer-wins.

(`QSoundEffect`, used for chat chimes, is safe — **Verified**, it is
implemented over `QAudioSink` and touches no session category.)

**(d) Audio device enumeration is meaningless on iOS.** **Verified**:
`qdarwinaudiodevices.mm.o` enumerates through `AVCaptureDevice`
(`defaultDeviceWithMediaType:`,
`discoverySessionWithDeviceTypes:mediaType:position:`), which on iOS
reports one built-in microphone and nothing else. It never touches
`AVAudioSession`'s route list. Two consequences:

- The audio device picker in Client Settings → Audio is empty theatre on
  iOS. It should be hidden, and route selection offered through the
  system route picker instead.
- **`QMediaDevices` will not emit a change when the user plugs in
  headphones or connects AirPods.** The whole `AudioDevicePolicy` /
  `RestartDebounce` hot-plug machinery in `AudioWorker` is therefore
  inert on iOS. `AVAudioSessionRouteChangeNotification` is the only
  route signal an iOS build gets, which is why
  `IosAudioSession.h` surfaces it as `SessionEvent::RouteChanged`.

**Effort: 1–3 days** to bring up and adapt, on top of §2.1. The format
negotiation question (does the device actually give us 48 kHz under
`voiceChat`, or 24 kHz with an `AudioConverter` in the path?) is
**Unknown** and is the sort of thing that eats an afternoon.

### 1.6 RTP and pacing — **portable as written**

`PeerConnectionManager` builds libdatachannel's own `H264RtpPacketizer` /
`H265RtpPacketizer` and wraps them in this project's own
`CodecPacketizerRouter`, `PacedRtpSender`, `RtpGapDetector`. The pacing
arithmetic lives in `src/voice/video/RtpPacerCore.h`, which is pure and
platform-free. `PacedRtpSender` uses `std::thread`, `std::mutex`,
`std::condition_variable`, `std::chrono::steady_clock` — nothing
platform-specific.

One iOS-shaped note, not a blocker: it spawns a **detached** drain thread
per track with a 200 ms idle wake. That is a battery and
background-execution consideration on a phone, not a correctness one.

**Effort: 0 days.** Revisit thread count only if battery measurements
say so.

---

## 2. The iOS platform work nothing in the tree does today

### 2.1 `AVAudioSession` — **partly done on this branch**

The single highest-leverage change, and the one everything else waits
behind. `src/voice/IosAudioSession.{h,mm}` is new: category
`playAndRecord`, mode `voiceChat`, option `defaultToSpeaker`, preferred
48 kHz / 20 ms, `setActive:`, and observers for interruption, route
change and media-services-reset. It is the iOS counterpart of
`AndroidAudioRouting.h` and follows the same rule — inline no-ops off
iOS, so `AudioWorker` calls it with no `#ifdef` at the call site.

Why each of the three settings is load-bearing:

| Setting | Without it |
|---|---|
| category `playAndRecord` | Recording is not permitted at all. `QAudioSource` has nothing to capture. |
| mode `voiceChat` | No call-tuned signal path, no implicit Bluetooth HFP routing. |
| option `defaultToSpeaker` | `playAndRecord` routes playback to the **earpiece**. The app sounds broken. (Exactly Android's `MODE_NORMAL` trap.) |

It is wired into `AudioWorker::startDevices()` *before* `QAudioSource` is
constructed — same ordering rule as Android's `MODE_IN_COMMUNICATION`,
because the category and mode decide which signal path the audio unit is
built on and changing them afterwards does not re-plumb an open unit.
Unlike Android, a failure here **refuses the join**: a
misconfigured session means guaranteed silence, and joining anyway would
reproduce the V-H4 ghost-member bug on a new platform.

**Status: written, reviewed, never compiled.** **Effort remaining: 0.5–1
day** to build it and confirm on a device.

### 2.2 The `audio` background mode — **done on this branch**

`UIBackgroundModes = ["audio"]` is now emitted into `ios/Info.plist.in`
through a new `@BSFCHAT_IOS_BACKGROUND_MODES@` substitution, **only when
`BSFCHAT_ENABLE_VOICE` is ON**. Both halves of that conditional matter:

- Voice **on** and the key **absent** → iOS silences the session the
  moment the app leaves the foreground. Every call dies when the user
  switches apps or locks the screen. Silent, no log, and it will be
  reported as "voice randomly cuts out".
- Voice **off** and the key **present** → an unused background mode,
  which is itself an App Review rejection.

A POST_BUILD guard (`check-ios-bgmode.sh`) fails the build if voice is
compiled in and the shipped bundle does not declare it — same reasoning
as the existing `NSMicrophoneUsageDescription` guard for macOS. The
guard's PlistBuddy-and-grep logic is **Verified** against real
PlistBuddy output (matches `audio`; rejects both a missing key and a
`voip`-only array), and the rendered plist is **Verified** valid by
`plutil -lint` in both the voice-on and voice-off configurations.

`voip` is deliberately never emitted. It requires a real
`PKPushRegistry` + CallKit integration and is rejected on sight without
one; there is no PushKit, no CallKit and no APNs anywhere in this
client.

**Effort remaining: 0.25 days** to verify on a device.

### 2.3 Echo cancellation — **the long pole (now built; see §9)**

> **Status 2026-09-23.** Option (c) below is implemented on branch
> `feat/darwin-aec`: `src/voice/DarwinVpioBackend.{h,mm}`, shared by
> macOS and iOS, behind the `IAudioBackend` seam the diagram in this
> section proposes. It compiles and links for both platforms. **Nobody
> has heard it.** The rest of this section is left exactly as written,
> because it is the reasoning the implementation followed and the record
> of what was believed before a device tested it. §9 is what happened.

**Verified, and this is the central technical finding of this document:**
Qt's iOS `QAudioSource`/`QAudioSink` are built on
`kAudioUnitSubType_RemoteIO`, not `kAudioUnitSubType_VoiceProcessingIO`.
QtMultimedia 6.10.3 for iOS contains exactly one
`AudioComponentDescription`, in `QCoreAudioUtils::makeAudioUnitForIO()`,
and it reads:

```
$ otool -s __TEXT __literal16 qcoreaudioutils.cpp.o
00000000000015c0   61756f75 72696f63 6170706c 00000000
                   'auou'   'rioc'   'appl'
```

`'rioc'` is `kAudioUnitSubType_RemoteIO`: the raw path. No AEC, no noise
suppression, no AGC.

**Setting mode `voiceChat` does not change this.** The mode is a hint
about routing and signal tuning; the echo canceller lives in the
voice-processing audio unit, and you only get it if that is the unit
doing the capture. So §2.1, necessary as it is, buys nothing here.

Consequence: an iOS build that goes through `QAudioSource` has the same
speakerphone echo problem the desktop build has — which `VoiceGain.h`
already documents as an open gap — except much worse, because a phone's
speaker and microphone are centimetres apart and the default route is
`defaultToSpeaker`. Anyone not wearing headphones is heard by everyone
else with their own voice echoed back.

#### The three options, assessed honestly

**(a) Ship without it, require headphones.** Free, and genuinely
unacceptable for a phone app whose whole point is casual voice. A
Discord-shaped product where speakerphone is unusable will be reviewed
as broken.

**(b) Port `webrtc-audio-processing`.** Already considered and rejected
in `VoiceGain.h`, for reasons that have not changed: it needs an
ExternalProject Meson build for five targets including the NDK and iOS
toolchains. Adding two more cross-build targets to that is the expensive
version of a problem Apple already solved.

**(c) A native Voice-Processing I/O capture backend. Recommended.**

#### Why (c) should be designed for macOS and iOS together

`kAudioUnitSubType_VoiceProcessingIO` is **not iOS-only** — it exists on
macOS too (AUVoiceProcessing). The desktop AEC gap and the iOS AEC gap
are therefore the *same gap*, and one Objective-C++ capture backend
closes both. That is a considerably better trade than building an
iOS-only path and leaving desktop echo open, and it is the shared design
worth proposing:

```
        AudioWorker  (unchanged: 20 ms int16 frames in, mixed frames out)
              │
      ┌───────┴────────┐
      │  IAudioBackend │   readyRead-shaped capture + bytesFree-shaped sink
      └───────┬────────┘
     ┌────────┴─────────┐
     │                  │
 QtAudioBackend    DarwinVpioBackend        (new, .mm, macOS + iOS)
 (today's          kAudioUnitSubType_VoiceProcessingIO
  QAudioSource/     → AEC + NS + AGC from the OS
  QAudioSink)       → far-end reference is the render callback's own
  Windows, Linux,      output bus, so it is time-aligned by construction
  Android
```

The structural problem to solve is that VPIO is a single
callback-driven unit for both directions, whereas `AudioWorker` is
`readyRead` for capture and a 10 ms `bytesFree()`-metered pump for
playback. The adaptation is a lock-free ring buffer on each side: the
render callback pulls a mixed frame from the playback ring, the input
callback pushes captured frames into the capture ring and signals. The
pump and the frame arithmetic stay exactly as they are — which matters,
because that code is carefully reasoned about (see the drift discussion
in `AudioWorker.cpp`) and should not be rewritten for this.

Note also that the far-end reference signal — the hard part of any
AEC — comes free here: VPIO's own output bus *is* the reference, so
there is no time-alignment problem to solve. That is precisely the part
`VoiceGain.h` flags as the difficulty with a software AEC.

When this lands, `kPlatformVoiceProcessing` flips to true for the
Darwin backend and the software `SpeechAgc` steps aside. Until then it
must stay **false** on iOS, and that is now recorded in `AudioWorker.h`
with the evidence.

**Effort: 6–12 days** for the shared Darwin backend including desktop
macOS. **Unknown** — this is the widest error bar in the document, and
it is the one to start designing early rather than late.

If the release date cannot absorb it, the honest fallback is to ship
voice on iOS with a prominent in-app "use headphones" prompt and a
known-issue note, and treat AEC as the next release. That is a product
call, not an engineering one.

---

## 3. Interruption and lifecycle correctness

### What the client does today

Nothing, on any platform, and this is worth stating plainly because the
Android situation is weaker than it looks.

- **Android** requests `AUDIOFOCUS_GAIN` with a **`nullptr` focus
  listener** (`AndroidAudioRouting.cpp`). The app asks for focus and
  never listens for losing it. `AUDIOFOCUS_LOSS`,
  `AUDIOFOCUS_LOSS_TRANSIENT` and `_DUCK` are all unhandled.
- **`applicationStateChanged` is connected nowhere in `src/`.** That is
  deliberate for Android — `main.cpp` explains that users expect to keep
  talking with the screen off, and the foreground service keeps the
  process alive — but it means there is no existing lifecycle hook to
  extend. The only one is `aboutToQuit` → `leaveAllVoice()`.
- **iOS has nothing at all.** No session, no observers, no recovery.

### What iOS must do

> **Status 2026-09-23.** The events are now consumed. The policy in the
> table below is implemented as a pure state machine,
> `src/voice/DarwinVoiceLifecycle.h`, unit-tested in
> `tests/test_darwin_audio_backend.cpp`, and driven by `AudioEngine`
> which owns the handler as this section recommends. One event was added
> that this table implies but the enum did not have:
> `RouteChangedDeviceLost`, for the `oldDeviceUnavailable` reason, so
> that "unplugging headphones pauses" is a transition rather than a
> comment. **No notification has been observed firing on a device.**

`IosAudioSession.h` defines the events. The required behaviour:

| Event | Cause | Required behaviour |
|---|---|---|
| `InterruptionBegan` | Incoming phone call, Siri, alarm, another app taking the session | The OS has **already** stopped our audio units. Mark the local user muted and *show it* — the worst outcome is a user who thinks they are still in the call. Keep the signalling/peer connections up; only the audio devices are gone. |
| `InterruptionEndedShouldResume` | Interruption over, OS grants resumption | Re-activate the session, reopen `QAudioSource`/`QAudioSink`, clear the mute state, resume. |
| `InterruptionEndedNoResume` | Interruption over, no grant | Stay down. Surface a tap-to-resume affordance. Silently retrying is wrong: the user may still be on a phone call. |
| `RouteChanged` | Headphones in/out, Bluetooth connect/disconnect, speaker override | Re-evaluate. **On iOS this is the only route signal that exists** (§1.5d) — `QMediaDevices` will not fire. Note the `oldDeviceUnavailable` reason specifically: iOS convention is that unplugging headphones **pauses**, it does not switch to speaker. |
| `MediaServicesReset` | Media server crashed and restarted | Every audio object in the process is invalid. Tear down and rebuild the session and both units from scratch. Rare, but it happens, and partial recovery is worse than none. |
| App backgrounded | User switches apps or locks the screen | **Nothing** — that is the entire point of `UIBackgroundModes: audio` (§2.2). The call must survive. This matches the deliberate Android policy in `main.cpp`. |
| App about to be suspended | The OS is out of patience | Existing `aboutToQuit` → `leaveAllVoice()` is the right shape, but it is not guaranteed to run on iOS. The server-side ghost reaper is the backstop. |

The handler runs on the Qt main thread (the observers are registered
against `[NSOperationQueue mainQueue]`), so anything touching
`AudioWorker` must hop to the audio thread — `AudioEngine` already has
exactly the right machinery for this (`BlockingQueuedConnection` for
start/stop) and should own the handler.

**Effort: 2–4 days.** Mostly policy and UI, and it needs a device to
test: a simulator cannot take a phone call.

---

## 4. Camera video on iOS

> **Partly done, 2026-09-23. See §10.** Permission, orientation and
> the codec are built. Front/back switching and mobile rate ceilings
> are NOT — they are still open, and items 1 and 5 below stand.

`src/voice/MacCameraCapturer.mm` is excluded from iOS
(`$<$<AND:$<PLATFORM_ID:Darwin>,$<NOT:$<BOOL:${IOS}>>>`), so an iOS
build lands in `CameraController`'s **`QCamera` branch** — the non-macOS
path — with no permission handling at all.

### Which path to take

The macOS native capturer exists for a reason that **does not apply to
iOS**. The comment in `CameraController.cpp` is explicit: Homebrew's Qt
lacks the `QCameraPermission` plugin, so `QCamera` silently refuses to
start even with TCC granted. On iOS, Qt *does* link
`QDarwinCameraPermission` — because `NSCameraUsageDescription` is
present in `ios/Info.plist.in` at configure time, which is the switch
that makes Qt link it (see the header comment in that file).

So **start with the `QCamera` branch on iOS.** It is the smaller change,
and the reason macOS abandoned it is absent here. Fall back to porting
`MacCameraCapturer` only if `QCamera` misbehaves — and note that porting
it is not free: `AVCaptureDeviceTypeExternalUnknown` (used twice) is
`API_UNAVAILABLE(ios)` and will not compile, and its manual-retain-release
ownership would need revisiting.

### What an iOS camera path needs regardless of branch

1. **Front/back selection and a switch-camera control.** Desktop picks
   "a camera"; a phone has two and the user expects to flip between
   them. Nothing in the current UI does this.
2. **Orientation.** Device rotation must be reflected in the transmitted
   frame (`AVCaptureConnection.videoRotationAngle`, or rotation metadata
   on the encoded stream). Neither the macOS capturer nor the QCamera
   branch handles this; without it remote peers see sideways video every
   time the phone is turned.
3. **Permission.** `CameraController`'s non-macOS branch does nothing.
   iOS needs the `QCameraPermission` request wired, parallel to the
   existing `QMicrophonePermission` handling in `ServerConnection`.
4. **Background stop/restart.** iOS suspends the capture session when
   the app backgrounds and does not resume it automatically. Note this
   is *correct* behaviour to keep: `UIBackgroundModes: audio` covers
   audio only, and continuing to capture camera in the background is
   both impossible and a privacy red flag.
5. **Thermal and battery ceilings.** 720p30 hardware-encoded on a phone
   is sustainable; 1080p may not be. `VideoRateController` exists and
   should get mobile-appropriate ceilings.
6. **VideoToolbox** must be enabled (§1.4), or there is nothing to
   encode the frames with and peers fall back to JPEG.

**Screen sharing stays off.** It needs ReplayKit and a Broadcast Upload
Extension — a separate app target, a separate provisioning profile, and
an inter-process frame pipeline. Out of scope here, and the UI already
hides the control.

**Effort: 3–6 days** for camera capture, on top of §1.4's 1–3 for the
codec. Camera video is the natural thing to cut if the date is tight:
voice without camera is a coherent product, and the mesh negotiates the
absence correctly.

---

## 5. Apple review implications

### Background audio must be genuine — and it is

The rule App Review applies is that `UIBackgroundModes: audio` is only
legitimate if the app actually produces or captures audio while
backgrounded. A voice channel that keeps running while the user checks
another app is exactly the intended use, so the declaration is honest.

What makes it *demonstrably* honest is that it ships together with the
`playAndRecord` session (§2.1). A build with the background mode and no
configured session is both a rejection risk and non-functional; the
build-time guard in §2.2 ties them together so they cannot drift apart.

A reviewer will exercise this. Make sure a call actually survives
backgrounding before submitting — it is a thirty-second test and it is
the first thing they will try.

### Microphone usage string

Already accurate, and now updated for background capture:

> BSFChat uses your microphone so other people in a voice channel can
> hear you speak. Your microphone is only active while you are connected
> to a voice channel — including while BSFChat is in the background, so
> the call continues when you switch apps — and muting stops it.

The background clause is not padding. With the background mode declared,
the microphone genuinely keeps running after the user leaves the app,
and a usage string that implies otherwise is inaccurate in exactly the
way App Review looks for. It also pre-empts the obvious reviewer
question about why the background mode is there.

### Things that are already handled, and should not be re-litigated

- **`ITSAppUsesNonExemptEncryption` is already `true`**, with the
  reasoning written out in `ios/Info.plist.in`. Turning voice on is the
  event that comment was written in anticipation of; the answer does not
  change, which is the whole point of having answered `true` up front.
- **`NSLocalNetworkUsageDescription` is already present**, and the plist
  comment already notes it becomes *mandatory* rather than merely
  correct once voice is on, because the ICE agent gathers host
  candidates on the LAN.
- **`NSCameraUsageDescription` is already present** and already accurate.
- **No new App ID capabilities are needed.** Background modes are an
  Info.plist declaration, not an entitlement. `docs/ios-release.md` §1.2
  says "capabilities to enable: none" and that stays true — unless and
  until someone adds CallKit/PushKit, which would change it.

### Metadata

`docs/ios-release.md` §7 item 8 says the first iOS build is a text-chat
client and that the store listing must not promise voice, or it is a
2.3.1 accurate-metadata rejection. **When voice ships, that note and the
listing have to move together** — in both directions. Screenshots
showing a voice channel in a build where voice is compiled out is the
same rejection from the other side.

---

## 6. What this branch changed

All of it is either verified by a tool or inert by construction. No
existing platform's build output changes: every new call site is an
inline no-op off iOS, and the new `.mm` is only reached under
`IOS AND BSFCHAT_ENABLE_VOICE`, which is still `OFF`.

| File | Change | Verified how |
|---|---|---|
| `src/voice/IosAudioSession.h` | New. The platform seam: `enterVoiceMode()` / `exitVoiceMode()` / `setEventHandler()`, inline no-ops off iOS. Modelled on `AndroidAudioRouting.h`. | Not compiled |
| `src/voice/IosAudioSession.mm` | New. `playAndRecord` + `voiceChat` + `defaultToSpeaker`, preferred 48 kHz / 20 ms, activation, and the three observers. ARC-agnostic ownership via `CFBridgingRetain`/`Release`. | **Not compiled** |
| `src/voice/AudioWorker.cpp` | Calls the seam around `startDevices()`/`stopDevices()`, before `QAudioSource` is constructed. A failed session refuses the join. | No-op off iOS |
| `src/voice/AudioWorker.h` | Records *why* `kPlatformVoiceProcessing` stays false on iOS, with the `'rioc'` evidence. | Comment only |
| `src/voice/VoiceStartPolicy.h` | iOS arm for the microphone-denied message (iOS says "Settings", not "System Settings"). Ordered before the macOS arm. | Preprocessor-isolated |
| `ios/Info.plist.in` | `@BSFCHAT_IOS_BACKGROUND_MODES@` substitution; microphone usage string updated for background capture. | `plutil -lint` clean in **both** configurations |
| `CMakeLists.txt` | iOS OpenSSL fallback hint; `IosAudioSession.mm` in the iOS voice branch; AVFoundation + AudioToolbox frameworks; conditional background-modes emission; `check-ios-bgmode.sh` POST_BUILD guard; the stale "not done yet" comment replaced with the real gap list. | `if`/`endif` balance checked against HEAD; the CMake string escaping rendered with `cmake -P` and fed through the plist validation above |

### What must be verified by building — do not trust any of this yet

1. **`IosAudioSession.mm` compiles.** It has never seen a compiler. The
   ARC/MRC question in particular (the project does not set
   `-fobjc-arc`, and `MacCameraCapturer.mm` calls `-release` while
   `IosAuthSession.mm` is written as if ARC were on) is the kind of
   inconsistency that produces a surprise.
2. **`configure_file` produces the plist CMake thinks it does.** The
   substitution and the resulting XML are both validated in isolation,
   but not end-to-end through a real configure.
3. **`check-ios-bgmode.sh` runs as an Xcode build phase.** The logic is
   verified against real PlistBuddy output; that it fires correctly
   under the Xcode generator is not. The existing plist check has a long
   comment about exactly this class of failure.
4. **The OpenSSL fallback picks the right sysroot.** The
   `CMAKE_OSX_SYSROOT MATCHES "iphonesimulator"` test is reasoned, not
   tested. It is also mostly moot, since both real entry points pass the
   paths explicitly.
5. **Everything in §1.2 (libdatachannel).** Untouched and unknown.

---

## 7. Estimates and recommended order

Working days for one experienced developer with a device. Ranges are
wide because nothing here has been compiled.

| # | Piece | Days | Confidence |
|---|---|---|---|
| 1 | libdatachannel cross-compiles | 1 – 2.5 | Low |
| 2 | opus cross-compiles | 0.25 – 0.5 | High |
| 3 | OpenSSL | 0 – 0.25 | Done |
| 4 | `AVAudioSession` — build + device-verify what is written | 0.5 – 1 | Medium |
| 5 | Background mode — verify | 0.25 | High |
| 6 | Qt audio capture/playback bring-up | 1 – 3 | Medium |
| 7 | `QMediaPlayer` category-stomp guard (§1.5c) | 0.5 – 1 | Medium |
| 8 | Interruption / route / lifecycle | 2 – 4 | Medium |
| 9 | Mobile voice UI + hide the useless device picker | 2 – 4 | Medium |
| 10 | CI: simulator + device, voice ON | 0.5 – 1 | Medium |
| 11 | Integration + two-device testing | 2 – 4 | Low |
| | **Voice, headphones-only (1–11)** | **10 – 21.5** | |
| 12 | **AEC — shared Darwin VPIO backend** | **6 – 12** | **Low** |
| | **Voice, complete (1–12)** | **16 – 33.5** | |
| 13 | VideoToolbox for iOS | 1 – 3 | Medium |
| 14 | Camera capture path | 3 – 6 | Low |
| 15 | Orientation, mobile video layout, rate ceilings | 1 – 2 | Medium |
| 16 | Camera testing | 1 – 2 | Low |
| | **Voice + camera (1–16)** | **22 – 46.5** | |

### The number to plan against

**Voice with working speakerphone: 16–33 working days. Central estimate
about 24, i.e. roughly five calendar weeks.** Adding camera video pushes
it to **22–46 days, central estimate about 34, roughly seven weeks.**

The spread is dominated by two items — AEC (#12) and libdatachannel
(#1). #1 resolves in the first two days and will tighten the whole
estimate sharply; #12 will not tighten until someone has a VPIO unit
running. **If a date has to be committed before either is known, commit
to the top of the range, not the middle.**

Dropping AEC takes the voice-only figure to 10–21 days but ships a phone
app where speakerphone echoes. That is a product decision and should be
made explicitly rather than by schedule pressure.

### Recommended order

**Phase 0 — find out if it builds (days 1–3).** Flip
`BSFCHAT_ENABLE_VOICE=ON` for iOS on a branch and fix the build. Nothing
else. This resolves #1 and #2, which between them own most of the
uncertainty in the early estimate. *Gate: the `ios` CI job is green with
voice ON, for both simulator and device.*

**Phase 1 — make a call happen (days 3–10).** #4, #5, #6, #7, #10. Test
on headphones only and do not chase echo yet. *Gate: two devices in a
voice channel hear each other, with headphones, and the call survives
backgrounding.*

**Phase 2 — make it survive reality (days 10–16).** #8 and #9. Phone
calls, Siri, AirPods in and out, screen lock, app switching. This is the
phase that needs a real device and real interruptions. *Gate: a call
survives an incoming phone call and a headphone unplug without a rejoin.*

**Phase 3 — echo cancellation (days 12–28, overlapping).** #12. **Start
the design during Phase 1**, not after Phase 2 — it is the long pole, it
has the widest error bar, and because it also closes the desktop AEC gap
it has value even if iOS slips. Do not let it be the thing that starts
last.

**Phase 4 — camera (days 28–40), or cut it.** #13–#16. The clean cut
line: voice without camera is a coherent product, the mesh negotiates a
codec-less peer correctly, and the alternative is delaying voice for
video.

**Phase 5 — submission.** Update `docs/ios-release.md` §7 item 8 and the
store metadata in step with whatever actually shipped.

---

## 8. Loose ends noticed in passing

Not blockers, but they will cost someone an hour if they are not
written down.

- **`src/main.cpp`** has `#if defined(BSFCHAT_VOICE_ENABLED) && !defined(Q_OS_IOS)`
  around the media-state announcement block, with the comment "iOS is
  excluded until it grows a capture controller". That gate has to be
  revisited when camera lands, and probably removed.
- **`CameraController.h`**'s class comment claims "`QCamera` works on
  Homebrew's Qt Multimedia build — no native Objective-C++ wrapper
  needed", which is the opposite of what the file now does. Stale since
  the macOS native capturer was added. Worth fixing while someone is in
  there.
- **The QML mic gate** in `ChannelList.qml` keys off `androidPerms`,
  which is registered on every platform and answers `granted = true` off
  Android. So on iOS it passes straight through and the real prompt
  comes from `ServerConnection`'s `QMicrophonePermission` path. That is
  *correct*, just non-obvious — the naming suggests otherwise.
- **`docs/ios-release.md` §5.4** ("The OIDC sign-in flow is
  desktop-shaped and will not work on iOS") is stale: `IosAuthSession.mm`
  landed. Unrelated to voice, but it is in the file the next person will
  read alongside this one.

## Build status: it compiles (2026-09-23)

The iOS voice build was configured and compiled for the first time on
2026-09-23, against Qt 6.10.3 for iOS and the iOS 26.5 SDK:

    ./scripts/build-openssl-ios.sh device
    qt-cmake -B build-ios-voice -G Xcode \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=iphoneos \
        -DBSFCHAT_ENABLE_VOICE=ON -DOPENSSL_USE_STATIC_LIBS=ON \
        -DQT_HOST_PATH=~/Qt/6.10.3/macos ...
    xcodebuild build -scheme bsfchat-app -configuration Release \
        -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO

Result: **BUILD SUCCEEDED**, zero errors, arm64, 70 MB .app, with
`UIBackgroundModes = (audio)` in the rendered Info.plist.

This closes the largest open question in the estimate above.
libdatachannel, opus and usrsctp were listed as the unknown that owned
the error bars; they cross-compile through the iOS toolchain with no
patching at all.

**One fix was needed**, and it is not discoverable from the error
message. libdatachannel's `CMakeLists.txt` has an Apple branch that
FORCEs `OPENSSL_SSL_LIBRARY` / `OPENSSL_CRYPTO_LIBRARY` to `.dylib`
paths unless `OPENSSL_USE_STATIC_LIBS` is set. `build-openssl-ios.sh`
configures `no-shared`, so those dylibs do not exist, `find_package`
creates no `OpenSSL::SSL` target, and the configure dies with
"Target datachannel-static links to OpenSSL::SSL but the target was not
found" — naming OpenSSL without naming the reason. `CMakeLists.txt` now
sets `OPENSSL_USE_STATIC_LIBS` in the iOS block, so this is handled;
the note is here because the same trap waits on any other static-only
Apple target.

**What this does NOT tell us.** Compiling is not working. Nothing on the
iOS voice path had executed a single instruction at the time this was
written. That changed the same day — see §9.

---

## 9. Device test, and the echo-cancellation backend (2026-09-23)

### What a real iPhone proved

Voice was run on an **iPhone 16 Pro Max**. It works:

- calls connect and **carry audio in both directions**;
- the **microphone permission prompt appears at first join**, which is
  the correct lazy behaviour and means §2.1's session configuration is
  doing its job;
- **the one clear defect is echo.** On speakerphone the far end hears
  themselves, exactly as §2.3 predicted from the `'rioc'` finding.

So §2.3 stops being a prediction. It is an observed defect with a known
cause.

### What was built in response

Option (c) of §2.3, as specified there: one native capture **and
render** backend on `kAudioUnitSubType_VoiceProcessingIO`, shared by
macOS and iOS, which closes the desktop AEC gap (`VoiceGain.h`) at the
same time.

| File | What it is |
|---|---|
| `src/voice/AudioBackend.h` | The `IAudioBackend` seam from §2.3's diagram |
| `src/voice/QtAudioBackend.{h,cpp}` | Today's `QAudioSource`/`QAudioSink`, moved behind it unchanged |
| `src/voice/DarwinVpioBackend.{h,mm}` | The VPIO unit, both directions, both Apple platforms |
| `src/voice/AudioRingBuffer.h` | Lock-free SPSC ring: CoreAudio callback ↔ the 10 ms pump |
| `src/voice/VpioFormat.h` | Format negotiation as a pure decision |
| `src/voice/DarwinVoiceLifecycle.h` | §3's table as a pure state machine |
| `src/voice/AudioBackendSelect.h` | Which backend, and why, as a pure decision |

The crux is the one §2.3 names: **playback had to move into the same
unit.** VPIO's canceller takes its far-end reference from its own output
bus, so a VPIO capture path feeding a `QAudioSink` would have been all of
the cost and none of the benefit.

`AudioWorker`'s pump, frame arithmetic and device policy are untouched,
which was the other requirement in §2.3. The only change to its shape is
that a polled backend is drained from the pump rather than from
`readyRead`, because a CoreAudio callback cannot call into a `QObject`.

### Two off switches, deliberately

`BSFCHAT_DARWIN_VPIO` (CMake, ON by default on Apple) compiles the
backend out. **Settings → Audio → Echo cancellation** (`audio/voiceProcessing`,
on by default) chooses at join time. A VPIO unit that fails to start
demotes itself to the Qt path for the rest of the process.

Both exist because the processing is a *preference* as well as a fix:
noise suppression and AGC tuned for a phone will be heard chewing word
tails by someone on a good microphone in a treated room.

### What is verified, and what is not

**Verified:** it compiles and links for arm64-iphoneos (70 MB .app) and
for macOS; the full client test suite passes (48/48) with 30 new cases
covering format negotiation, the ring, the lifecycle machine and the
backend choice.

**Not verified — all of it needs the phone:**

1. **That the echo is gone.** The whole point, and entirely untested.
2. That capture and playback flow through the unit at all. A silent call
   is the failure mode if the rings or the callbacks are wrong.
3. Format negotiation on real hardware. The code asks the unit for
   48 kHz mono int16 and converts if refused; which branch iOS actually
   takes is unknown.
4. Every lifecycle event. A simulator cannot take a phone call.
5. macOS per-element device selection. `kAudioOutputUnitProperty_CurrentDevice`
   set per element is what Chromium does, but Apple documents the
   property as global-scope; if VPIO ignores the element, the symptom is
   the output device setting being ignored while AEC is on.

---

## 10. Camera video on iOS, built (2026-09-23)

Branch `feat/ios-video`, on top of `feat/darwin-aec`. Voice already ran
on a real iPhone 16 Pro Max (§9); this is the video half.

**Status: it builds for arm64-iphoneos and every pure-logic test passes.
Not one frame has been captured or encoded on a phone.** Everything in
"What a device has to answer" below is genuinely open.

### 10.1 What changed

**VideoToolbox is built for iOS.** The gate in `CMakeLists.txt` went
from `if(APPLE AND NOT IOS)` to `if(APPLE)`. That was the whole reason
an iOS build advertised no codecs: with openh264 and libaom correctly
excluded on mobile, there was no video backend compiled in at all, so
`h264EncodeProfiles()` and `h264DecodeProfiles()` both returned empty,
`localCapsJson()` published `video_codecs: []`, and every peer in the
mesh was routed to the legacy JPEG stills path — while the phone itself
could not send video at all, because `VideoEncoder::create()` had
nothing to return.

Confirmed on the built binary: `bsfchat-app` for `iphoneos` links
`VideoToolbox`, `CoreMedia`, `CoreVideo`, and carries
`MacVTEncoder`/`MacVTDecoder` symbols and the `VTCompressionSession*` /
`VTDecompressionSession*` imports.

`LatencyCriticalActivity.mm` stays macOS-only — its header defines the
methods inline off macOS, so compiling it on iOS is a redefinition, and
what it opts out of (App Nap, desktop timer coalescing) has no iOS
meaning.

**The encoder's three iOS-specific defects, all as predicted in §1.4:**

1. `kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder`
   is now macOS-only (`BSFCHAT_VT_WANT_HW_SPEC`, keyed on
   `TARGET_OS_OSX`), at both sites. On iOS the hardware encoder is the
   only encoder; the key is `ios(17.4)` against a deployment target of
   17, so referencing it would be a weak-linking hazard for a question
   with one possible answer.
2. Input is now **NV12 from an IOSurface-backed pool** instead of a
   freshly-allocated tri-planar `CVPixelBuffer` per frame. The session's
   own pool (`VTCompressionSessionGetPixelBufferPool`) is used where it
   exists, with a hand-allocated IOSurface-backed buffer as the fallback;
   the I420→NV12 interleave is `src/voice/video/NV12Pack.{h,cpp}`,
   which is pure and unit-tested against padded destination strides.
   **Applied on macOS too, deliberately** — NV12 is VideoToolbox's
   native input on both platforms, and sharing the path means the Mac,
   which gets used daily, exercises the code the phone runs.
3. `kVTInvalidSessionErr` is handled. iOS reclaims the compression
   session when the app backgrounds; every call after that fails, and
   nothing upstream could see why — `VideoSendPipeline` only learns that
   `encode()` returned false, treats it as a bad frame, and retries
   forever. `encode()` now rebuilds the session once and retries with a
   forced keyframe. One retry, not a loop: a second failure is not the
   session, and the pipeline's software-fallback path is the right next
   move.

The decode side needed nothing: `Result::Error` already causes
`VideoReceivePipeline` to reset the decoder, arm the keyframe gate and
request an IDR, which is exactly right for an invalidated session — and
it does *not* reach `onDecoderUnavailable`, so a backgrounded app cannot
latch `markH265DecodeBroken()` and retract H.265 for the rest of the
process. That distinction is invisible from `MacVTDecoder.mm` and is now
written down there.

**`VTCompressionSessionCompleteFrames` per frame was left in place.**
It drains the hardware pipeline every frame and is real waste on a
phone, but removing it means making `VideoEncoder::encode()`
asynchronous for every backend on every platform. That is a different
change and should be made against a measurement, not blind.

**Camera permission is lazy, and structurally so.** The rule is
`src/voice/CameraPermissionPolicy.h` (pure, unit-tested); iOS answers it
through `QCameraPermission`, macOS keeps its AVFoundation shim because
Homebrew Qt has no permission plugin, and every other platform reports
`Unsupported` and behaves exactly as before. Two things make "lazy" a
property rather than an intention:

- The policy is consulted from exactly one place —
  `CameraController::startForCamera()` — which is only reachable from
  the dock's camera button.
- `CameraController`'s constructor no longer builds a `QCamera`. The
  capture objects are created in `ensureCaptureSession()` on first
  start, and device enumeration moved there too. The constructor runs at
  launch on every platform, so anything it does happens on the splash
  screen; this is the same class of bug a worker fixed on Android on
  2026-09-22, where a startup camera prompt would have failed review.

`androidPerms.hasCamera()` returns true off Android, so `VoiceDock.qml`
short-circuits its Android branch on iOS and calls `camera.start()`
directly — no QML change was needed, and the iOS prompt is raised inside
the controller.

`NSCameraUsageDescription` was reviewed and left as written: it names
the feature and the fact that the camera is only on while the user
chooses to share video, which is accurate for what now ships. It is
notably *not* the microphone string — there is no background clause,
because camera capture genuinely stops when the app backgrounds.

**Orientation is baked into the pixels at the sender.** The rule is
`src/voice/video/VideoFrameOrientation.h`. Qt stamps a presentation
rotation (and a mirror flag) on every captured `QVideoFrame` and applies
both when *drawing*; the pixels are untouched, so everything that reads
them — `FrameConverter::toI420` for the encoder, `QVideoFrame::toImage()`
for the legacy JPEG path — got a sideways picture, and H.264 over RTP
carries no orientation metadata for a receiver to undo it with.

Both paths now apply the rotation. The alternative — RTP's CVO header
extension — is the "proper" answer and was rejected because it needs
every receiver to implement it, and this mesh has desktop peers on three
platforms plus a JPEG fallback; a rotation the old peers ignore is a
rotation that does not happen. Scaling happens before rotating (a
quarter turn leaves the long edge unchanged, so the output size is the
same and the rotation touches far fewer pixels).

**Mirroring is deliberately not applied to the wire.** The front camera
is mirrored for the local preview because people expect their own image
to behave like a mirror; the far end is looking at you, not at
themselves, and would otherwise read your T-shirt backwards. Qt gives
both for free — the preview goes through `QVideoSink`, which applies the
flag, and the encoder gets raw pixels that never had it applied.

**`scripts/build-ios.sh` no longer forces voice off.** It defaulted
`BSFCHAT_ENABLE_VOICE` to `OFF` and passed it unconditionally, which
outranks an `option()` default — so the commit that turned voice on for
iOS had no effect through the script that device builds are actually
made with. **This was caught by building: the first iOS binary of this
branch linked no VideoToolbox at all.** The script now passes the flag
only when the caller sets one.

**An iPhone's camera state is announced to the roster.** The
`setLocalMediaState` block in `main.cpp` excluded iOS because there was
no capture controller there; without it an iPhone's video tile would
never appear on anyone's roster until the first frame landed (the S-7
failure shape). The screen half is a compile-time `false` — there is
still no iOS screen share.

### 10.2 Tests

`tests/test_mobile_video.cpp` (17 cases, all passing), covering the
orientation rule and its effect on `FrameConverter` (including the
dimension swap that `VideoSendPipeline` sizes the encoder from), the
NV12 packing against padded strides, the permission policy, and what the
build advertises and negotiates — built the same way `localCapsJson()`
builds it and run through the real `peerCanReceiveRtpVideo` /
`peerNeedsLegacyJpeg` predicates, with the old codec-less shape kept as
an explicit contrast.

Nothing in it opens a camera, a microphone, a screen or an encode
session, so it runs on the owner's Mac without a TCC prompt.

The existing `test_video_codec` VideoToolbox round-trips
(`vtEncodesOpenh264Decodes`, `openh264EncodesVtDecodes`) pass on the new
NV12/pool path, which is real evidence that the encoder change produces
a valid bitstream — on a Mac.

### 10.3 What a device has to answer

None of this can be checked without a phone:

1. **Does Qt's iOS AVF backend stamp a rotation on the frame at all?**
   The whole orientation fix rests on it. If it reports `None` for every
   device orientation, the picture is upright only when the phone is
   held the way the sensor likes, and the fix has to move to
   `AVCaptureConnection.videoRotationAngle` — which Qt does not expose,
   and which would mean porting the capturer after all.
2. **Does the hardware encoder accept the pooled NV12 buffers?** §1.4
   called this the most likely functional failure on iOS. A Mac proves
   the format is right; it does not prove the pool is.
3. **Does the prompt appear at first video use and nowhere else?**
4. **What happens across a background/foreground cycle?** The
   invalidation retry has never run.
5. **Thermals.** 720p30 hardware-encoded should be sustainable. Nobody
   has measured it.

### 10.4 Left undone, on purpose

- **Front/back camera switching.** §4 item 1 stands. A phone has two
  cameras and the dock has no control to flip between them; the button
  starts the default one. This is the most obvious remaining user-facing
  gap.
- **`VideoRateController` was not retuned.** It was overhauled recently
  for desktop and a phone breaks several of its assumptions; retuning it
  blind would be worse than leaving it. The specific breaks, for whoever
  picks this up:
  - **Encode pressure cannot fire.** `VideoSendStats.h` declares an
    encode bottleneck when convert+encode takes ≥90 % of a frame
    interval. With a fixed-function encoder that time measures submit
    latency, not load, and sits near zero while the SoC throttles — so a
    thermally-throttled phone never declares itself encode-bound. The
    upshift headroom model compounds this: it predicts cost as
    quadratic in the long edge, which is true for software motion search
    and false for hardware.
  - **A camera that drops frames is classified `Idle`, not `Capture`.**
    `CameraController` passes `capturePolled=false`, so the capture
    branch in `VideoSendStats` is skipped entirely. On a phone, dropping
    from 30 to 15 fps is routine — thermal throttle, or low light
    extending exposure — and `Idle` is excluded from the sender-short
    path, so the bitrate is not scaled down. Bytes per frame double,
    packets per frame double, and the frame-damage model then amplifies
    any loss. This is the sharpest latent bug of the set.
  - **The resolution ladder sheds encode bits, not capture cost.**
    Nothing selects a `QCameraFormat`; the camera keeps running its
    native format and libyuv scales afterwards. On a phone that is
    battery and heat the ladder cannot reclaim.
  - **Back-off is bounded to 15 %/tick with a "never below 70 % of
    recent goodput" ratchet.** That is tuned for a WiFi hiccup. A
    cellular handover drops capacity by an order of magnitude instantly,
    and the ratchet actively prevents the fast collapse that needs.
    There is also no network-change hook: `resetState()` is called only
    from `setActive(true)`, so a knee learned on one cell survives the
    handover and takes ~20 s to climb back through.
  - **Nothing thermal anywhere.** No `NSProcessInfo` thermal state, no
    low-power-mode check, no hook to feed one into. The controller has
    exactly two inputs: receiver delivery reports and the sender window.
- **Screen sharing on iOS.** Still needs ReplayKit and a Broadcast
  Upload Extension. Unchanged.

### 10.5 Portrait-only, and what it does to the orientation question

The app ships portrait-only on both platforms now (`feat/mobile-ui-polish`:
`screenOrientation="portrait"`, and `UISupportedInterfaceOrientations`
reduced to `UIInterfaceOrientationPortrait`). That is independent of the
sender-side rotation here — locking the UI does not turn the sensor —
but it changes the shape of the problem in a useful way, and it changes
what is worth testing.

**It makes the problem strictly simpler.** There is now exactly one
interface orientation the app can ever be in, so whatever angle the
capture arrives at, it is a *constant*. The fix, if one is needed, is a
single number rather than a mapping table maintained against four
orientations.

**The evidence says a fix probably IS needed.** Qt 6.10.3's concrete iOS
camera backend is `AVFCameraSession` — the shared Darwin one — and it
carries no orientation or rotation method at all: no
`videoRotationAngle`, no `videoOrientation`, nothing that consults the
screen. If that reading is right, `QVideoFrame::rotation()` is always
`None` on iOS, `videoorient::wireRotation()` returns 0, and the
orientation code added here is in place and doing nothing while the far
end gets a sideways picture. This is static reading of a binary, not a
measurement; §10.3 item 1 is still the open question.

**The local preview is a free oracle for it.** Qt applies the same
presentation rotation when it *draws* a frame through `QVideoSink`. So
if Qt stamps nothing, the preview is sideways too — inside a portrait
UI, which is unmissable. That means:

- **Preview sideways in the portrait UI ⇒ Qt stamps nothing.** The
  rotation has to come from somewhere else, and the `[camera] first
  frame:` log line says which way round to apply it.
- **Preview upright ⇒ Qt stamps a rotation**, the wire gets the same
  one, and the far end should be upright too.

Either way the log line settles it on the first build, which is why it
was added rather than leaving the question for a second round trip.

**The "rotate to landscape and back" test no longer applies as written**
— the UI cannot rotate. Replace it with a *tilt* observation, which
answers a different and still-open question: whether Qt derives any
rotation it does stamp from the INTERFACE orientation (locked, so
constant) or from the physical DEVICE orientation (still free to change
in a locked app). Tilt the phone to landscape while watching your own
preview: if the preview rotates inside the portrait UI, Qt is keyed on
the device and the wire angle moves with the user's wrist; if it stays
put, the angle is constant and there is exactly one number to get right.
