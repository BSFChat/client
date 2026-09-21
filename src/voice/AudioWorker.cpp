#include "voice/AudioWorker.h"
#include "voice/AudioMixer.h"
#include "voice/JitterBuffer.h"
#include "voice/AndroidAudioRouting.h"
#include "core/AppProfile.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QAudioDevice>
#include <QDateTime>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <utility>

// The by-description lookups that used to live here, and the
// QMediaDevices calls behind them, are gone: resolving a device is now
// AudioDevicePolicy's job, it happens against a snapshot AudioEngine
// takes on the GUI thread, and it happens on every change rather than
// once at join. See the "Live device changes" note in AudioWorker.h.

using bsfchat::voice::AudioPacketQueue;

AudioWorker::AudioWorker(std::shared_ptr<AudioPacketQueue> queue,
                         QObject* parent)
    : QObject(parent)
    , m_queue(std::move(queue))
    , m_mixer(new AudioMixer(kFrameSamples))
{
    m_captureFrame.assign(kFrameSamples, 0);
    m_captureFloat.assign(kFrameSamples, 0.0f);
    m_captureOut.assign(kFrameSamples, 0);
    m_playbackFrame.assign(kFrameSamples, 0);
    m_peerFrame.assign(kFrameSamples, 0);
}

AudioWorker::~AudioWorker() {
    // stopDevices() is expected to have run on the audio thread already
    // (AudioEngine blocking-invokes it before quitting the thread). Call
    // it again defensively: it is idempotent, and if we somehow get here
    // with devices still open it is better to leak nothing than to leak
    // an encoder and N Opus decoders.
    stopDevices();
    delete m_mixer;
}

bool AudioWorker::startDevices() {
    if (m_started) return true;

    // Flip Android into VoIP mode + route to speakerphone BEFORE we
    // open QAudioSource. Done in MODE_NORMAL, Android routes capture
    // to a "normal" profile that prefers earpiece for playback and
    // skips hardware AEC — switching the mode after the device is
    // open doesn't re-plumb the routing graph. No-op off Android.
    bsfchat::audio_routing::enterVoiceMode();

    // Initialize Opus encoder
    int err;
    m_encoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !m_encoder) {
        qWarning("Failed to create Opus encoder: %s", opus_strerror(err));
        m_encoder = nullptr;
        bsfchat::audio_routing::exitVoiceMode();
        return false;
    }
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(32000));
    opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    // Audio format: 48kHz, mono, 16-bit signed. A member, so a mid-call
    // device switch can reopen without rebuilding it.
    m_format = QAudioFormat();
    m_format.setSampleRate(kSampleRate);
    m_format.setChannelCount(kChannels);
    m_format.setSampleFormat(QAudioFormat::Int16);

    // Honour the user's selection from Client Settings → Audio; fall back
    // to the OS default if the saved preference isn't present (device
    // unplugged, renamed, etc).
    // Named through AppProfile so a --profile instance reads its own
    // device preference — the same store Settings writes to.
    //
    // Read once, here: these are the user's stated choice, and the
    // settings dialog says a new choice applies on the next join. What
    // IS re-evaluated live is which real device the choice resolves to,
    // which is a different question and the one that was broken.
    QSettings prefs(bsfchat::organizationName(), bsfchat::applicationName());
    m_preferredInput  = prefs.value("audio/inputDevice").toString();
    m_preferredOutput = prefs.value("audio/outputDevice").toString();
    // Tie-breaker only, for the case where several devices share one
    // description. Written by Settings when the user picks a device.
    m_inputHintId  = prefs.value("audio/inputDeviceId").toString();
    m_outputHintId = prefs.value("audio/outputDeviceId").toString();

    m_inputInUseId.clear();
    m_outputInUseId.clear();
    m_inputInUseDesc.clear();
    m_outputInUseDesc.clear();
    m_inputDebounce.reset();
    m_outputDebounce.reset();

    // The same decision procedure every later change goes through, so a
    // device that was absent at join and appears a second afterwards is
    // handled by identical code rather than by a special case that only
    // runs once and is therefore never exercised.
    //
    // Input first, and note what is NOT done here: nothing opens the
    // microphone unless the user selected it or it is the system default
    // input. Opening the AirPods mic drops them into hands-free mode
    // system-wide — a quality cliff the user hears in every other app —
    // so "follow the default output" must never drag the input along
    // with it.
    evaluateDevice(Direction::Input);
    if (m_inputInUseId.isEmpty()) {
        qWarning("[voice] No audio input device available");
    }
    evaluateDevice(Direction::Output);

    m_sequence = 0;
    m_captureBuffer.clear();
    m_captureHead = 0;
    m_playbackPending.clear();
    m_playbackPendingHead = 0;
    m_peerLevelFrames = 0;

    m_captureFrames = 0;
    m_captureLimitedFrames = 0;
    m_playbackFrames = 0;
    m_playbackLimitedFrames = 0;
    m_levelSummaryLogged = false;
    m_appliedInputGain = m_inputGain.load(std::memory_order_relaxed);
    m_appliedOutputGain = m_outputGain.load(std::memory_order_relaxed);

    // The timer is only a pump: it decides *when* we look at the sink,
    // never how much we write. Wall-clock timers drift against the
    // sound card's 48kHz crystal, so clocking playback off a 20ms
    // QTimer guarantees a slow slide into alternating underrun and
    // overrun. bytesFree() is the device asking for exactly what it
    // needs; ticking faster than the frame period just means we always
    // notice the request promptly. (Qt 6 dropped QAudioSink::notify()
    // / setNotifyInterval(), so a pump is the only hook available.)
    //
    // Created here rather than held as a member so that it is owned by,
    // and started on, the audio thread — QTimer refuses to start or stop
    // from any other one.
    m_playbackTimer = new QTimer(this);
    m_playbackTimer->setInterval(kPumpIntervalMs);
    m_playbackTimer->setTimerType(Qt::PreciseTimer);
    connect(m_playbackTimer, &QTimer::timeout, this, &AudioWorker::pumpPlayback);
    m_playbackTimer->start();

    m_started = true;
    qInfo("[voice] audio pipeline running on thread %p",
          static_cast<void*>(QThread::currentThread()));
    return true;
}

