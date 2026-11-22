#pragma once

#include "Library.h"

#include <functional>

namespace cdj
{
struct ExportOptions
{
    enum class Format { keep, wav, mp3 };

    Format format = Format::keep;
    int mp3Kbps = 320;
    CueOptions cues;
    bool mp3DelayCorrection = true;     // shift cues by the MP3 encoder/decoder delay
    juce::String playlistName = "Adormis";
    bool replaceEverything = false;     // on a stick written before: false = add this playlist to it
    bool rekordboxXml = true;           // also write "Adormis CDJ Export.xml" on the stick

    // For which DJ software. A stick keeps every format it was written in
    // before (unless everything is replaced), so no library on it goes stale.
    bool rekordbox = true;              // Pioneer CDJs / rekordbox: PIONEER folder
    bool serato = false;                // Serato DJ: tags in the audio files + _Serato_ crates
};

struct ExportReport
{
    bool ok = false;
    bool cancelled = false;
    juce::String error;
    juce::StringArray warnings;
    int tracksCopied = 0, tracksConverted = 0, tracksReused = 0;
    juce::File xmlFile;
    StickLibrary library;
};

// Called often with 0…1 and what's happening; return false to cancel.
using ExportProgress = std::function<bool (double fraction, const juce::String& status)>;

// The Pioneer database writer plugs in here (export.pdb, ANLZ, *SETTING.DAT):
// writePioneerLibrary() in PioneerWriter.h. It gets the complete library and
// writes every Pioneer file; without it, the export writes only the audio,
// the list of tracks and the rekordbox XML.
using PioneerWriter = std::function<bool (const StickLibrary&, const juce::File& stickRoot, juce::String& error)>;

ExportReport exportToStick (const std::vector<Track>& playlist, const juce::File& stickRoot,
                            const ExportOptions& options, const ExportProgress& progress,
                            const PioneerWriter& pioneerWriter = {});

// True when a CDJ-2000 can play this WAV as it is: 16 or 24-bit PCM,
// 44.1 or 48 kHz, mono or stereo.
bool isCdjReadyWav (const Track& track);

// The output type ("wav" / "mp3") a track gets with these options, and
// whether it is copied unchanged.
juce::String outputType (const Track& track, ExportOptions::Format format, bool& copyUnchanged);

juce::String programVersion();
} // namespace cdj
