// Ported from baken (github.com/M-Igashi/baken, crates/baken-export), MIT License,
// Copyright (c) 2025 Jesse Pinkman (JPHFA). See THIRD_PARTY_NOTICES.md.

#pragma once

#include "Library.h"

#include <cstdint>
#include <map>
#include <vector>

// export.pdb: the DeviceSQL database rekordbox writes on a stick and every
// CDJ reads.
namespace cdj::pioneer
{
using Bytes = std::vector<std::uint8_t>;

constexpr size_t pdbPageSize = 4096;

// One string in DeviceSQL form: short ASCII (one length byte) or UTF-16LE.
Bytes encodeDeviceSqlString (const juce::String& text);

struct PdbOptions
{
    juce::String exportDate;            // YYYY-MM-DD, shown as the date tracks were added
    juce::String deviceName;            // the stick's name, may be empty
    std::map<int, juce::String> analyzePaths;   // track id → "/PIONEER/USBANLZ/…/ANLZ0000.DAT"
};

// The whole file: every table the players expect, a whole number of pages.
// Fails (with a reason) only when a row can't fit a page, e.g. an absurdly
// long title.
bool buildExportPdb (const StickLibrary& library, const PdbOptions& options, Bytes& out, juce::String& error);
} // namespace cdj::pioneer
