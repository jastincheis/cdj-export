#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace cdj
{
// MP3 encoding goes through LAME (libmp3lame, LGPL) when the build has it.
bool mp3EncodingAvailable();

// Writes a CBR MP3 (with a LAME/Info header so players know the encoder delay).
bool writeMp3 (const juce::File& destination, const juce::AudioBuffer<float>& audio, double sampleRate,
               int kbps, juce::String& error);

// The silence an MP3 encoder put in front of the audio, in samples, as the
// file's LAME header says (encoder delay only; decoders add 529 more).
// -1 when the file has no LAME header (made by another encoder).
int mp3EncoderDelay (const juce::File& mp3);

// Sample rate of the first frame; 0 if unknown.
int mp3SampleRate (const juce::File& mp3);

// CBR bit rate of the first frame, in kbps; 0 if unknown.
int mp3Bitrate (const juce::File& mp3);
} // namespace cdj