void AudioWorker::stopDevices() {
    if (!m_started) {
        // Never started, or already stopped. Jitter buffers can still
        // exist if packets were ingested without a successful device
        // open, so the cleanup below is unconditional — same reasoning
        // as the old ~AudioEngine fix.
        qDeleteAll(m_jitter);
        m_jitter.clear();
        m_peerLevels.clear();
        m_peerLevelPending.clear();
        return;
    }
    m_started = false;

    // What the call actually sounded like, level-wise, before the state
    // that knows is torn down.
    if (m_captureFrames > 0 || m_playbackFrames > 0) logLevelSummary("session end");

    // Stop the pump first: nothing below should run concurrently with a
    // render, and the timer is the only thing that triggers one.
    if (m_playbackTimer) {
        m_playbackTimer->stop();
        delete m_playbackTimer;
        m_playbackTimer = nullptr;
    }

    // Cancel any armed device restart before its devices go away. Same
    // thread-affinity rule as the pump timer: stopped and destroyed on
    // the thread that started it.
    for (QTimer** t : {&m_inputRestartTimer, &m_outputRestartTimer}) {
        if (*t) {
            (*t)->stop();
            delete *t;
            *t = nullptr;
        }
    }
    m_inputDebounce.reset();
    m_outputDebounce.reset();

    closeSource();
    closeSink();
    setInUse(Direction::Input, QString(), QString());
    setInUse(Direction::Output, QString(), QString());

    if (m_encoder) {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }

    qDeleteAll(m_jitter);
    m_jitter.clear();
    m_peerLevels.clear();
    m_peerLevelPending.clear();

    m_captureBuffer.clear();
    m_captureHead = 0;
    m_playbackPending.clear();
    m_playbackPendingHead = 0;
    m_inbox.clear();
    if (m_queue) m_queue->clear();

    // Hand VoIP mode back to the OS. Balanced against enterVoiceMode()
    // in startDevices().
    bsfchat::audio_routing::exitVoiceMode();
}

