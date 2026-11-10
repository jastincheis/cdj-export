#pragma once

#include <juce_core/juce_core.h>

#include <vector>

// USB sticks (and other removable drives) the export can be written to.
namespace cdj
{
enum class StickContent
{
    empty,          // no PIONEER folder: free to write
    adormis,        // written by Adormis CDJ Export before
    rekordbox,      // has a library made by rekordbox (or another program): never overwritten
};

struct Drive
{
    juce::File root;
    juce::String label;         // volume name, or the mount folder's name
    juce::String fileSystem;    // "vfat", "exfat", "FAT32", "msdos", … as the OS reports it
    juce::int64 totalBytes = 0, freeBytes = 0;
    StickContent content = StickContent::empty;

    juce::String describe() const;
    // FAT32 works on every CDJ; exFAT only on newer ones (not the first CDJ-2000).
    juce::String fileSystemWarning() const;
};

std::vector<Drive> findDrives();
StickContent inspectStick (const juce::File& root);

// The small file that marks a stick as written by this program.
juce::File stickMarkerFile (const juce::File& root);
} // namespace cdj
