# Hardware H.264 video on Android (MediaCodec)

## 0. How confident each claim is

| Claim | Confidence |
| --- | --- |
| An Android build compiled no video backend at all before this | **Certain** — read off the build |
| It now compiles one, links `libmediandk`, and carries both classes | **Certain** — verified on the built `arm64-v8a` `.so` |
| `h264EncodeProfiles()` / `h264DecodeProfiles()` come back non-empty on a device | **High** — the code path is compiled in and the probes are the only gate; not run on hardware |
| The buffer-geometry and Annex-B logic is correct | **High** — 11 host-run cases, mutation-checked |
| The encoder comes up on a Pixel 6 Pro | **Certain** — it did, on the first device run |
| A frame actually encodes on a Pixel 6 Pro | **Unverified.** The first device run refused every frame; §3.1 is the fix, not yet re-run on hardware |

## 1. The failure this closes

`src/voice/video/` had four backends: `MacVTEncoder/Decoder` (Apple),
`MFEncoder/Decoder` (Windows), `OpenH264Encoder/Decoder` (Linux, plus the
macOS software fallback) and `AomLossless*` (AV1). None of them is Android.
`cmake/Dependencies.cmake` excludes openh264 and libaom on mobile — correctly,
they are CPU codecs and a phone cannot afford one — and the two hardware
backends are Apple-only and Windows-only.

So an Android build compiled **no video backend**, and the consequences fanned
out well past the phone:

* `VideoEncoder::h264EncodeProfiles()` and `VideoDecoder::h264DecodeProfiles()`
  both returned empty lists.
* `VoiceEngine::localCapsJson()` therefore published `video_codecs: []`.
* `peerCanReceiveRtpVideo()` was false for the phone, so `peerNeedsLegacyJpeg()`
  was true, and the **sender dropped the whole call to the legacy JPEG stills
  path — for every participant, not just the phone.** Observed as
  `[screenshare] broadcast 164836-byte JPEG` repeating in the desktop log:
  roughly 165 KB per still, several per second, about 6 Mbps for a slideshow.
* `VideoEncoder::create()` had nothing to return, so the phone could not send
  video either. That is why the camera tile read "Starting camera…" on the far
  end. Mobile camera was not unreliable; it was unimplemented.

This is the same hole iOS had, closed the same way, days apart — see
`docs/ios-voice.md` §1.4 and commit `9b88524`.

## 2. NDK `AMediaCodec`, not JNI to `android.media.MediaCodec`

`minSdk` is 28. Both bindings were available; the NDK one was chosen.

**For it.** No JNI anywhere on the encode or decode path. A `VideoEncoder`
instance is owned and driven by one `VideoSendPipeline` worker thread, which is
a `QThread` this app created — a JNI binding would have to
`AttachCurrentThread` it, hold global refs to the codec and its `ByteBuffer`s,
manage a local-ref frame per call at 30 Hz, and detach cleanly on teardown.
Each of those is a way to leak or crash that has nothing to do with video. It
also keeps the codec out of the Qt package source dir: no new Java class, no
proguard rule, no `androiddeployqt` surprise, and no involvement with the class
loader this platform already fights (see `921715c`).

**Against it, and the reason it is written down.** The NDK has no
`getInputImage()` / `getOutputImage()`. In Java, `COLOR_FormatYUV420Flexible`
hands back an `android.media.Image` whose `Plane`s carry their own row and pixel
strides, and the buffer layout answers itself. The NDK gives a flat `uint8_t*`,
so the layout has to be **reconstructed** from the `MediaFormat`'s
`color-format`, `stride` and `slice-height` keys. That reconstruction is the
riskiest part of this port, which is why it lives in `MediaCodecLayout.h/.cpp`
— pure, NDK-free, and host-tested — rather than inline in the backend.

Everything else needed is at or below API 28: `AMediaCodec_getInputFormat` (the
layout readback) is API 28 exactly, `AMediaCodec_setParameters` (live bitrate,
forced IDR) is 26, the rest is 21.

## 3. Colour formats

`FrameConverter` produces tightly-packed I420. The iOS path settled on NV12
(`NV12Pack.h`) because that is what Apple encoders consume; Android has no
single answer, so the backend resolves one per session:

1. Ask for `COLOR_FormatYUV420Flexible` — the portable ask, accepted by every
   codec since API 21.
2. Read `AMediaCodec_getInputFormat()` back and resolve `color-format`,
   `stride` and `slice-height` into a `BufferLayout` (`I420`, `NV12`, or
   `Unsupported`).
3. If the codec echoed Flexible back without resolving it, or named a layout no
   linear copy can read, drop to explicitly requesting `YUV420SemiPlanar`, then
   `YUV420Planar`.

`stride` and `slice-height` are honoured separately from width and height,
because a codec buffer's rows are padded to the hardware's alignment and its
chroma planes start at an independently padded row offset. **1080 is routinely
1088 here**, and taking `height` literally is the single most common way to get
a band of garbage across the bottom of a 1080p frame. Qualcomm's `64x32` tiled
format is refused by name — it sits numerically between two vendor semiplanar
formats that *are* plain NV12, and a linear row copy of it is confetti.