void AudioWorker::onMicDataReady() {
    if (!m_captureDevice || !m_encoder) return;

    const bool muted = m_muted.load(std::memory_order_relaxed);

    QByteArray chunk = m_captureDevice->readAll();
    m_captureBuffer.append(chunk);

    // Log the first N frames so we can confirm captured data actually has
    // amplitude. macOS TCC-denied mic returns all zeros silently.
    //
    // This counter was a function-local `static` before the pipeline
    // moved off the GUI thread. Only one audio thread exists at a time
    // and consecutive ones are separated by a join, so it was not
    // actually racing — but a mutable process-wide static reachable from
    // a non-GUI thread is a trap to leave lying around, and it also
    // meant the diagnostic only ever fired for the first voice join of
    // the process. Per-instance is both safer and more useful.
    if (m_debugFrameCount < 5 && chunk.size() > 0) {
        const int16_t* p = reinterpret_cast<const int16_t*>(chunk.constData());
        int n = chunk.size() / 2;
        int16_t mx = 0;
        for (int i = 0; i < n; ++i) {
            int16_t v = p[i] < 0 ? -p[i] : p[i];
            if (v > mx) mx = v;
        }
        qInfo("[voice] mic frame #%d: %d bytes, peak |sample|=%d (0=silent, 32767=clip)",
              m_debugFrameCount, int(chunk.size()), int(mx));
        m_debugFrameCount++;
    }

    while (m_captureBuffer.size() - m_captureHead >= kFrameBytes) {
        // Copy out rather than reinterpret_cast'ing into the QByteArray:
        // one 1920-byte memcpy is free next to the Opus encode, and it
        // sidesteps any alignment question about an offset read cursor.
        std::memcpy(m_captureFrame.data(),
                    m_captureBuffer.constData() + m_captureHead,
                    kFrameBytes);
        const int16_t* pcm = m_captureFrame.data();

        // ----- Gain control -----
        // AGC, input volume, limiter. See processCaptureFrame(). What is
        // encoded below is m_captureOut; the level meter still reads the
        // raw frame (times the input volume) — see the note there.
        bsfchat::voice::int16ToFloat(pcm, m_captureFloat.data(), kFrameSamples);
        processCaptureFrame();
        bsfchat::voice::floatToInt16(m_captureFloat.data(), m_captureOut.data(),
                                     kFrameSamples);

        // ----- Mic transmit level -----
        // Linear RMS in int16 units, normalized to 0..1 by int16 max, then
        // log-compressed so the UI isn't dominated by the top 10 dB.
        // We always publish the level so clients can display mic input even
        // when muted (useful to confirm the device is live); but we also
        // force it to zero when muted so "you're transmitting" indicators
        // don't falsely light up.
        float level = 0.0f;
        if (!muted) {
            double acc = 0.0;
            for (int i = 0; i < kFrameSamples; ++i) {
                double s = pcm[i];
                acc += s * s;
            }
            // The RAW mic level scaled by the input volume, deliberately
            // not the AGC's output. The speaking ring lights at 0.05, i.e.
            // about -57 dBFS on this scale, and the AGC may legitimately
            // lift a quiet mic's room tone to -40 dBFS: measured after the
            // AGC, the ring would be lit permanently for exactly the users
            // the AGC helps most. The input volume IS included, so turning
            // yourself down to 0% also turns the ring off — the ring must
            // not claim you are heard when you are not.
            float rms = static_cast<float>(std::sqrt(acc / kFrameSamples) / 32768.0)
                        * m_appliedInputGain;
            // Map to a perceptual 0..1: everything below ~-60 dBFS → 0,
            // above ~-10 dBFS → 1, linear-in-dB in between.
            constexpr float kFloor = -60.0f;
            constexpr float kCeil = -10.0f;
            float db = rms > 1e-6f ? 20.0f * std::log10(rms) : kFloor;
            level = (db - kFloor) / (kCeil - kFloor);
            if (level < 0.0f) level = 0.0f;
            if (level > 1.0f) level = 1.0f;
        }
        // EWMA smoothing — fast attack, slow release so the indicator
        // tracks voice onsets but doesn't twitch off between syllables.
        constexpr float kAttack = 0.6f;
        constexpr float kRelease = 0.15f;
        float alpha = level > m_smoothedLevel ? kAttack : kRelease;
        m_smoothedLevel = m_smoothedLevel + alpha * (level - m_smoothedLevel);
        // Deliberately NOT rate-limited, unlike peerLevelChanged. This is
        // one float per 20ms frame regardless of participant count, so the
        // marshalling cost is negligible — and ServerConnection's
        // "mic appears silent" detector counts *emissions* (150 of them
        // == 3s), so throttling this to 12.5Hz would silently stretch
        // that warning out to 12 seconds.
        emit micLevelChanged(m_smoothedLevel);

        // ----- Encode + transmit -----
        if (!muted) {
            unsigned char opusBuf[kMaxOpusPacket];
            int encoded = opus_encode(m_encoder, m_captureOut.data(), kFrameSamples,
                                      opusBuf, kMaxOpusPacket);

            if (encoded > 0) {
                // Build frame: 2 bytes sequence + 2 bytes timestamp_delta + opus data
                QByteArray frame;
                frame.resize(4 + encoded);
                // Big-endian sequence number
                frame[0] = static_cast<char>((m_sequence >> 8) & 0xFF);
                frame[1] = static_cast<char>(m_sequence & 0xFF);
                // Timestamp delta (20ms per frame)
                uint16_t ts = static_cast<uint16_t>(m_sequence * 20);
                frame[2] = static_cast<char>((ts >> 8) & 0xFF);
                frame[3] = static_cast<char>(ts & 0xFF);
                std::memcpy(frame.data() + 4, opusBuf, encoded);

                m_sequence++;
                // Crosses back to the GUI thread (queued) where
                // PeerConnectionManager::sendAudioFrame does the SCTP
                // send. See AudioEngine.h for why the send is not done
                // from here.
                emit audioFrameReady(frame);
            }
        }

        m_captureHead += kFrameBytes;
    }

    // Amortised compaction. The overwhelmingly common case is that the
    // drain consumed everything readAll() handed us, which costs a
    // clear(); otherwise we only memmove once the leftover prefix has
    // grown past ~1.3s of audio, instead of every single 20ms frame.
    if (m_captureHead >= m_captureBuffer.size()) {
        m_captureBuffer.clear();
        m_captureHead = 0;
    } else if (m_captureHead >= kCaptureCompactBytes) {
        m_captureBuffer.remove(0, m_captureHead);
        m_captureHead = 0;
    }
}

