#pragma once

#include "CuePlan.h"

// What is on the stick: every track copied there and every playlist. It is
// saved on the stick (PIONEER/adormis-cdj-export.json) and the whole Pioneer
// database is rebuilt from it at each export, so a later export can add a
// playlist without reading Pioneer's own files back.
namespace cdj
{
struct StickTrack
{
    int id = 0;                         // 1, 2, 3… (also the rekordbox track id)
    juce::String path;                  // relative to the stick root, "/" separators
    juce::String title, artist;
    juce::String fileType;              // "wav" or "mp3"
    double seconds = 0.0;
    int sampleRate = 0, bitsPerSample = 0, channels = 0, bitrateKbps = 0;
    juce::int64 bytes = 0;
    double bpm = 0.0;                   // 0 = no beat grid
    double firstBeatMs = 0.0;           // the first downbeat ("1"), in the file on the stick
    std::vector<Cue> cues;
    juce::String source;                // where it came from (full path), for the user
    juce::String sourceFingerprint;     // size + modification time of the source, to spot re-adds
};

struct StickPlaylist
{
    juce::String name;
    std::vector<int> trackIds;
};

struct StickLibrary
{
    std::vector<StickTrack> tracks;
    std::vector<StickPlaylist> playlists;
    juce::StringArray formats;          // "rekordbox", "serato": what this stick carries
    juce::String writtenBy, writtenAt;

    const StickTrack* find (int id) const;
    int nextId() const;

    juce::var toJson() const;
    static bool fromJson (const juce::var& json, StickLibrary& library, juce::String& error);

    bool save (const juce::File& file, juce::String& error) const;
    static bool load (const juce::File& file, StickLibrary& library, juce::String& error);
};

// A file or folder name every CDJ can read from a FAT32 / exFAT stick:
// no \ / : * ? " < > |, no control characters, no trailing dots or spaces,
// not a reserved Windows name, and not too long.
juce::String fatSafeName (const juce::String& name, int maxCharacters = 100);

// Writes `data` next to `target` first, then renames it into place, so a
// pulled-out stick never ends up with half a file under the real name.
bool replaceFileSafely (const juce::File& target, const juce::MemoryBlock& data, juce::String& error);
} // namespace cdj
