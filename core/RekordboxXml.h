#pragma once

#include "Library.h"

// The fallback: a rekordbox XML file (File > Preferences > Advanced >
// rekordbox xml) that lists the tracks on the stick with their hot cues,
// memory cues and loops. rekordbox imports it as-is.
namespace cdj
{
// file://localhost/E:/Contents/My%20Track.wav, the way rekordbox writes it.
juce::String rekordboxLocation (const juce::File& file);

juce::String rekordboxXml (const StickLibrary& library, const juce::File& stickRoot, const juce::String& version);
} // namespace cdj
