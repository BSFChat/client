#pragma once

#include <QByteArray>
#include <cstdint>

// Generates small WAV audio buffers for notification sounds.
// All sounds are synthesized — no external files needed.
namespace SoundGenerator {

// Ascending two-tone chime (join)
QByteArray generateJoinSound();

// Descending two-tone chime (leave/disconnect)
QByteArray generateLeaveSound();

// Short click/pop (mute toggle)
QByteArray generateMuteSound();

// Helper: generate a WAV file in memory
QByteArray generateTone(float freq1, int duration1Ms,
                         float freq2, int duration2Ms,
                         int sampleRate = 48000, float volume = 0.3f);

} // namespace SoundGenerator
