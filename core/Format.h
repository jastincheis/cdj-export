#pragma once

#include "Drives.h"

#include <functional>

// Formatting a USB stick for CDJs: one MBR partition, FAT32 (every CDJ) or
// exFAT (newer players only). Everything on the stick is erased.
//
// Safety: only a removable / USB disk is ever formatted, never one that holds
// the system; the whole disk is checked again right before erasing (same
// device, same size), and the user must type the stick's name to confirm.
namespace cdj
{
enum class FileSystem { fat32, exfat };

struct FormatPlan
{
    Drive drive;                    // as chosen in the window
    juce::String disk;              // whole disk: /dev/sdb, /dev/disk4, or Windows disk number
    juce::String partition;         // the mounted partition (/dev/sdb1) or drive letter (E:)
    juce::int64 diskBytes = 0;
    juce::String model;             // what the system calls the device, for the confirmation
};

// Checks the stick and fills the plan; false with a reason when this drive
// must not (or cannot) be formatted.
bool planFormat (const Drive& drive, FormatPlan& plan, juce::String& error);

// Empty when it can be done here; otherwise why not (e.g. FAT32 above 32 GB
// on Windows, which Windows' own formatter refuses).
juce::String formatLimitation (const FormatPlan& plan, FileSystem fs);

// A name every system and CDJ accepts: A-Z, 0-9, _ and -, at most 11 characters.
juce::String cleanVolumeLabel (const juce::String& label);

struct FormatResult
{
    bool ok = false;
    juce::String error;
    juce::String manualCommand;     // Linux without a password prompt: what to run in a terminal
    juce::File newRoot;             // where the stick is mounted afterwards, when known
};

// Erases and formats. May ask for the administrator password (Windows UAC,
// Linux polkit). Runs for a while: call it off the message thread.
FormatResult formatStick (const FormatPlan& plan, FileSystem fs, const juce::String& label,
                          const std::function<void (const juce::String&)>& status);
} // namespace cdj
