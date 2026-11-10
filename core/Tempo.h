#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>

namespace cdj
{
// Beats per minute of `range`, to 0.1 BPM (60..200), or empty when there's no
// steady beat (speech, silence, pads). From Adormis Audio.
std::optional<double> measureTempo (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sampleRate);

// The first downbeat ("1" of a bar) in the file for this tempo, in samples:
// beat phase from onset strength, the "one" from the kick-drum band, then
// moved onto the attacks in the waveform itself.
std::optional<juce::int64> findFirstDownbeat (const juce::AudioBuffer<float>& audio, double sampleRate, double bpm);
} // namespace cdj
