#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <vector>

// One audio file in the playlist: what it is, and the markers and regions
// that will become cues on the stick.
namespace cdj
{
struct Marker
{
    juce::int64 sample = 0;     // in the marker source's own sample rate
    juce::String name;
};

struct Region
{
    juce::int64 start = 0, end = 0;
    juce::String name;
};

struct Track
{
    juce::File file;
    juce::String extension;     // lower case, no dot: "wav", "mp3", ...
    double sampleRate = 0.0;
    juce::int64 lengthSamples = 0;
    int channels = 0;
    int bitsPerSample = 0;
    bool floatingPoint = false;
    bool compressed = false;    // MP3 and other lossy formats

    juce::String title, artist;

    // Markers and regions, with the sample rate their positions are in. For a
    // WAV that's the file itself; for an MP3 it's the WAV next to it with the
    // same name (the Adormis project it was made from), when there is one.
    std::vector<Marker> markers;
    std::vector<Region> regions;
    double markerRate = 0.0;
    juce::File markerSource;

    // Markers taken from the WAV next to an MP3 sit on the WAV's timeline;
    // in the MP3 everything is later by the encoder + decoder delay. This is
    // that shift (0 when the markers are on this file's own timeline).
    double markerShiftSeconds = 0.0;

    juce::StringArray notes;    // shown to the user: where markers came from, what's missing

    // Beat grid: tempo and the first downbeat ("1" of a bar), in this file's
    // own samples. bpm 0 = no grid yet.
    double bpm = 0.0;
    juce::int64 firstBeat = 0;
    bool gridByHand = false;

    bool markersChanged = false;    // edited here and not saved into the file yet

    double seconds() const { return sampleRate > 0.0 ? (double) lengthSamples / sampleRate : 0.0; }
    double markerSeconds (juce::int64 sample) const { return markerRate > 0.0 ? (double) sample / markerRate : 0.0; }

    // A marker's place in this file (what you hear), and back.
    double markerInFile (juce::int64 sample) const { return markerSeconds (sample) + markerShiftSeconds; }
    juce::int64 markerFromFile (double seconds) const
    {
        return (juce::int64) std::llround ((seconds - markerShiftSeconds) * (markerRate > 0.0 ? markerRate : sampleRate));
    }
};

// Reads the file's format, length, title/artist and markers. Returns false
// (with a reason) only when the file can't be used at all.
bool loadTrack (const juce::File& file, Track& track, juce::String& error);

// The cue points, labels and regions JUCE reads from a WAV's "cue " and
// "LIST adtl" chunks, the way Sound Forge and Adormis Audio write them.
// Unnamed markers saved as "Marker 3" come back with no name.
void readWavCues (const juce::StringPairArray& metadata, juce::int64 totalSamples,
                  std::vector<Marker>& markers, std::vector<Region>& regions);

// Writes the markers and regions back into the WAV they came from (cue
// points + labels, as Sound Forge and Adormis Audio do). The audio is
// rewritten sample for sample in the same format, through a temporary file
// that replaces the original only when it is complete. An MP3 with no WAV
// next to it keeps its markers in this program's own list instead.
bool saveMarkers (Track& track, juce::String& error);

// Tempo, first beat and (for files that can't hold them) markers, remembered
// per file between sessions in the program's settings folder.
void rememberTrack (const Track& track);
void recallTrack (Track& track);

// "Artist - Title.wav" → artist and title; otherwise the whole name is the title.
void titleFromFileName (const juce::File& file, juce::String& artist, juce::String& title);

juce::AudioFormatManager& formats();
} // namespace cdj
