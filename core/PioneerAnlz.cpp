// Ported from baken (github.com/M-Igashi/baken, crates/baken-export), MIT License,
// Copyright (c) 2025 Jesse Pinkman (JPHFA). See THIRD_PARTY_NOTICES.md.
//
// baken's anlz/hash.rs (where the players look), anlz/section.rs (the PMAI
// container), anlz/cues.rs (PCOB/PCO2), anlz/generate/grid.rs (PQTZ, PQT2),
// anlz/generate/waveform.rs (every waveform, rules recovered by comparing
// rekordbox 7 analysis files with their audio), anlz/generate/assemble.rs
// (which section goes in which file, in rekordbox's order) and
// anlz/rewrite.rs (counting MP3 frames for PVBR).

#include "PioneerAnlz.h"

#include <algorithm>
#include <cmath>

namespace cdj::pioneer
{
namespace
{
// Everything in the analysis files is big-endian.
void put16 (Bytes& b, std::uint32_t v)
{
    b.push_back ((std::uint8_t) (v >> 8));
    b.push_back ((std::uint8_t) v);
}

void put32 (Bytes& b, std::uint32_t v)
{
    for (int i = 3; i >= 0; --i)
        b.push_back ((std::uint8_t) (v >> (8 * i)));
}

void append (Bytes& b, const Bytes& more) { b.insert (b.end(), more.begin(), more.end()); }

std::vector<std::uint16_t> utf16 (const juce::String& text)
{
    std::vector<std::uint16_t> units;

    for (auto p = text.getCharPointer(); ! p.isEmpty();)
    {
        auto c = (std::uint32_t) p.getAndAdvance();

        if (c >= 0x10000)
        {
            c -= 0x10000;
            units.push_back ((std::uint16_t) (0xd800 + (c >> 10)));
            units.push_back ((std::uint16_t) (0xdc00 + (c & 0x3ff)));
        }
        else
        {
            units.push_back ((std::uint16_t) c);
        }
    }

    return units;
}

// A tagged section: tag, header length, whole length, then the payload.
Bytes section (const char* tag, std::uint32_t headerLength, const Bytes& payload)
{
    Bytes s (tag, tag + 4);
    put32 (s, headerLength);
    put32 (s, (std::uint32_t) (12 + payload.size()));
    append (s, payload);
    return s;
}

// The PMAI file: a 28-byte header (rekordbox 7 writes words 3 to 6 as
// 1, 0x10000, 0x10000, 0), a PPTH section with the audio path, the rest.
Bytes anlzFile (const juce::String& usbPath, const std::vector<Bytes>& sections)
{
    Bytes path;

    for (auto u : utf16 (usbPath))
        put16 (path, u);

    put16 (path, 0);

    Bytes ppth;
    put32 (ppth, (std::uint32_t) path.size());
    append (ppth, path);

    std::vector<Bytes> all { section ("PPTH", 16, ppth) };
    all.insert (all.end(), sections.begin(), sections.end());

    size_t total = 0x1c;

    for (const auto& s : all)
        total += s.size();

    Bytes f { 'P', 'M', 'A', 'I' };
    put32 (f, 0x1c);
    put32 (f, (std::uint32_t) total);

    for (auto w : { 1u, 0x10000u, 0x10000u, 0u })
        put32 (f, w);

    for (const auto& s : all)
        append (f, s);

    return f;
}

//==============================================================================
// Cues.
constexpr std::uint32_t hotList = 1, memoryList = 0, noLoop = 0xffffffff;

std::uint32_t hotNumber (const Cue& c) { return c.kind == Cue::Kind::hot ? (std::uint32_t) c.hotIndex + 1 : 0; }
std::uint8_t cueType (const Cue& c) { return c.isLoop() ? 2 : 1; }
std::uint32_t loopEnd (const Cue& c) { return c.isLoop() ? (std::uint32_t) c.endMs : noLoop; }

// PCOB: the cue list the players before the NXS2 read, 56-byte PCPT entries.
Bytes pcob (std::uint32_t list, const std::vector<const Cue*>& cues)
{
    Bytes p;
    put32 (p, list);
    put16 (p, 0);
    put16 (p, (std::uint32_t) cues.size());
    put32 (p, list == memoryList && ! cues.empty() ? (std::uint32_t) cues.size() - 1 : noLoop);

    for (size_t i = 0; i < cues.size(); ++i)
    {
        const auto& c = *cues[i];
        p.insert (p.end(), { 'P', 'C', 'P', 'T' });
        put32 (p, 0x1c);
        put32 (p, 0x38);
        put32 (p, hotNumber (c));
        put32 (p, 0);           // status: 4 marks rekordbox's active loop, we have none
        put32 (p, 0x10000);
        // Memory cues are chained by list position; hot cues are not.
        const auto first = list == hotList || i == 0 ? 0xffffu : (std::uint32_t) (i - 1);
        const auto last = list == hotList || i + 1 == cues.size() ? 0xffffu : (std::uint32_t) (i + 1);
        put16 (p, first);
        put16 (p, last);
        p.push_back (cueType (c));
        p.insert (p.end(), { 0x00, 0x03, 0xe8 });
        put32 (p, (std::uint32_t) c.startMs);
        put32 (p, loopEnd (c));
        p.insert (p.end(), 16, 0);
    }

    return section ("PCOB", 0x18, p);
}

// The colours of rekordbox's hot cue menu: the code, the colour rekordbox
// shows (and writes in its XML) and the three colour bytes after the code
// (the player's own palette), as rekordbox 7.2.18 exported them (baken #236).
struct MenuColour { int code; Rgb shown; std::uint8_t bytes[3]; };

const MenuColour menuColours[] = {
    { 0x01, { 48, 90, 255 }, { 0x00, 0x00, 0xff } },  { 0x05, { 80, 180, 255 }, { 0x00, 0x70, 0xff } },
    { 0x09, { 0, 224, 255 }, { 0x00, 0xe0, 0xff } },  { 0x0e, { 31, 163, 146 }, { 0x00, 0xff, 0xa3 } },
    { 0x12, { 16, 177, 118 }, { 0x00, 0xff, 0x47 } }, { 0x16, { 40, 226, 20 }, { 0x1a, 0xff, 0x00 } },
    { 0x1a, { 165, 225, 22 }, { 0x80, 0xff, 0x00 } }, { 0x1e, { 180, 190, 4 }, { 0xe6, 0xff, 0x00 } },
    { 0x20, { 195, 175, 4 }, { 0xff, 0xe8, 0x00 } },  { 0x26, { 224, 100, 27 }, { 0xff, 0x5e, 0x00 } },
    { 0x2a, { 230, 40, 40 }, { 0xff, 0x00, 0x00 } },  { 0x2d, { 255, 18, 123 }, { 0xff, 0x00, 0x45 } },
    { 0x31, { 222, 68, 207 }, { 0xff, 0x00, 0xa1 } }, { 0x38, { 180, 50, 255 }, { 0xb3, 0x00, 0xff } },
    { 0x3c, { 170, 114, 255 }, { 0x4d, 0x00, 0xff } }, { 0x3e, { 100, 115, 255 }, { 0x1a, 0x00, 0xff } } };

// What rekordbox shows for each of its 62 hot cue colour codes (0x01–0x3e),
// from Deep Symmetry's beat-link: for codes outside the menu, which take the
// nearest menu colour.
const Rgb codeColours[62] = {
    { 0x30, 0x5a, 0xff }, { 0x50, 0x73, 0xff }, { 0x50, 0x8c, 0xff }, { 0x50, 0xa0, 0xff }, { 0x50, 0xb4, 0xff },
    { 0x50, 0xb0, 0xf2 }, { 0x50, 0xae, 0xe8 }, { 0x45, 0xac, 0xdb }, { 0x00, 0xe0, 0xff }, { 0x19, 0xda, 0xf0 },
    { 0x32, 0xd2, 0xe6 }, { 0x21, 0xb4, 0xb9 }, { 0x20, 0xaa, 0xa0 }, { 0x1f, 0xa3, 0x92 }, { 0x19, 0xa0, 0x8c },
    { 0x14, 0xa5, 0x84 }, { 0x14, 0xaa, 0x7d }, { 0x10, 0xb1, 0x76 }, { 0x30, 0xd2, 0x6e }, { 0x37, 0xde, 0x5a },
    { 0x3c, 0xeb, 0x50 }, { 0x28, 0xe2, 0x14 }, { 0x7d, 0xc1, 0x3d }, { 0x8c, 0xc8, 0x32 }, { 0x9b, 0xd7, 0x23 },
    { 0xa5, 0xe1, 0x16 }, { 0xa5, 0xdc, 0x0a }, { 0xaa, 0xd2, 0x08 }, { 0xb4, 0xc8, 0x05 }, { 0xb4, 0xbe, 0x04 },
    { 0xba, 0xb4, 0x04 }, { 0xc3, 0xaf, 0x04 }, { 0xe1, 0xaa, 0x00 }, { 0xff, 0xa0, 0x00 }, { 0xff, 0x96, 0x00 },
    { 0xff, 0x8c, 0x00 }, { 0xff, 0x75, 0x00 }, { 0xe0, 0x64, 0x1b }, { 0xe0, 0x46, 0x1e }, { 0xe0, 0x30, 0x1e },
    { 0xe0, 0x28, 0x23 }, { 0xe6, 0x28, 0x28 }, { 0xff, 0x37, 0x6f }, { 0xff, 0x2d, 0x6f }, { 0xff, 0x12, 0x7b },
    { 0xf5, 0x1e, 0x8c }, { 0xeb, 0x2d, 0xa0 }, { 0xe6, 0x37, 0xb4 }, { 0xde, 0x44, 0xcf }, { 0xde, 0x44, 0x8d },
    { 0xe6, 0x30, 0xb4 }, { 0xe6, 0x19, 0xdc }, { 0xe6, 0x00, 0xff }, { 0xdc, 0x00, 0xff }, { 0xcc, 0x00, 0xff },
    { 0xb4, 0x32, 0xff }, { 0xb9, 0x3c, 0xff }, { 0xc5, 0x42, 0xff }, { 0xaa, 0x5a, 0xff }, { 0xaa, 0x72, 0xff },
    { 0x82, 0x72, 0xff }, { 0x64, 0x73, 0xff } };

// Quantised loop length in beats, when the loop is a whole number of beats.
std::pair<int, int> loopBeats (const Cue& c, double bpm)
{
    if (! c.isLoop() || bpm <= 0.0)
        return { 0, 0 };

    const auto beats = (double) (c.endMs - c.startMs) / 1000.0 * bpm / 60.0;
    const auto rounded = std::round (beats);

    if (rounded >= 1.0 && std::abs (beats - rounded) < 0.02)
        return { (int) rounded, 1 };

    return { 0, 0 };
}

// PCO2: the extended cue list of the NXS2 and later, with names and colours.
Bytes pco2 (std::uint32_t list, const std::vector<const Cue*>& cues, double bpm)
{
    Bytes p;
    put32 (p, list);
    put16 (p, (std::uint32_t) cues.size());
    put16 (p, 0);

    for (const auto* cue : cues)
    {
        const auto& c = *cue;
        Bytes comment;

        if (c.name.isNotEmpty())
        {
            for (auto u : utf16 (c.name))
                put16 (comment, u);

            put16 (comment, 0);
        }

        const auto length = 0x2c + comment.size() + 4 + 40;
        Bytes e { 'P', 'C', 'P', '2' };
        put32 (e, 0x10);
        put32 (e, (std::uint32_t) length);
        put32 (e, hotNumber (c));
        e.push_back (cueType (c));
        e.insert (e.end(), { 0x00, 0x03, 0xe8 });
        put32 (e, (std::uint32_t) c.startMs);
        put32 (e, loopEnd (c));
        e.push_back (0);    // memory cue colour id: none
        e.insert (e.end(), { 0x01, 0, 0, 0, 0, 0, 0 });
        const auto [numerator, denominator] = loopBeats (c, bpm);
        put16 (e, (std::uint32_t) numerator);
        put16 (e, (std::uint32_t) denominator);
        put32 (e, (std::uint32_t) comment.size());
        append (e, comment);

        if (list == hotList)
        {
            const auto colour = hotCueColourBytes (c.colour);
            e.insert (e.end(), colour.begin(), colour.end());
        }
        else
        {
            e.insert (e.end(), 4, 0);
        }

        e.resize (length, 0);
        append (p, e);
    }

    return section ("PCO2", 0x14, p);
}

//==============================================================================
// Waveforms: 150 columns per second of the mono mix; the previews (400, 100
// and 1200 columns) are made from those columns.
constexpr double columnsPerSecond = 150.0;
constexpr size_t previewColumns = 400, tinyColumns = 100, colourColumns = 1200;

constexpr double whitenessLowpassHz = 150.0;
constexpr double lowHz = 300.0, midLowHz = 250.0, midHighHz = 1500.0, highHz = 2500.0;
constexpr double release[3] = { 0.97, 0.93, 0.90 };
constexpr double gainTarget[3] = { 78.0, 105.0, 115.0 };
constexpr double gainFloor[3] = { 80, 80, 95 };
constexpr double gainCap[3] = { 267, 300, 500 };
constexpr double bandScale[3] = { 1.25, 1.15, 0.6 };
constexpr double pwv6Scale[3] = { 0.56, 0.71, 2.0 };
constexpr double pwv4Scale[3] = { 2.4, 1.6, 1.1 };

// rekordbox never writes a preview column lower than these; Nexus-era
// firmware reads a zero column as an unfinished preview (baken #285).
constexpr std::uint8_t pwavMinHeight = 2, pwv2MinHeight = 1;
constexpr std::uint8_t pwavFloor = (5 << 5) | pwavMinHeight;

struct Biquad
{
    double b[3] {}, a[2] {}, x[2] {}, y[2] {};

