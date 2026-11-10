// Ported from baken (github.com/M-Igashi/baken, crates/baken-export), MIT License,
// Copyright (c) 2025 Jesse Pinkman (JPHFA). See THIRD_PARTY_NOTICES.md.

#pragma once

#include "PioneerPdb.h"

#include <array>

// The analysis files of one track (PIONEER/USBANLZ/…/ANLZ0000.DAT, .EXT,
// .2EX): beat grid, cues, loops and waveforms.
namespace cdj::pioneer
{
// The folder the players compute for an audio file themselves (they ignore
// the analyze path in export.pdb): "/PIONEER/USBANLZ/P016/0000875E" for
// `usbPath` like "/Contents/Adormis/Artist - Title.wav".
juce::String anlzFolder (const juce::String& usbPath);

// The audio of a track reduced to 150 columns per second: everything the
// waveforms are drawn from.
struct Measured
{
    struct Column
    {
        double peak = 0.0, white = 0.0, band[3] {}, sumSquares = 0.0;
        std::uint32_t samples = 0;
    };

    double sampleRate = 0.0;
    juce::int64 frames = 0;
    std::vector<Column> columns;

    double durationMs() const { return sampleRate > 0.0 ? (double) frames * 1000.0 / sampleRate : 0.0; }
};

// Decodes the file (WAV or MP3, through cdj::formats()) and measures it.
bool measureAudio (const juce::File& file, Measured& measured, juce::String& error);

// MPEG audio frames in an MP3, without the ID3 tag and the Xing/Info frame,
// the way rekordbox counts them for PVBR. 0 if none.
std::uint32_t mp3AudioFrames (const juce::File& mp3);

struct Beat
{
    int number = 1;             // 1–4 in the bar
    int tempo = 0;              // BPM × 100
    std::uint32_t timeMs = 0;
};

// Constant-tempo 4/4 grid through the first downbeat, from the first beat
// at or after 0 to the end of the track. Empty when bpm is 0.
std::vector<Beat> gridBeats (double bpm, double firstBeatMs, double durationMs);

struct AnlzFiles
{
    Bytes dat, ext, twoEx;
};

// `mp3Frames`: what mp3AudioFrames() says for an MP3, 0 for other files.
AnlzFiles buildAnlzFiles (const StickTrack& track, const juce::String& usbPath, const Measured& audio,
                          std::uint32_t mp3Frames);

// The four colour bytes (code, r, g, b) a PCO2 hot cue entry carries for a
// rekordbox hot cue colour code (0 = no colour).
std::array<std::uint8_t, 4> hotCueColourBytes (int code);
} // namespace cdj::pioneer
