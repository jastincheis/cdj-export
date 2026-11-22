#pragma once

#include "Library.h"

// Serato DJ (Pro / Lite) on a stick. Serato keeps cues, loops and the beat
// grid inside each audio file, as ID3 "GEOB" frames ("Serato Markers2",
// "Serato BeatGrid"); MP3s carry the ID3 tag at the start, WAVs in an
// "id3 " RIFF chunk. Playlists are crates in _Serato_/Subcrates, and
// _Serato_/database V2 lists the tracks.
//
// Formats as reverse-engineered by Jan Holthuis, serato-tags (MIT):
// https://github.com/Holzhaus/serato-tags
namespace cdj
{
// The Serato hot-cue pad colours 1–8 (Serato DJ Pro defaults).
juce::uint32 seratoHotCueRgb (int index);

// Tag contents, without any ID3 framing.
juce::MemoryBlock seratoMarkers2 (const StickTrack& track);
juce::MemoryBlock seratoBeatGrid (const StickTrack& track);   // empty when no tempo

// A complete ID3v2 tag for `track`: Serato GEOB frames and TBPM, plus every
// frame of `oldTag` (an ID3v2.3 / 2.4 tag, may be empty) except the old Serato
// frames and TBPM. Title and artist are added when the old tag has none.
juce::MemoryBlock seratoId3Tag (const StickTrack& track, const juce::MemoryBlock& oldTag);

// Rewrites the file on the stick with the Serato tags (through a temporary
// file next to it, renamed into place). The audio is not touched.
bool writeSeratoTags (const juce::File& audioFile, const StickTrack& track, juce::String& error);

// Reads the ID3 tag back: from the start of an MP3, or the "id3 " chunk of a WAV.
juce::MemoryBlock readId3Tag (const juce::File& audioFile);

// The GEOB frame with this description inside an ID3 tag (empty if none).
juce::MemoryBlock findGeob (const juce::MemoryBlock& tag, const juce::String& description);

// _Serato_ files.
juce::MemoryBlock seratoCrate (const StickLibrary& library, const StickPlaylist& playlist);
juce::MemoryBlock seratoDatabase (const StickLibrary& library);

juce::File seratoFolder (const juce::File& stickRoot);
juce::File seratoMarkerFile (const juce::File& stickRoot);    // says the _Serato_ folder is ours

// True when the stick has a _Serato_ folder this program didn't write.
bool hasForeignSeratoLibrary (const juce::File& stickRoot);

// Tags every track of `library` on the stick (updates each track's byte
// size) and writes database V2 plus one crate per playlist.
bool writeSeratoLibrary (StickLibrary& library, const juce::File& stickRoot, juce::String& error);
} // namespace cdj
