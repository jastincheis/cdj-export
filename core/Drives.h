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
    juce::File root;            // where it is mounted; empty when it isn't (Linux)
    bool mounted = true;
    juce::String device;        // Linux: /dev/sdb for a stick that isn't mounted
    juce::String label;         // volume name, or the mount folder's name
    juce::String fileSystem;    // "vfat", "exfat", "FAT32", "msdos", … as the OS reports it
    juce::int64 totalBytes = 0, freeBytes = 0;
    StickContent content = StickContent::empty;

    juce::String describe() const;
    // FAT32 works on every CDJ; exFAT only on newer ones (not the first CDJ-2000).
    juce::String fileSystemWarning() const;
};

std::vector<Drive> findDrives();

// Mounts a stick that isn't (Linux, where nothing mounts sticks by itself
// on some desktops). Fills `root`; false with a reason, e.g. when the stick
// has no file system a computer can mount (then it needs Format...).
bool mountDrive (Drive& drive, juce::String& error);

// Safely removes a stick: writes everything still in memory, unmounts it and
// (Linux) powers it off, so it can be pulled out. False with a reason.
bool ejectDrive (const Drive& drive, juce::String& error);

// The same stick in another list (after a refresh), by mount point or device.
bool sameDrive (const Drive& a, const Drive& b);
StickContent inspectStick (const juce::File& root);

// The small file that marks a stick as written by this program.
juce::File stickMarkerFile (const juce::File& root);
} // namespace cdj
