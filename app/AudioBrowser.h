#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace cdj
{
// Picks audio files, with a preview that plays the highlighted file.
//   Up / Down   move through the list       Left  parent folder (the one you came from stays selected)
//   Right       open the highlighted folder  Enter / double-click  open a folder, or add the files
//   Space       play / stop the preview      Esc   cancel
// `done` gets the chosen files (nothing when cancelled). The folder is remembered for next time.
void showAudioBrowser (juce::Component& parent, std::function<void (juce::StringArray)> done);
} // namespace cdj
