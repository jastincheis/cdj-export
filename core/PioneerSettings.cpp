// Ported from baken (github.com/M-Igashi/baken, crates/baken-export/src/settings.rs),
// MIT License, Copyright (c) 2025 Jesse Pinkman (JPHFA). See THIRD_PARTY_NOTICES.md.
//
// File layout and checksum rules as baken describes them: a 104-byte header
// (0x60, then brand, software and version as 32-byte fields, then the
// payload length), the payload, CRC-16/XMODEM and two zero bytes. baken
// copies the files from a rekordbox installation; there is none here, so
// the payloads are rekordbox's defaults, the values an empty rekordbox 6
// export carries.

#include "PioneerSettings.h"

#include <cstring>

namespace cdj::pioneer
{
namespace
{
struct Defaults
{
    const char* name;
    const char* brand;
    const char* version;
    std::vector<std::uint8_t> payload;
    bool checksumCoversHeader;      // DJMMYSETTING.DAT's checksum covers the header too
};

const std::vector<Defaults>& defaults()
{
    static const std::vector<Defaults> all {
        // Waveform blue, half overview, classic key display, current position in the centre.
        { "DEVSETTING.DAT", "PIONEER DJ", "6.6.1",
          { 0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
          false },
        // CDJ: on-air display, LCD brightness, quantize, auto cue level,
        // language, jog ring, slip flashing, disc slot, eject lock, sync,
        // play mode, quantize beat value, hot cue auto load and colour,
        // needle lock, time mode, jog mode, auto cue, master tempo, tempo
        // range, phase meter. One change from rekordbox's defaults: HOT CUE
        // COLOR is On (payload byte 26 = 0x81), so the pads light in the
        // colours this program gives them.
        { "MYSETTING.DAT", "PIONEER", "0.001",
          { 0x78, 0x56, 0x34, 0x12, 0x02, 0x00, 0x00, 0x00, 0x81, 0x83, 0x81, 0x88, 0x81, 0x01, 0x82, 0x81,
            0x81, 0x01, 0x01, 0x01, 0x82, 0x80, 0x80, 0x81, 0x80, 0x81, 0x81, 0x00, 0x00, 0x81, 0x00, 0x00,
            0x81, 0x81, 0x81, 0x80, 0x81, 0x80, 0x00, 0x00 },
          false },
        // CDJ: vinyl speed adjust, jog display, pad brightness, jog LCD
        // brightness, waveform divisions, waveform/phase meter, beat jump value.
        { "MYSETTING2.DAT", "PIONEER", "0.001",
          { 0x81, 0x80, 0x83, 0x83, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x81, 0x85, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
          false },
        // DJM mixer: fader curves, headphones, beat FX quantize, mic, talk
        // over, MIDI, brightness.
        { "DJMMYSETTING.DAT", "PioneerDJ", "1.000",
          { 0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x81, 0x82, 0x80, 0x80,
            0x81, 0x81, 0x80, 0x81, 0x80, 0x80, 0x85, 0x82, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00 },
          true } };

    return all;
}

void putField (Bytes& b, const char* text)
{
    const auto start = b.size();
    b.insert (b.end(), text, text + std::strlen (text));
    b.resize (start + 32, 0);
}
} // namespace

std::uint16_t crc16Xmodem (const std::uint8_t* data, size_t size)
{
    std::uint16_t crc = 0;

    for (size_t i = 0; i < size; ++i)
    {
        crc = (std::uint16_t) (crc ^ (data[i] << 8));

        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) != 0 ? (std::uint16_t) ((crc << 1) ^ 0x1021) : (std::uint16_t) (crc << 1);
    }

    return crc;
}

const juce::StringArray& settingsFileNames()
{
    static const juce::StringArray names { "DEVSETTING.DAT", "MYSETTING.DAT", "MYSETTING2.DAT", "DJMMYSETTING.DAT" };
    return names;
}

Bytes defaultSettingsFile (const juce::String& name)
{
    for (const auto& d : defaults())
    {
        if (name != d.name)
            continue;

        Bytes b { 0x60, 0, 0, 0 };
        putField (b, d.brand);
        putField (b, "rekordbox");
        putField (b, d.version);
        const auto length = (std::uint32_t) d.payload.size();

        for (int i = 0; i < 4; ++i)
            b.push_back ((std::uint8_t) (length >> (8 * i)));

        jassert (b.size() == 104);
        b.insert (b.end(), d.payload.begin(), d.payload.end());
        const auto crc = d.checksumCoversHeader ? crc16Xmodem (b.data(), b.size())
                                                : crc16Xmodem (b.data() + 104, d.payload.size());
        b.push_back ((std::uint8_t) crc);
        b.push_back ((std::uint8_t) (crc >> 8));
        b.push_back (0);
        b.push_back (0);
        return b;
    }

    return {};
}

bool savedByPlayer (const juce::MemoryBlock& bytes)
{
    // The software field (0x24, 32 bytes, NUL-terminated and then NUL or
    // space padded) says "rekordbox" in rekordbox's files and names the
    // player (e.g. "CDJ-2000NXS2") in one a player saved.
    if (bytes.getSize() < 0x68 || bytes[0] != 0x60 || bytes[1] != 0 || bytes[2] != 0 || bytes[3] != 0)
        return false;

    const auto* field = static_cast<const char*> (bytes.getData()) + 0x24;
    const auto software = juce::String::fromUTF8 (field, (int) strnlen (field, 32)).trim();
    return software.isNotEmpty() && software != "rekordbox";
}
} // namespace cdj::pioneer