void AudioWorker::processCaptureFrame() {
    float* buf = m_captureFloat.data();

    const bool agcOn = !kPlatformVoiceProcessing
                       && m_autoGain.load(std::memory_order_relaxed);
    if (agcOn) {
        m_agc.processFrame(buf, kFrameSamples);
    } else if (m_agcRan) {
        // Switched off mid-call: fade from whatever gain the AGC had
        // reached back to unity over this one frame, instead of stepping
        // there, and forget its state so switching it back on starts
        // from unity rather than from a stale level estimate.
        bsfchat::voice::applyGainRamp(
            buf, kFrameSamples, bsfchat::voice::dbToLinear(m_agc.gainDb()), 1.0f);
        m_agc.reset();
    }
    m_agcRan = agcOn;

    const float inGain = m_inputGain.load(std::memory_order_relaxed);
    bsfchat::voice::applyGainRamp(buf, kFrameSamples, m_appliedInputGain, inGain);
    m_appliedInputGain = inGain;

    // Always, including when muted: the limiter's 2 ms delay line must
    // stay continuous with the mic, or unmuting would splice in a stale
    // tail.
    m_captureLimiter.process(buf, kFrameSamples);

    ++m_captureFrames;
    if (m_captureLimiter.lastBlockMinGain() < 0.999f) ++m_captureLimitedFrames;

    // One line, once, ten seconds of speech into the call — the earliest
    // point at which the numbers mean anything. Not per-frame logging:
    // the flag makes it fire at most once per session.
    if (!m_levelSummaryLogged && agcOn
        && m_agc.speechFrames() >= kLevelSummarySpeechFrames) {
        m_levelSummaryLogged = true;
        logLevelSummary("after 10 s of speech");
    }
}