    static Biquad make (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        Biquad q;
        q.b[0] = b0 / a0;
        q.b[1] = b1 / a0;
        q.b[2] = b2 / a0;
        q.a[0] = a1 / a0;
        q.a[1] = a2 / a0;
        return q;
    }

    // RBJ cookbook, Q = 1/sqrt(2).
    static Biquad lowpass (double fc, double rate)
    {
        const auto w = 2.0 * juce::MathConstants<double>::pi * fc / rate;
        const auto s = std::sin (w), c = std::cos (w);
        const auto alpha = s / juce::MathConstants<double>::sqrt2;   // s / (2Q)
        return make ((1.0 - c) / 2.0, 1.0 - c, (1.0 - c) / 2.0, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    static Biquad highpass (double fc, double rate)
    {
        const auto w = 2.0 * juce::MathConstants<double>::pi * fc / rate;
        const auto s = std::sin (w), c = std::cos (w);
        const auto alpha = s / juce::MathConstants<double>::sqrt2;   // s / (2Q)
        return make ((1.0 + c) / 2.0, -(1.0 + c), (1.0 + c) / 2.0, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    double step (double in)
    {
        const auto out = b[0] * in + b[1] * x[0] + b[2] * x[1] - a[0] * y[0] - a[1] * y[1];
        x[1] = x[0];
        x[0] = in;
        y[1] = y[0];
        y[0] = out;
        return out;
    }
};

struct Waveforms
{
    std::vector<std::uint8_t> pwv3;                 // 5-bit height, 3-bit whiteness per column
    std::vector<std::uint16_t> pwv5;                // rrrgggbbbhhhhh00 per column
    std::vector<std::array<std::uint8_t, 3>> pwv7;  // low, mid, high per column
    std::uint16_t gains[3] {};
    std::vector<std::uint8_t> pwav, pwv2;
    std::vector<std::array<std::uint8_t, 3>> pwv6;
    std::vector<std::array<std::uint8_t, 6>> pwv4;
};

// `n` items in `parts` consecutive ranges, each at least one item long
// (they overlap when n < parts). n ≥ 1.
std::vector<std::pair<size_t, size_t>> ranges (size_t n, size_t parts)
{
    std::vector<std::pair<size_t, size_t>> out;

    for (size_t k = 0; k < parts; ++k)
    {
        const auto a = std::min (k * n / parts, n - 1);
        const auto b = std::clamp ((k + 1) * n / parts, a + 1, n);
        out.emplace_back (a, b);
    }

    return out;
}

std::uint8_t toByte (double v, double top)
{
    return (std::uint8_t) std::clamp (std::round (v), 0.0, top);
}

Waveforms analyse (const Measured& audio)
{
    const auto& columns = audio.columns;
    const auto n = columns.size();
    Waveforms w;
    double trackPeak = 0.0;

    for (const auto& c : columns)
        trackPeak = std::max (trackPeak, c.peak);

    // Height: floor(31.5 · (peak / track peak)²); whiteness: 7 − floor(8 ·
    // low-passed peak / peak), silence counts as white.
    std::vector<std::uint8_t> heights, whites;

    for (const auto& c : columns)
    {
        std::uint8_t h = 0;

        if (trackPeak > 0.0)
        {
            const auto q = c.peak / trackPeak;
            h = (std::uint8_t) std::min (31.0, std::floor (31.5 * q * q));
        }

        std::uint8_t white = 7;

        if (c.peak > 0.0)
            white = (std::uint8_t) (7 - std::min (7, (int) std::floor (8.0 * std::clamp (c.white / c.peak, 0.0, 1.0))));

        heights.push_back (h);
        whites.push_back (white);
        w.pwv3.push_back ((std::uint8_t) ((white << 5) | h));
    }

    // Band envelopes with release, then the gain that scales the loudest
    // column of each band to its target level.
    std::vector<std::array<double, 3>> envelope (n);

    for (size_t i = 0; i < n; ++i)
        for (int j = 0; j < 3; ++j)
            envelope[i][(size_t) j] = std::max (columns[i].band[j], i == 0 ? 0.0 : envelope[i - 1][(size_t) j] * release[j]);

    for (int j = 0; j < 3; ++j)
    {
        double top = 0.0;

        for (const auto& e : envelope)
            top = std::max (top, e[(size_t) j]);

        w.gains[j] = (std::uint16_t) (top > 0.0 ? std::clamp (std::round (gainTarget[j] / top), gainFloor[j], gainCap[j])
                                                : gainFloor[j]);
    }

    for (const auto& e : envelope)
    {
        std::array<std::uint8_t, 3> out {};

        for (int j = 0; j < 3; ++j)
            out[(size_t) j] = toByte (bandScale[j] * w.gains[j] * e[(size_t) j], 127.0);

        w.pwv7.push_back (out);
    }

    // Colour: each band's share of the strongest band, on a square root so
    // faint bands still tint the column.
    for (size_t i = 0; i < n; ++i)
    {
        const auto& c = columns[i];
        const auto top = std::max ({ c.band[0], c.band[1], c.band[2] });
        const auto level = [top] (double b) -> std::uint16_t
        {
            return top <= 0.0 ? 0 : (std::uint16_t) std::min (7.0, std::round (7.0 * std::sqrt (b / top)));
        };

        w.pwv5.push_back ((std::uint16_t) ((level (c.band[0]) << 13) | (level (c.band[1]) << 10)
                                           | (level (c.band[2]) << 7) | (heights[i] << 2)));
    }

    // Previews from the RMS of column groups.
    const auto rmsOver = [&] (std::pair<size_t, size_t> r)
    {
        double sum = 0.0;
        std::uint64_t samples = 0;

        for (auto i = r.first; i < r.second; ++i)
        {
            sum += columns[i].sumSquares;
            samples += columns[i].samples;
        }

        return samples == 0 ? 0.0 : std::sqrt (sum / (double) samples);
    };

    std::vector<double> rms400;

    for (auto r : ranges (n, previewColumns))
        rms400.push_back (rmsOver (r));

    const auto rms400Max = *std::max_element (rms400.begin(), rms400.end());
    std::vector<std::uint8_t> white200;

    for (auto [a, b] : ranges (n, previewColumns / 2))
    {
        double sum = 0.0;

        for (auto i = a; i < b; ++i)
            sum += whites[i];

        white200.push_back ((std::uint8_t) std::round (sum / (double) (b - a)));
    }

    for (size_t k = 0; k < rms400.size(); ++k)
    {
        const auto h = rms400Max > 0.0 ? (std::uint8_t) std::round (24.0 * rms400[k] / rms400Max) : (std::uint8_t) 0;
        w.pwav.push_back (h < pwavMinHeight ? pwavFloor
                                            : (std::uint8_t) ((std::min<std::uint8_t> (white200[k / 2], 7) << 5)
                                                              | std::min<std::uint8_t> (h, 31)));
    }

    std::vector<double> rms100;

    for (auto r : ranges (n, tinyColumns))
        rms100.push_back (rmsOver (r));

    const auto rms100Max = *std::max_element (rms100.begin(), rms100.end());

    for (auto rms : rms100)
    {
        const auto h = rms100Max > 0.0 ? (std::uint8_t) std::min (15.0, std::round (15.0 * std::sqrt (rms / rms100Max)))
                                       : (std::uint8_t) 0;
        w.pwv2.push_back (std::max (h, pwv2MinHeight));
    }

    // Three-band and colour previews from the detail bytes.
    for (auto [a, b] : ranges (n, colourColumns))
    {
        std::array<std::uint8_t, 3> out {};

        for (int j = 0; j < 3; ++j)
        {
            double sum = 0.0;

            for (auto i = a; i < b; ++i)
                sum += w.pwv7[i][(size_t) j];

            out[(size_t) j] = toByte (pwv6Scale[j] * sum / (double) (b - a), 127.0);
        }

        w.pwv6.push_back (out);
    }

    for (const auto& b6 : w.pwv6)
    {
        std::array<std::uint8_t, 6> b {};

        for (int j = 0; j < 3; ++j)
            b[(size_t) (3 + j)] = toByte (pwv4Scale[j] * b6[(size_t) j], 127.0);

        const double r = b[3], g = b[4], bl = b[5];

        if (std::max ({ r, g, bl }) > 0.0)
        {
            b[2] = toByte (0.91 * std::sqrt (r * r + g * g + bl * bl), 127.0);
            b[0] = toByte (0.88 * std::max ({ r, g, bl }) + 38.0, 127.0);
            b[1] = toByte (203.0 - 0.49 * b[0], 255.0);
        }

        w.pwv4.push_back (b);
    }

    return w;
}

// len_data, 0x00010000, data, with a 20-byte header (PWAV, PWV2).
Bytes previewSection (const char* tag, const std::vector<std::uint8_t>& data)
{
    Bytes p;
    put32 (p, (std::uint32_t) data.size());
    put32 (p, 0x10000);
    p.insert (p.end(), data.begin(), data.end());
    return section (tag, 0x14, p);
}

// len_entry_bytes, len_entries, unknown, data, with a 24-byte header.
Bytes detailSection (const char* tag, std::uint32_t entryBytes, std::uint32_t unknown, const Bytes& data)
{
    Bytes p;
    put32 (p, entryBytes);
    put32 (p, (std::uint32_t) (data.size() / entryBytes));
    put32 (p, unknown);
    append (p, data);
    return section (tag, 0x18, p);
}

// PVBR: 400 seek entries and a trailer. An MP3 gets no entries and its audio
// frames × 1152 (rekordbox's form for CBR); other files are all zero.
Bytes pvbr (std::uint32_t mp3Frames)
{
    Bytes p (4 + 400 * 4 + 4, 0);

    if (mp3Frames > 0)
    {
        const auto total = mp3Frames * 1152u;

        for (int i = 0; i < 4; ++i)
            p[1604 + (size_t) i] = (std::uint8_t) (total >> (8 * (3 - i)));
    }

    return section ("PVBR", 0x10, p);
}

Bytes pqtz (const std::vector<Beat>& beats)
{
    Bytes p;
    put32 (p, 0);
    put32 (p, 0x00080000);
    put32 (p, (std::uint32_t) beats.size());

    for (const auto& b : beats)
    {
        put16 (p, (std::uint32_t) b.number);
        put16 (p, (std::uint32_t) b.tempo);
        put32 (p, b.timeMs);
    }

    return section ("PQTZ", 0x18, p);
}

// PQT2 as rekordbox writes it without an extended grid.
Bytes pqt2Empty()
{
    Bytes p (44, 0);
    p[4] = 0x01;
    p[7] = 0x02;
    return section ("PQT2", 0x38, p);
}
} // namespace

//==============================================================================
juce::String anlzFolder (const juce::String& usbPath)
{
    // Recovered from the rekordbox binary by morizkraemer/fourfour; baken
    // verified it on every track of a real export. The hash is taken modulo
    // 200003, so two tracks can share a folder (they get ANLZ0000, ANLZ0001).
    std::uint32_t h = 0;

    for (auto unit : utf16 (usbPath))
    {
        h = h * 0x5bc9u + unit;
        h = h * 0x93b5u + unit;
    }

    const auto r = h % 200003u;
    const auto p = (r & 1) | ((r >> 1) & 2) | ((r >> 4) & 4) | ((r >> 4) & 8) | ((r >> 5) & 0x10)
                 | ((r >> 8) & 0x20) | ((r >> 10) & 0x40);

    return "/PIONEER/USBANLZ/P" + juce::String::toHexString ((int) p).toUpperCase().paddedLeft ('0', 3) + "/"
         + juce::String::toHexString ((int) r).toUpperCase().paddedLeft ('0', 8);
}

std::array<std::uint8_t, 4> hotCueColourBytes (int code)
{
    // No colour, and green, go out as rekordbox 7 writes them: code 0 with
    // the green bytes (rekordbox writes both forms for green; both show green).
    const std::array<std::uint8_t, 4> noColour { 0, 0x1a, 0xff, 0x00 };

    if (code <= 0 || code > 0x3e)
        return noColour;

    const auto shown = codeColours[code - 1];
    const MenuColour* best = nullptr;
    int bestDistance = 0;

    for (const auto& m : menuColours)
    {
        const auto d = [] (int x, int y) { return (x - y) * (x - y); };
        const auto distance = m.code == code ? -1 : d (shown.r, m.shown.r) + d (shown.g, m.shown.g) + d (shown.b, m.shown.b);

        if (best == nullptr || distance < bestDistance)
        {
            best = &m;
            bestDistance = distance;
        }
    }

    if (best->code == 0x16)
        return noColour;

    return { (std::uint8_t) best->code, best->bytes[0], best->bytes[1], best->bytes[2] };
}

bool measureAudio (const juce::File& file, Measured& measured, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0)
    {
        error = "Can't read \"" + file.getFileName() + "\" to draw its waveform.";
        return false;
    }

    const auto rate = reader->sampleRate;
    const auto channels = (int) reader->numChannels;
    Biquad white = Biquad::lowpass (whitenessLowpassHz, rate), low = Biquad::lowpass (lowHz, rate),
           midHigh = Biquad::highpass (midLowHz, rate), midLow = Biquad::lowpass (midHighHz, rate),
           high = Biquad::highpass (highHz, rate);

    measured = {};
    measured.sampleRate = rate;
    Measured::Column column;
    auto nextEdge = (juce::int64) std::llround (rate / columnsPerSecond);
    juce::int64 frames = 0;
    const auto total = reader->lengthInSamples;
    juce::AudioBuffer<float> buffer (channels, 65536);

    for (juce::int64 position = 0; position < total; position += buffer.getNumSamples())
    {
        const auto count = (int) std::min ((juce::int64) buffer.getNumSamples(), total - position);

        if (! reader->read (&buffer, 0, count, position, true, true))
        {
            error = "Reading \"" + file.getFileName() + "\" failed.";
            return false;
        }

        for (int i = 0; i < count; ++i)
        {
            while (frames >= nextEdge)
            {
                measured.columns.push_back (column);
                column = {};
                nextEdge = (juce::int64) std::llround ((double) (measured.columns.size() + 1) * rate / columnsPerSecond);
            }

            double x = 0.0;

            for (int ch = 0; ch < channels; ++ch)
                x += buffer.getSample (ch, i);

            x /= channels;
            column.peak = std::max (column.peak, std::abs (x));
            column.white = std::max (column.white, std::abs (white.step (x)));
            column.band[0] = std::max (column.band[0], std::abs (low.step (x)));
            column.band[1] = std::max (column.band[1], std::abs (midLow.step (midHigh.step (x))));
            column.band[2] = std::max (column.band[2], std::abs (high.step (x)));
            column.sumSquares += x * x;
            ++column.samples;
            ++frames;
        }
    }

    // ceil(frames · 150 / rate) columns: a last column that starts at the
    // end of the audio stays empty.
    const auto n = std::max ((size_t) 1, (size_t) std::ceil ((double) frames * columnsPerSecond / rate));
    measured.columns.push_back (column);
    measured.columns.resize (n);
    measured.frames = frames;
    return true;
}

std::uint32_t mp3AudioFrames (const juce::File& mp3)
{
    juce::MemoryBlock block;

    if (! mp3.loadFileAsData (block))
        return 0;

    const auto* data = static_cast<const std::uint8_t*> (block.getData());
    const auto size = block.getSize();
    size_t pos = 0;

    if (size > 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3')
        pos = 10 + (((size_t) (data[6] & 0x7f) << 21) | ((size_t) (data[7] & 0x7f) << 14)
                    | ((size_t) (data[8] & 0x7f) << 7) | (size_t) (data[9] & 0x7f));

    static constexpr std::uint32_t bitrates[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static constexpr std::uint32_t rates[4] = { 44100, 48000, 32000, 0 };
    std::uint32_t frames = 0;
    bool first = true;

    while (pos + 4 <= size)
    {
        const auto* h = data + pos;
        const auto sync = h[0] == 0xff && (h[1] & 0xe0) == 0xe0;
        const auto mpeg1Layer3 = (h[1] & 0x18) == 0x18 && (h[1] & 0x06) == 0x02;
        const auto bitrate = bitrates[h[2] >> 4];
        const auto rate = rates[(h[2] >> 2) & 3];

        if (! (sync && mpeg1Layer3) || bitrate == 0 || rate == 0)
        {
            if (frames == 0)
            {
                ++pos;      // junk before the first frame
                continue;
            }

            break;
        }

        const auto length = (size_t) (144 * bitrate * 1000 / rate + ((h[2] >> 1) & 1));

        if (first)
        {
            first = false;
            const auto end = std::min (pos + length, size);
            const auto isHeader = [&] (const char* tag) { return std::search (data + pos, data + end, tag, tag + 4) != data + end; };

            if (isHeader ("Xing") || isHeader ("Info"))
            {
                pos += length;
                continue;
            }
        }

        ++frames;
        pos += length;
    }

    return frames;
}

std::vector<Beat> gridBeats (double bpm, double firstBeatMs, double durationMs)
{
    std::vector<Beat> beats;

    if (bpm <= 0.0 || ! std::isfinite (bpm))
        return beats;

    // Beats before the first downbeat count back 4, 3, 2, 1 to the start.
    const auto period = 60000.0 / bpm;
    const auto tempo = (int) std::lround (bpm * 100.0);
    auto k = (juce::int64) std::ceil (-firstBeatMs / period - 1.0e-9);

    for (;; ++k)
    {
        const auto t = firstBeatMs + (double) k * period;

        if (t >= durationMs)
            break;

        if (t < 0.0)
            continue;

        beats.push_back ({ (int) (((k % 4) + 4) % 4) + 1, tempo, (std::uint32_t) std::llround (t) });
    }

    return beats;
}

AnlzFiles buildAnlzFiles (const StickTrack& track, const juce::String& usbPath, const Measured& audio,
                          std::uint32_t mp3Frames)
{
    // Hot cues A–H and memory cues, each list in time order (hot cues by pad).
    // The players before the NXS2 read only the .DAT's PCOB, which holds hot
    // cues A–C (the pads they have); D–H go in the .EXT's, and PCO2 lists them all.
    std::vector<const Cue*> hot, memory, datHot, extHot;

    for (const auto& c : track.cues)
    {
        if (c.kind == Cue::Kind::hot && c.hotIndex >= 0 && c.hotIndex < 8)
            hot.push_back (&c);
        else if (c.kind == Cue::Kind::memory)
            memory.push_back (&c);
    }

    std::stable_sort (hot.begin(), hot.end(), [] (auto* a, auto* b) { return a->hotIndex < b->hotIndex; });
    std::stable_sort (memory.begin(), memory.end(), [] (auto* a, auto* b) { return a->startMs < b->startMs; });

    for (auto* c : hot)
        (c->hotIndex < 3 ? datHot : extHot).push_back (c);

    const auto waves = analyse (audio);
    const auto beats = gridBeats (track.bpm, track.firstBeatMs, audio.durationMs());

    Bytes pwv3 (waves.pwv3.begin(), waves.pwv3.end()), pwv5, pwv4, pwv7, pwv6;

    for (auto v : waves.pwv5)
        put16 (pwv5, v);

    for (const auto& b : waves.pwv4)
        pwv4.insert (pwv4.end(), b.begin(), b.end());

    for (const auto& b : waves.pwv7)
        pwv7.insert (pwv7.end(), b.begin(), b.end());

    put32 (pwv6, 3);
    put32 (pwv6, (std::uint32_t) waves.pwv6.size());

    for (const auto& b : waves.pwv6)
        pwv6.insert (pwv6.end(), b.begin(), b.end());

    Bytes pwvc;
    put16 (pwvc, 0);

    for (auto g : waves.gains)
        put16 (pwvc, g);

    AnlzFiles files;
    files.dat = anlzFile (usbPath, { pvbr (mp3Frames), pqtz (beats), previewSection ("PWAV", waves.pwav),
                                     previewSection ("PWV2", waves.pwv2), pcob (hotList, datHot),
                                     pcob (memoryList, memory) });
    files.ext = anlzFile (usbPath, { detailSection ("PWV3", 1, 0x00960000, pwv3), pcob (hotList, extHot),
                                     pcob (memoryList, {}), pco2 (hotList, hot, track.bpm),
                                     pco2 (memoryList, memory, track.bpm), pqt2Empty(),
                                     detailSection ("PWV5", 2, 0x00960305, pwv5), detailSection ("PWV4", 6, 0, pwv4) });
    files.twoEx = anlzFile (usbPath, { detailSection ("PWV7", 3, 0x00960000, pwv7), section ("PWV6", 0x14, pwv6),
                                       section ("PWVC", 0x0e, pwvc) });
    return files;
}
} // namespace cdj::pioneer
