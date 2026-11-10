#pragma once

#include "Format.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace cdj
{
// Asks how to format the stick (FAT32 / exFAT, new name), makes the user type
// the stick's current name to confirm, then formats it.
// `done` gets the new mount point (or an empty File) after a successful format.
void showFormatDialog (juce::Component& parent, const Drive& drive, std::function<void (juce::File)> done);
} // namespace cdj
