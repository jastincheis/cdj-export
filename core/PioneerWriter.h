#pragma once

#include "Library.h"

// Writes the whole Pioneer library of a stick from the StickLibrary:
// PIONEER/rekordbox/export.pdb, an ANLZ0000.DAT/.EXT/.2EX per track under
// PIONEER/USBANLZ, and the *SETTING.DAT files. Matches cdj::PioneerWriter,
// so it plugs straight into exportToStick().
//
// Everything is built in PIONEER.adormis-new next to the old PIONEER folder
// and swapped in at the end, so a stick pulled out half way keeps its old
// library whole. The list of tracks (PIONEER/adormis-cdj-export.json) and
// settings a player saved onto the stick are carried over.
namespace cdj
{
bool writePioneerLibrary (const StickLibrary& library, const juce::File& stickRoot, juce::String& error);
} // namespace cdj
