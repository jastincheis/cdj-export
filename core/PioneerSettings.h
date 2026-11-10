// Ported from baken (github.com/M-Igashi/baken, crates/baken-export), MIT License,
// Copyright (c) 2025 Jesse Pinkman (JPHFA). See THIRD_PARTY_NOTICES.md.

#pragma once

#include "PioneerPdb.h"

// The "My Settings" files rekordbox puts in PIONEER/ (DEVSETTING.DAT,
// MYSETTING.DAT, MYSETTING2.DAT, DJMMYSETTING.DAT). Players expect them on
// an export stick; these carry rekordbox's default settings.
namespace cdj::pioneer
{
// The four file names, in the order they are written.
const juce::StringArray& settingsFileNames();

// The default contents of one of them (empty for any other name).
Bytes defaultSettingsFile (const juce::String& name);

// True when `bytes` is a settings file a player saved onto the stick (its
// header names the player, not rekordbox): such a file is kept as it is, so
// what the DJ set on the player survives a new export.
bool savedByPlayer (const juce::MemoryBlock& bytes);

// CRC-16/XMODEM, the checksum of these files.
std::uint16_t crc16Xmodem (const std::uint8_t* data, size_t size);
} // namespace cdj::pioneer