void AudioWorker::logLevelSummary(const char* when) {
    // Levels only, to the local log. Nothing here leaves the machine.
    //
    // Raw speech percentiles are what the microphone delivered BEFORE any
    // gain, over frames the AGC classified as speech — the number that
    // answers "was the mic simply quiet". Only collected while the AGC
    // runs; on Android, or with AGC switched off, they read -120.
    const double capLimited = m_captureFrames > 0
        ? 100.0 * double(m_captureLimitedFrames) / double(m_captureFrames) : 0.0;
    const double playLimited = m_playbackFrames > 0
        ? 100.0 * double(m_playbackLimitedFrames) / double(m_playbackFrames) : 0.0;
    qInfo("[voice] levels (%s): %.1f s speech, raw speech p10/p50/p90 "
          "%.0f/%.0f/%.0f dBFS, noise floor %.0f dBFS, AGC %s gain %+.1f dB, "
          "input vol x%.2f, capture limiter active %.1f%% of frames; "
          "output vol x%.2f, playback limiter active %.1f%% of frames",
          when, double(m_agc.speechFrames()) * 0.02,
          double(m_agc.rawSpeechPercentileDbfs(0.1f)),
          double(m_agc.rawSpeechPercentileDbfs(0.5f)),
          double(m_agc.rawSpeechPercentileDbfs(0.9f)),
          double(m_agc.noiseFloorDbfs()),
          kPlatformVoiceProcessing ? "platform"
              : (m_autoGain.load(std::memory_order_relaxed) ? "on" : "off"),
          double(m_agc.gainDb()), double(m_appliedInputGain), capLimited,
          double(m_appliedOutputGain), playLimited);
}

void AudioWorker::ingestQueuedPackets() {
    if (!m_queue) return;
    m_queue->drain(m_inbox);
    for (const auto& item : m_inbox) {
        switch (item.kind) {
        case AudioPacketQueue::Kind::RemovePeer:
            dropPeer(item.peerId);
            break;
        case AudioPacketQueue::Kind::Audio: {
            // 4-byte header + at least 1 byte of Opus.
            if (item.data.size() <= bsfchat::voice::JitterBuffer::kHeaderBytes)
                break;
            auto* jb = jitterFor(item.peerId);
            if (!jb) break;
            // No decode here. The packet is parked in the jitter buffer,
            // which reorders it against its neighbours and hands it to
            // the decoder at its playout instant. Packets that arrive
            // after that instant are discarded by push() rather than
            // being decoded out of order.
            jb->pushPacket(item.data.constData(),
                           static_cast<int>(item.data.size()));
            break;
        }
        case AudioPacketQueue::Kind::PeerGain:
            // operator[] inserts on first sight of a peer — one small
            // allocation per user whose volume is changed, on a user
            // action, the same class of event as jitterFor() creating a
            // buffer. Never per frame.
            m_peerGains[item.peerId].target = item.gain;
            break;
        }
    }
    m_inbox.clear();
}

bool AudioWorker::flushPendingPlayback() {
    if (!m_playbackDevice) return false;
    while (m_playbackPendingHead < m_playbackPending.size()) {
        const qint64 written = m_playbackDevice->write(
            m_playbackPending.constData() + m_playbackPendingHead,
            m_playbackPending.size() - m_playbackPendingHead);
        if (written <= 0) return false;  // device full (or errored)
        m_playbackPendingHead += static_cast<int>(written);
    }
    m_playbackPending.clear();
    m_playbackPendingHead = 0;
    return true;
}

