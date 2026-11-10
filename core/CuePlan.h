#pragma once

#include "Track.h"

// Markers and regions → Pioneer cues. A marker is a cue point; a region is a
// loop (cue at its start, loop end at its end). They are taken in time order:
// the first ones become hot cues A, B, C… and all of them also become memory
// cues, unless the options say otherwise.
namespace cdj
{
struct CueOptions
{
    bool hotCues = true;
    int hotCueCount = 8;            // A–H
    bool memoryCues = true;
    bool regionsAsLoops = true;     // off: a region is just a cue at its start
    bool colours = false;
    double offsetMs = 0.0;          // added to every position (MP3 encoder delay correction)
};

struct Cue
{
    enum class Kind { memory, hot };

    Kind kind = Kind::memory;
    int hotIndex = -1;              // 0 = A … 7 = H; -1 for memory cues
    juce::int64 startMs = 0;
    juce::int64 endMs = -1;         // loop end; -1 = not a loop
    juce::String name;
    int colour = 0;                 // Pioneer colour index, 0 = none

    bool isLoop() const { return endMs > startMs; }
    juce::String hotLetter() const { return hotIndex >= 0 ? juce::String::charToString ((juce::juce_wchar) ('A' + hotIndex)) : juce::String(); }
};

// Exact conversion: samples at `rate` → milliseconds, rounded to nearest.
juce::int64 samplesToMs (juce::int64 sample, double rate);

std::vector<Cue> planCues (const Track& track, const CueOptions& options);

// The default hot-cue colours of CDJ-3000 / rekordbox pads A–H, as
// rekordbox colour codes and RGB (for the XML and the ANLZ cue entries).
struct Rgb { juce::uint8 r = 0, g = 0, b = 0; };
int defaultHotCueColour (int hotIndex);
Rgb hotCueRgb (int hotIndex);
} // namespace cdj