The decoder emits `Format_NV12` `QVideoFrame`s, the same pixel format
`MacVTDecoder` emits, so the `QVideoSink` playout path needs no Android branch.

### 3.1 The 62 bytes — reported geometry is not the allocation

The first device run (Pixel 6 Pro, Tensor G1) came up and then refused every
single frame:

```
W bsfchat.video.mediacodec: input pack refused (capacity 614338, need 614400)
```

`614400` is `640 × 640 × 3/2`: the codec resolved to stride 640, slice-height
640. The buffer it actually handed back was **62 bytes short of that**.

The shortfall is not a rounding quirk to clamp away. `614338` is **not
divisible by 3**, so it is not a 4:2:0 frame size at all — a 4:2:0 buffer is
`3 × (w·h/2)` and is always divisible by 3. It is whatever the Codec2 graphic
allocator rounded to. **A codec's reported geometry and its allocation are two
different facts**, and requiring them to agree was never justified.

What *is* justified is that every byte written lands inside the buffer. libyuv
writes `width` bytes per row over `height` rows — never the row padding, never
the rows between `height` and `slice-height`. The frame here is **480×640**:
`FrameConverter` rotates the 640×480 sensor frame upright and
`VideoSendPipeline` sizes the encoder from the rotated dimensions (see
`FrameConverter.h`). So the last byte actually touched is

```
NV12:  640·640 + (640/2 − 1)·640 + 480 − 1  =  614239   → needs 614240
```

which fits in 614338 with 98 bytes to spare. The old rule over-demanded by 160
bytes and was refused by 62. `BufferLayout::requiredBytes(w, h)` is that number;
`sizeBytes()` is kept for the plane extent and is no longer used as a bound.

Two things follow, both implemented:

* **The bound is now the bytes written, on both sides.** The decoder's output
  buffer is sized by the same allocator and can be short for the same reason.
* **The encoder measures a real input buffer before accepting a colour format.**
  `probeInputCapacity()` dequeues one buffer at session open, reads its
  capacity, and hands it back via `flush()`. If it cannot hold
  `requiredBytes()`, that rung of the ladder is rejected and the next colour
  format is tried; if none fits, `init()` **fails**. A codec that starts and
  then drops every frame is the worst available outcome — see §3.2.

`AMEDIAFORMAT_KEY_MAX_INPUT_SIZE` was considered and not used: for raw encoder
input under Codec2 the buffer is a graphic block, not a linear one, so the key
is generally ignored — and guessing a value that a codec *did* honour could make
the allocation smaller rather than larger.

### 3.2 Why "comes up, then refuses everything" is worse than failing

There is no JPEG safety net underneath a broken encoder. `CameraController`'s
legacy path is gated on `voice->hasLegacyOpenPeers(VideoStreamId::Camera)`,
which is true only while a peer *cannot* receive RTP video or its track has not
opened. Once the phone advertises `h264` — which is the whole point of this port
— the desktop's track opens, `hasLegacyOpenPeers` goes false, and the JPEG
branch correctly stops running.

So a phone that advertises H.264 and then produces no access units sends
**nothing at all**, which is strictly worse than the JPEG slideshow it replaced.
That is what the owner saw. The answer is not to re-arm JPEG behind a working
advertisement — that would put the mesh back on 6 Mbps stills — but to make the
encoder either work or refuse to start, which is what the probe does.

## 4. Pipeline depth

VideoToolbox is made one-in-one-out with `VTCompressionSessionCompleteFrames`.
MediaCodec has no cheap equivalent — it is a queue in and a queue out, and a
hardware encoder legitimately holds one to three frames. Draining per frame
would mean `flush()`, which discards state and forces an IDR: a slideshow with
extra steps.

So `encode()` queues its input, drains whatever output is ready into a FIFO, and
returns the **oldest** access unit. Steady state is one-in-one-out with a
constant delay of a frame or two; only the first frames of a session return
false, which `VideoSendPipeline` already handles (it re-arms a pending keyframe
request and moves on). No frame is dropped and none is reordered.

Two consequences, both correct and both unlike the Apple backend:

* `out.captureTimeUs` comes from the codec's presentation timestamp, not from
  the frame just handed in — they are different frames.
* A `forceKeyframe` request surfaces as a keyframe a frame or two later than
  asked. `AMEDIAFORMAT_KEY_LATENCY=1` asks the encoder to keep that short.

## 5. Reclaim

Android takes codecs back. A `MediaCodec` instance is a handle on a shared,
foreground-biased hardware resource, and the framework reclaims it when the app
backgrounds or a higher-priority client (the camera app, a phone call) wants it
— after which every call on that instance fails forever.