void AudioWorker::renderMixedFrame(int16_t* out) {
    m_mixer->begin();

    // One emission window per kPeerLevelEmitFrames rendered frames.
    const bool emitLevels =
        (++m_peerLevelFrames >= kPeerLevelEmitFrames);

    for (auto it = m_jitter.begin(); it != m_jitter.end(); ++it) {
        auto* jb = it.value();
        if (!jb) continue;

        // Decode happens here, at playout time — that is what lets the
        // jitter buffer splice Opus PLC frames into the right slots.
        const auto result = jb->pop(m_peerFrame.data());

        // Peak-magnitude → 0..1 level, EWMA-smoothed per peer so the
        // UI ring doesn't flicker. Same vocabulary as micLevelChanged.
        // Driving it from playout (rather than arrival) means the ring
        // also decays back to zero when a peer stops sending, instead
        // of freezing on its last value.
        int16_t peak = 0;
        if (result != bsfchat::voice::JitterBuffer::PopResult::Silence) {
            for (int i = 0; i < kFrameSamples; ++i) {
                const int16_t v = m_peerFrame[static_cast<size_t>(i)];
                const int16_t a = v < 0 ? static_cast<int16_t>(-(v + 1)) : v;
                if (a > peak) peak = a;
            }
            // Per-user volume, ramped from where the last frame ended.
            // find() on an unshared QMap neither allocates nor detaches.
            float from = 1.0f, to = 1.0f;
            auto g = m_peerGains.find(it.key());
            if (g != m_peerGains.end()) {
                from = g->applied;
                to = g->target;
                g->applied = to;
            }
            m_mixer->add(m_peerFrame.data(), kFrameSamples, from, to);
        }
        const float raw = static_cast<float>(peak) / 32768.0f;
        const float prev = m_peerLevels.value(it.key(), 0.0f);
        const float smoothed = prev * 0.75f + raw * 0.25f;
        m_peerLevels[it.key()] = smoothed;

        // Hold the window's maximum so a one-frame onset still reaches
        // the UI even though we only emit every fourth frame.
        float pending = m_peerLevelPending.value(it.key(), 0.0f);
        if (smoothed > pending) pending = smoothed;
        if (emitLevels) {
            // Reset before emitting, not after. The emit is a queued
            // hop today so it cannot re-enter, but holding a reference
            // into m_peerLevelPending across a signal would silently
            // become a dangling-reference bug the moment anyone
            // attached a direct connection to peerLevelChanged.
            m_peerLevelPending[it.key()] = 0.0f;
            emit peerLevelChanged(it.key(), pending);
        } else {
            m_peerLevelPending[it.key()] = pending;
        }
    }

    if (emitLevels) m_peerLevelFrames = 0;

    // Output volume, then the limiter, inside finish(). The limiter runs
    // even while deafened (below) so its delay line stays continuous.
    const float outGain = m_outputGain.load(std::memory_order_relaxed);
    const auto& mixed = m_mixer->finish(m_appliedOutputGain, outGain);
    m_appliedOutputGain = outGain;
    ++m_playbackFrames;
    if (m_mixer->lastLimiterMinGain() < 0.999f) ++m_playbackLimitedFrames;
    if (m_deafened.load(std::memory_order_relaxed)) {
        // Still pop every jitter buffer above (so they keep draining
        // and don't resync-thrash while deafened) — just don't play it.
        std::memset(out, 0, static_cast<size_t>(kFrameBytes));
    } else {
        std::memcpy(out, mixed.data(), static_cast<size_t>(kFrameBytes));
    }
}

void AudioWorker::pumpPlayback() {
    // Take delivery of whatever the network produced since the last
    // pump, before deciding what to play. Doing it here rather than on
    // arrival is what keeps every JitterBuffer single-threaded.
    ingestQueuedPackets();

    if (!m_audioSink || !m_playbackDevice) return;
    if (m_audioSink->state() == QAudio::StoppedState) return;

    // Anything the device refused last time goes out first, otherwise
    // we'd reorder the stream.
    if (!flushPendingPlayback()) return;

    // The device tells us how much it wants. Writing exactly that —
    // rather than one frame per wall-clock tick — is what keeps us
    // locked to the sound card's clock instead of drifting against it.
    qsizetype free = m_audioSink->bytesFree();
    int frames = 0;
    while (free >= kFrameBytes && frames < kMaxFramesPerPump) {
        renderMixedFrame(m_playbackFrame.data());

        const char* bytes =
            reinterpret_cast<const char*>(m_playbackFrame.data());
        const qint64 written = m_playbackDevice->write(bytes, kFrameBytes);
        if (written < 0) return;  // device error; retry next pump
        if (written < kFrameBytes) {
            // Short write. The old code ignored write()'s return value
            // entirely, which silently threw away the tail of the
            // frame; keep the remainder and finish it next pump.
            m_playbackPending = QByteArray(bytes + written,
                                           kFrameBytes - static_cast<int>(written));
            m_playbackPendingHead = 0;
            return;
        }
        free -= kFrameBytes;
        frames++;
    }
}

bsfchat::voice::JitterBuffer* AudioWorker::jitterFor(const QString& peerId) {
    auto it = m_jitter.find(peerId);
    if (it != m_jitter.end()) return it.value();

    auto* jb = new bsfchat::voice::JitterBuffer(kSampleRate, kChannels, kFrameSamples);
    if (!jb->isValid()) {
        qWarning("[voice] Failed to create Opus decoder for peer %s",
                 qPrintable(peerId));
        delete jb;
        return nullptr;
    }
    m_jitter.insert(peerId, jb);
    return jb;
}

void AudioWorker::dropPeer(const QString& peerId) {
    auto it = m_jitter.find(peerId);
    if (it != m_jitter.end()) {
        delete it.value();
        m_jitter.erase(it);
    }
    m_peerLevels.remove(peerId);
    m_peerLevelPending.remove(peerId);
}

// ---------------------------------------------------------------------
// Live device selection
// ---------------------------------------------------------------------

void AudioWorker::setDeviceSnapshot(Direction dir, QList<QAudioDevice> devices,
                                    QAudioDevice defaultDevice) {
    if (dir == Direction::Input) {
        m_inputSnapshot = std::move(devices);
        m_defaultInput = std::move(defaultDevice);
    } else {
        m_outputSnapshot = std::move(devices);
        m_defaultOutput = std::move(defaultDevice);
    }
}

void AudioWorker::onSystemDevicesChanged(Direction dir,
                                         QList<QAudioDevice> devices,
                                         QAudioDevice defaultDevice) {
    setDeviceSnapshot(dir, std::move(devices), std::move(defaultDevice));

    // Not in a call. The snapshot above is kept anyway, so the next
    // startDevices() resolves against current reality rather than
    // against whatever was true when the engine was created.
    if (!m_started) return;

    const bool input = (dir == Direction::Input);
    auto& debounce = input ? m_inputDebounce : m_outputDebounce;
    QTimer*& timer = input ? m_inputRestartTimer : m_outputRestartTimer;

    // Absorbed into a window that is already armed. Nothing is lost: the
    // window acts on the snapshot as it stands when it closes, which is
    // the one we just stored.
    if (!debounce.onEvent(QDateTime::currentMSecsSinceEpoch())) return;

    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, dir]() {
            (dir == Direction::Input ? m_inputDebounce : m_outputDebounce)
                .onFire();
            evaluateDevice(dir);
        });
    }
    timer->start(int(debounce.windowMs()));
}

QList<bsfchat::voice::DeviceInfo> AudioWorker::deviceInfos(Direction dir) const {
    using bsfchat::voice::DeviceInfo;

    const QList<QAudioDevice>& devices =
        (dir == Direction::Input) ? m_inputSnapshot : m_outputSnapshot;
    const QAudioDevice& def =
        (dir == Direction::Input) ? m_defaultInput : m_defaultOutput;
    const QString defId = QString::fromLatin1(def.id());

    QList<DeviceInfo> out;
    out.reserve(devices.size());
    for (const QAudioDevice& d : devices) {
        const QString id = QString::fromLatin1(d.id());
        // Two independent sources for "this is the default": the flag
        // the backend sets on the device, and the id
        // QMediaDevices::defaultAudio*() reported in the same snapshot.
        // Qt's CoreAudio backend sets both; taking either keeps this
        // honest on a backend that only sets one.
        out.append(DeviceInfo{
            id, d.description(),
            d.isDefault() || (!defId.isEmpty() && id == defId)});
    }
    return out;
}

const QAudioDevice* AudioWorker::snapshotDevice(Direction dir,
                                                const QString& id) const {
    const QList<QAudioDevice>& devices =
        (dir == Direction::Input) ? m_inputSnapshot : m_outputSnapshot;
    for (const QAudioDevice& d : devices) {
        if (QString::fromLatin1(d.id()) == id) return &d;
    }
    return nullptr;
}

void AudioWorker::setInUse(Direction dir, const QString& id,
                           const QString& description) {
    const bool input = (dir == Direction::Input);
    QString& inUseId = input ? m_inputInUseId : m_outputInUseId;
    QString& inUseDesc = input ? m_inputInUseDesc : m_outputInUseDesc;
    if (inUseId == id && inUseDesc == description) return;
    inUseId = id;
    inUseDesc = description;
    emit deviceInUseChanged(input, description);
}