This is the same shape as iOS's `kVTInvalidSessionErr` and needs the same
answer, for the same reason: nothing upstream can see it. `VideoSendPipeline`
only learns that `encode()` returned false, treats it as a bad frame, and
retries with the next one forever — the user comes back to the app with the
camera light on and nobody able to see them. The encoder rebuilds and retries
**once**, forcing a keyframe; if a fresh codec also fails, the failure is not
the codec.

The decoder needs no equivalent. `Result::Error` already causes
`VideoReceivePipeline` to `reset()` (tearing the dead codec down), arm the
keyframe gate and request an IDR, and it does **not** reach
`VoiceEngine::onDecoderUnavailable` — that edge is raised by a failed
`VideoDecoder::create()`, not a failed decode — so a backgrounded app cannot
latch a codec as broken for the rest of the process.

## 6. What this build advertises

| | Android | Why |
| --- | --- | --- |
| `video_codecs` | `["h264"]` | Both probes pass |
| `h264_profiles_encode` | `["cb"]` | A claim, not a limit — see below |
| `h264_profiles_decode` | `["cb", "high"]` | CDD-required since API 21 |
| `h265` | never | No trustworthy encode/decode probe below API 36 |
| `lossless` | never | libaom is excluded on mobile |

The encode/decode asymmetry is deliberate. `negotiatedH264Profile()` is a
whole-call decision needing unanimity among viewers, so:

* **Not claiming High on encode** costs nothing: requesting a profile on an
  Android encoder needs a matching level in the same format, the pair is
  rejected outright by encoders that do not implement it, and there is no NDK
  capability query below API 36 to check either against. Asking for High is a
  hard `configure()` failure — no video at all from that phone — to fix a
  bitrate inefficiency. The encoder is left on its default (Baseline or
  Constrained Baseline everywhere in practice).
* **Claiming High on decode** matters a great deal: declining it would drag the
  *whole call* to Baseline the moment a phone joined, a permanent quality cost
  on every other participant, to hedge against a device class that does not
  exist.

## 7. What is tested, and what is not

`tests/test_mobile_video.cpp` grew 11 cases (§5 of that file) covering
`MediaCodecLayout` and `MediaCodecAnnexB`: the two planar families and their
vendor spellings, the tiled and Flexible refusals, absent and nonsensical
padding keys, packing into a padded codec buffer in both families, unpacking
that drops padding, a round trip cross-checked against the independently
written `NV12Pack`, and the Annex-B cases the encoder's "does this keyframe
already carry its parameter sets" decision rests on.

They run **on the desktop host**, unconditionally, like `NV12Pack.cpp` before
them. That is not a convenience: the Android CI job passes
`-DGAMECHAT_CLIENT_BUILD_TESTS=OFF` and a hosted runner has no phone, so
guarding them behind `if(ANDROID)` would ship the riskiest arithmetic in the
port with zero coverage. Verified by mutation — ignoring the codec's vertical
padding fails three of them, including the 1080→1088 case.

Nothing in them opens a codec, a camera or a network socket.

## 8. What only a device can answer

1. ~~Which colour format a Tensor G1 encoder resolves Flexible to~~ —
   **answered**: it resolves, at stride 640 / slice-height 640 for a 480×640
   frame, and reports both keys. What is still open is whether the resolved
   family is NV12 or I420; the run predates the log line that says so.
2. **Whether a frame encodes.** The first run got as far as a started codec and
   no further (§3.1). The bound is fixed and the probe is in, but nothing here
   proves the hardware accepts what is packed for it — only a device does.
3. **Real pipeline depth.** The FIFO copes with any depth, but a deep encoder
   costs latency the rate controller was not tuned for.
4. **Reclaim on background.** Needs the app backgrounded mid-call and brought
   back, and the "rebuilding" log line to appear exactly once.
5. **Decode of a desktop High-profile stream.** Advertised, universally
   supported in practice, never exercised here.
6. **Whether the phone's camera delivers frames at all.** Separate from this
   work — `VideoEncoder::create()` now returns something, which is necessary and
   not sufficient. See `fix/mobile-camera-stream`.

## 9. Not done

* **H.265.** Android hardware HEVC encoders are common but not universal, and
  there is no NDK capability query below API 36 to tell the devices that have
  one from the devices that fail at share time. `init()` refuses it outright so
  the advertisement and the behaviour cannot drift apart.
* **`VideoRateController` retuning.** Exactly as listed for iOS in
  `docs/ios-voice.md` §10.4, and for the same reasons: encode pressure cannot
  fire against a fixed-function encoder, the ladder sheds encode bits but no
  capture cost, back-off is tuned for a WiFi hiccup rather than a cellular
  handover, and nothing anywhere is thermal. Retuning it blind would be worse
  than leaving it.
* **Surface input.** Feeding the encoder a `Surface` from the camera or from
  `MediaProjection` would skip the CPU I420 round trip entirely. It is the right
  destination and a much larger change: it bypasses `FrameConverter`, and with
  it the orientation baking (`VideoFrameOrientation.h`) that every receiver
  depends on.