void AudioWorker::evaluateDevice(Direction dir) {
    using bsfchat::voice::DeviceAction;
    using bsfchat::voice::DeviceDecision;

    const bool input = (dir == Direction::Input);
    const char* what = input ? "Input" : "Output";
    const QString& pref = input ? m_preferredInput : m_preferredOutput;
    const QString& hint = input ? m_inputHintId : m_outputHintId;
    // Copies, not references: setInUse() below rewrites the members.
    const QString inUseId = input ? m_inputInUseId : m_outputInUseId;
    const QString wasDesc = input ? m_inputInUseDesc : m_outputInUseDesc;

    const DeviceDecision d =
        bsfchat::voice::resolveDevice(pref, hint, deviceInfos(dir), inUseId);

    // By far the common case: a notification burst that resolves to the
    // device already open. Restarting here would be an audible gap for
    // no change at all.
    if (d.action == DeviceAction::Keep) return;

    if (d.action == DeviceAction::None) {
        if (!inUseId.isEmpty()) {
            qWarning("[voice] %s device %s went away and there is no "
                     "replacement — %s is now silent",
                     what, qPrintable(wasDesc),
                     input ? "capture" : "playback");
            if (input) closeSource(); else closeSink();
            setInUse(dir, QString(), QString());
        }
        return;
    }

    const QAudioDevice* device = snapshotDevice(dir, d.id);
    if (!device) {
        // Decision and snapshot disagree, which needs the list to have
        // changed underneath us. Another notification is already on its
        // way; do nothing rather than guess.
        return;
    }

    const char* why =
        d.preferenceMissing ? "preferred device gone, using system default"
        : d.isSystemDefault ? "system default"
                            : "preferred device";
    if (inUseId.isEmpty()) {
        qInfo("[voice] %s device: %s (%s)", what, qPrintable(d.description),
              why);
    } else {
        qInfo("[voice] %s device changed: %s → %s (%s)", what,
              qPrintable(wasDesc), qPrintable(d.description), why);
    }

    // ONE direction. The encoder, every peer's jitter buffer and its
    // adaptive playout target, the pump timer and the voice session
    // itself are all untouched — a headset switch is not a reason to
    // rebuild the call.
    if (input) {
        closeSource();
        openSource(*device);
    } else {
        closeSink();
        openSink(*device);
    }
    setInUse(dir, d.id, d.description);
}

void AudioWorker::openSource(const QAudioDevice& device) {
    if (device.isNull()) return;
    // Parented to `this`, which lives on the audio thread, and
    // constructed here — so the source and the QIODevice it hands
    // back are both affine to the thread that will drive them.
    // A different microphone: its noise floor, level history and the
    // tail in the limiter's delay line all belonged to the old one. The
    // AGC re-learns within its 0.5 s warm-up, starting from unity.
    m_agc.reset();
    m_captureLimiter.reset();
    m_audioSource = new QAudioSource(device, m_format, this);
    m_captureDevice = m_audioSource->start();
    if (m_captureDevice) {
        // Both ends are audio-thread objects, so this is a direct
        // connection and the encode happens inline on the device
        // callback's thread — never a hop through the GUI.
        connect(m_captureDevice, &QIODevice::readyRead,
                this, &AudioWorker::onMicDataReady);
    } else {
        qWarning("[voice] QAudioSource::start() returned null — "
                 "macOS likely still denying microphone access");
    }
    // QAudioSource has a State enum we can peek at for a sanity check.
    qInfo("[voice] QAudioSource initial state=%d error=%d",
          int(m_audioSource->state()), int(m_audioSource->error()));
}

void AudioWorker::closeSource() {
    if (m_audioSource) {
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
        m_captureDevice = nullptr;
    }
    // Whatever the old device captured but we had not yet encoded. Its
    // sample stream has no relation to the next device's, and a partial
    // frame spliced onto the front of the new one is a click.
    m_captureBuffer.clear();
    m_captureHead = 0;
}

void AudioWorker::openSink(const QAudioDevice& device) {
    if (device.isNull()) return;
    m_mixer->resetLimiter();
    m_audioSink = new QAudioSink(device, m_format, this);
    // Must be set before start(). Bounds the amount of audio the
    // device holds, and therefore the floor on output latency.
    m_audioSink->setBufferSize(kFrameBytes * kSinkBufferFrames);
    m_playbackDevice = m_audioSink->start();
    if (!m_playbackDevice) {
        qWarning("[voice] QAudioSink::start() returned null — no playback");
    }
}

void AudioWorker::closeSink() {
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
        m_playbackDevice = nullptr;
    }
    // The tail of a short write to the device that just went away.
    // pumpPlayback() already tolerates the gap: it ingests into the
    // jitter buffers before it looks at the sink, so the few
    // milliseconds a restart takes cost nothing but the restart.
    m_playbackPending.clear();
    m_playbackPendingHead = 0;
}
