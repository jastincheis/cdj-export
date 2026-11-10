// Ported from baken (github.com/M-Igashi/baken, crates/baken-export), MIT License,
// Copyright (c) 2025 Jesse Pinkman (JPHFA). See THIRD_PARTY_NOTICES.md.
//
// The page layout, row layouts and the rows rekordbox writes into every
// export are baken's (pdb/page.rs, pdb/rows.rs, pdb/fixed.rs, pdb/writer.rs,
// build.rs), which reproduce real rekordbox 7 exports byte for byte. The
// field names follow Deep Symmetry's description of the format.

#include "PioneerPdb.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace cdj::pioneer
{
namespace
{
constexpr size_t heapStart = 0x28;
constexpr size_t groupBytes = 36;      // 16 row offsets + presence mask + last-write mask
// The largest row a page holds: an empty page's heap less the first row's
// offset and the two words of its group, in whole 4-byte units.
constexpr size_t maxRow = (pdbPageSize - heapStart - 6) / 4 * 4;
constexpr std::uint32_t noPage = 0x03ffffff;
constexpr std::uint32_t numTables = 20;

size_t align4 (size_t n) { return (n + 3) & ~(size_t) 3; }

void put16 (Bytes& b, std::uint16_t v)
{
    b.push_back ((std::uint8_t) v);
    b.push_back ((std::uint8_t) (v >> 8));
}

void put32 (Bytes& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back ((std::uint8_t) (v >> (8 * i)));
}

void set16 (Bytes& b, size_t at, std::uint16_t v)
{
    b[at] = (std::uint8_t) v;
    b[at + 1] = (std::uint8_t) (v >> 8);
}

void set32 (Bytes& b, size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b[at + (size_t) i] = (std::uint8_t) (v >> (8 * i));
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

// rekordbox packs ASCII strings but starts every UTF-16 string on a 4-byte
// boundary of the row. A CDJ-2000NXS2 froze on a stick where they weren't
// (baken issue #116).
size_t stringAt (size_t at, const Bytes& encoded)
{
    return encoded[0] == 0x90 ? align4 (at) : at;
}

//==============================================================================
// Pages: 4096 bytes, a 40-byte header, rows growing up from 0x28 and a row
// directory growing down from the end in groups of 16.
size_t directoryBytes (size_t rows)
{
    const auto rest = rows % 16;
    return rows / 16 * groupBytes + (rest > 0 ? rest * 2 + 4 : 0);
}

struct DataPage
{
    std::uint32_t index = 0, table = 0, next = noPage, sequence = 0;
    bool fixedStyle = false;    // colors and columns count their rows differently in the header
    std::vector<Bytes> rows;
    size_t heapUsed = 0;

    bool tryPush (const Bytes& row)
    {
        const auto need = heapUsed + align4 (row.size()) + directoryBytes (rows.size() + 1);

        if (need > pdbPageSize - heapStart || rows.size() >= 0x1fff)
            return false;

        heapUsed += align4 (row.size());
        rows.push_back (row);
        return true;
    }

    Bytes toBytes() const
    {
        Bytes p (pdbPageSize, 0);
        const auto n = rows.size();
        const auto freeBytes = pdbPageSize - heapStart - heapUsed - directoryBytes (n);
        const std::uint16_t unknown5 = fixedStyle ? (std::uint16_t) n : 1;
        const std::uint16_t numRowsLarge = fixedStyle ? 0 : (std::uint16_t) (n > 0 ? n - 1 : 0);

        set32 (p, 0x04, index);
        set32 (p, 0x08, table);
        set32 (p, 0x0c, next);
        set32 (p, 0x10, sequence);
        // num_row_offsets (13 bits) and num_rows (11 bits), packed.
        p[0x18] = (std::uint8_t) (n & 0xff);
        p[0x19] = (std::uint8_t) (((n & 7) << 5) | ((n >> 8) & 0x1f));
        p[0x1a] = (std::uint8_t) (n >> 3);
        p[0x1b] = 0x24;     // data page
        set16 (p, 0x1c, (std::uint16_t) freeBytes);
        set16 (p, 0x1e, (std::uint16_t) heapUsed);
        set16 (p, 0x20, unknown5);
        set16 (p, 0x22, numRowsLarge);

        // Each row group ends with the presence mask and DeviceSQL's record
        // of the last write (the rows from num_rows_large on), which
        // rekordbox clears before it next changes the page (baken #293).
        const auto lastWriteFrom = fixedStyle ? (size_t) 0 : (n > 0 ? n - 1 : 0);
        size_t offset = 0;

        for (size_t i = 0; i < n; ++i)
        {
            const auto& row = rows[i];
            std::copy (row.begin(), row.end(), p.begin() + (std::ptrdiff_t) (heapStart + offset));

            const auto base = pdbPageSize - (i / 16) * groupBytes;
            const auto slot = i % 16;
            set16 (p, base - 6 - 2 * slot, (std::uint16_t) offset);

            const auto bit = (std::uint16_t) (1u << slot);
            set16 (p, base - 4, (std::uint16_t) (p[base - 4] | (p[base - 3] << 8) | bit));

            if (i >= lastWriteFrom)
                set16 (p, base - 2, (std::uint16_t) (p[base - 2] | (p[base - 1] << 8) | bit));

            offset += align4 (row.size());
        }

        return p;
    }
};

// The page every table starts with. `next` is the first data page, or the
// empty candidate page when the table has no rows.
Bytes indexPage (std::uint32_t index, std::uint32_t table, std::uint32_t next, std::optional<std::uint32_t> firstData)
{
    Bytes p (pdbPageSize, 0);
    set32 (p, 0x04, index);
    set32 (p, 0x08, table);
    set32 (p, 0x0c, next);
    set32 (p, 0x10, 1);
    p[0x1b] = 0x64;     // index page
    set16 (p, 0x20, 0x1fff);
    set16 (p, 0x22, 0x1fff);
    set16 (p, 0x24, 0x03ec);

    size_t at = heapStart;

    for (auto w : { index, firstData.value_or (noPage), noPage, 0u, 0x1fff0000u })
    {
        set32 (p, at, w);
        at += 4;
    }

    for (int i = 0; i < 1004; ++i, at += 4)
        set32 (p, at, 0x1ffffff8);

    jassert (at == pdbPageSize - 20);
    return p;
}

struct TablePointer
{
    std::uint32_t type, emptyCandidate, firstPage, lastPage;
};

Bytes headerPage (std::uint32_t tables, std::uint32_t nextUnused, std::uint32_t sequence,
                  const std::vector<TablePointer>& pointers)
{
    Bytes p (pdbPageSize, 0);
    set32 (p, 0x04, (std::uint32_t) pdbPageSize);
    set32 (p, 0x08, tables);
    set32 (p, 0x0c, nextUnused);
    set32 (p, 0x10, 1);
    set32 (p, 0x14, sequence);
    size_t at = 0x1c;

    for (const auto& t : pointers)
        for (auto w : { t.type, t.emptyCandidate, t.firstPage, t.lastPage })
        {
            set32 (p, at, w);
            at += 4;
        }

    return p;
}

//==============================================================================
// Rows. Layouts as rekordbox 7 writes them (baken pdb/rows.rs).

// The `0x03, one offset byte per string, strings` tail of artist and album
// rows, allocated align4(header) + Σ align4(string) + 4 bytes like rekordbox.
Bytes withStrings (Bytes r, const std::vector<juce::String>& strings)
{
    const auto header = r.size() + 1 + strings.size();
    auto allocated = align4 (header) + 4;
    r.push_back (0x03);
    r.resize (header, 0);

    for (size_t i = 0; i < strings.size(); ++i)
    {
        const auto s = encodeDeviceSqlString (strings[i]);
        const auto at = stringAt (r.size(), s);
        r[header - strings.size() + i] = (std::uint8_t) at;
        r.resize (at, 0);
        append (r, s);
        allocated += align4 (s.size());
    }

    r.resize (allocated, 0);
    return r;
}

// artists (0x02): 0x60, index_shift, id, 0x03, name_offset, name.
Bytes artistRow (size_t positionInPage, std::uint32_t id, const juce::String& name)
{
    Bytes r;
    put16 (r, 0x60);
    put16 (r, (std::uint16_t) (positionInPage * 0x20));
    put32 (r, id);
    return withStrings (std::move (r), { name });
}

// colors (0x06): 0u32, id u8, id u16, 0u8, name.
Bytes colorRow (std::uint8_t id, const juce::String& name)
{
    Bytes r { 0, 0, 0, 0, id, id, 0, 0 };
    append (r, encodeDeviceSqlString (name));
    return r;
}

// playlist_tree (0x07): parent, 0, sort_order, id, is_folder, name.
Bytes playlistNodeRow (std::uint32_t parent, std::uint32_t sortOrder, std::uint32_t id, bool isFolder,
                       const juce::String& name)
{
    Bytes r;

    for (auto v : { parent, 0u, sortOrder, id, isFolder ? 1u : 0u })
        put32 (r, v);

    append (r, encodeDeviceSqlString (name));
    return r;
}

// playlist_entries (0x08): entry_index (from 1), track_id, playlist_id.
Bytes playlistEntryRow (std::uint32_t entryIndex, std::uint32_t trackId, std::uint32_t playlistId)
{
    Bytes r;

    for (auto v : { entryIndex, trackId, playlistId })
        put32 (r, v);

    return r;
}

// columns (0x10): id u16, code u16, name.
Bytes columnRow (std::uint16_t id, std::uint16_t code, const juce::String& name)
{
    Bytes r;
    put16 (r, id);
    put16 (r, code);
    append (r, encodeDeviceSqlString (name));
    return r;
}

// history (0x13): the single "property" row of an export: 0x0280,
// track_count (what a CDJ-3000 shows as Songs), display bytes, export date,
// 0x19 0x1e, "1000", device name, zero-padded to 40 bytes.
Bytes propertyRow (std::uint32_t trackCount, const juce::String& exportDate, const juce::String& deviceName)
{
    Bytes r;
    put32 (r, 0x0280);
    put32 (r, trackCount);
    put32 (r, 0);   // display: Device Library background colour, rekordbox's default
    append (r, encodeDeviceSqlString (exportDate));
    r.push_back (0x19);
    r.push_back (0x1e);
    append (r, encodeDeviceSqlString ("1000"));
    const auto name = encodeDeviceSqlString (deviceName);
    r.resize (stringAt (r.size(), name), 0);
    append (r, name);

    if (r.size() < 40)
        r.resize (40, 0);

    return r;
}

struct TrackRow
{
    std::uint32_t id = 0, sampleRate = 0, fileSize = 0, artistId = 0, bitrate = 0, tempo = 0;
    std::uint16_t sampleDepth = 0, durationSeconds = 0, fileType = 0;
    juce::String dateAdded, analyzePath, title, fileName, filePath;
};

// Track row constants rekordbox writes whose meaning is unknown (baken found
// the same values in rekordbox 7's OneLibrary rows).
constexpr std::uint32_t masterDbId = 3933607398u;
constexpr std::uint32_t trackBitmask = 0xc0700;
constexpr std::uint16_t analysedBits = 41;
constexpr std::uint16_t fileTypeMp3 = 1, fileTypeWav = 11;

Bytes encodeTrack (const TrackRow& t, size_t positionInPage)
{
    Bytes r;
    put16 (r, 0x24);
    put16 (r, (std::uint16_t) (positionInPage * 0x20));

    for (auto v : { trackBitmask, t.sampleRate, 0u /* composer */, t.fileSize,
                    t.id /* master content id: rekordbox's own id of the track */, masterDbId,
                    0u /* artwork */, 0u /* key */, 0u /* original artist */, 0u /* label */,
                    0u /* remixer */, t.bitrate, 0u /* track number */, t.tempo, 0u /* genre */,
                    0u /* album */, t.artistId, t.id })
        put32 (r, v);

    for (auto v : { (std::uint16_t) 0 /* disc */, (std::uint16_t) 0 /* play count */, (std::uint16_t) 0 /* year */,
                    t.sampleDepth, t.durationSeconds, analysedBits })
        put16 (r, v);

    r.push_back (0);    // colour
    r.push_back (0);    // rating
    put16 (r, t.fileType);
    put16 (r, 3);
    jassert (r.size() == 0x5e);

    // The 21 strings. The three small decimal strings look like update
    // counters in rekordbox's exports; "1" is a plausible fresh value (baken).
    const juce::String strings[21] = {
        {}, {}, "1", "1", "1", {}, "ON", "ON", {}, {},
        t.dateAdded,        // 10: date created (the XML has none, rekordbox's own exports put DateAdded here too)
        {},                 // 11: release date
        {},                 // 12: mix name
        {},
        t.analyzePath,      // 14
        t.dateAdded,        // 15: date added, what a player shows as DATE ADDED
        {},                 // 16: comment
        t.title,            // 17
        {},
        t.fileName,         // 19
        t.filePath          // 20
    };

    std::vector<Bytes> encoded;
    std::vector<size_t> offsets;
    size_t offset = 0x88;
    size_t allocated = 0x88 + 4;

    for (const auto& s : strings)
    {
        encoded.push_back (encodeDeviceSqlString (s));
        offset = stringAt (offset, encoded.back());
        put16 (r, (std::uint16_t) offset);
        offsets.push_back (offset);
        offset += encoded.back().size();
        allocated += align4 (encoded.back().size());
    }

    jassert (r.size() == 0x88);

    // rekordbox allocates 0x88 + Σ align4(string) + 4 per track row, which
    // always covers the padding before UTF-16 strings.
    for (size_t i = 0; i < encoded.size(); ++i)
    {
        r.resize (offsets[i], 0);
        append (r, encoded[i]);
    }

    r.resize (allocated, 0);
    return r;
}

//==============================================================================
// What rekordbox writes identically into every fresh export from its own
// preferences (baken pdb/fixed.rs, captured from an empty rekordbox 7.0.5 export).
struct Column { std::uint16_t id, code; const char* name; };

const Column columns[] = {
    { 1, 0x0080, "GENRE" }, { 2, 0x0081, "ARTIST" }, { 3, 0x0082, "ALBUM" }, { 4, 0x0083, "TRACK" },
    { 5, 0x0085, "BPM" }, { 6, 0x0086, "RATING" }, { 7, 0x0087, "YEAR" }, { 8, 0x0088, "REMIXER" },
    { 9, 0x0089, "LABEL" }, { 10, 0x008a, "ORIGINAL ARTIST" }, { 11, 0x008b, "KEY" }, { 12, 0x008d, "CUE" },
    { 13, 0x008e, "COLOR" }, { 14, 0x0092, "TIME" }, { 15, 0x0093, "BITRATE" }, { 16, 0x0094, "FILE NAME" },
    { 17, 0x0084, "PLAYLIST" }, { 18, 0x0098, "HOT CUE BANK" }, { 19, 0x0095, "HISTORY" },
    { 20, 0x0091, "SEARCH" }, { 21, 0x0096, "COMMENTS" }, { 22, 0x008c, "DATE ADDED" },
    { 23, 0x0097, "DJ PLAY COUNT" }, { 24, 0x0090, "FOLDER" }, { 25, 0x00a1, "DEFAULT" },
    { 26, 0x00a2, "ALPHABET" }, { 27, 0x00aa, "MATCHING" } };

const char* const colourNames[] = { "Pink", "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple" };

// Table 0x11: browse categories; table 0x12: sort list (rekordbox's defaults).
const std::uint8_t categoryRows[22][8] = {
    { 0x01, 0, 0x01, 0, 0x63, 0x01, 0, 0 }, { 0x05, 0, 0x06, 0, 0x05, 0x01, 0, 0 },
    { 0x06, 0, 0x07, 0, 0x63, 0x01, 0, 0 }, { 0x07, 0, 0x08, 0, 0x63, 0x01, 0, 0 },
    { 0x08, 0, 0x09, 0, 0x63, 0x01, 0, 0 }, { 0x09, 0, 0x0a, 0, 0x63, 0x01, 0, 0 },
    { 0x0a, 0, 0x0b, 0, 0x63, 0x01, 0, 0 }, { 0x0d, 0, 0x0f, 0, 0x63, 0x01, 0, 0 },
    { 0x0e, 0, 0x13, 0, 0x04, 0x01, 0, 0 }, { 0x0f, 0, 0x14, 0, 0x06, 0x01, 0, 0 },
    { 0x10, 0, 0x15, 0, 0x63, 0x01, 0, 0 }, { 0x12, 0, 0x17, 0, 0x63, 0x01, 0, 0 },
    { 0x02, 0, 0x02, 0, 0x02, 0x00, 0x01, 0 }, { 0x03, 0, 0x03, 0, 0x03, 0x00, 0x02, 0 },
    { 0x04, 0, 0x04, 0, 0x01, 0x00, 0x03, 0 }, { 0x0b, 0, 0x0c, 0, 0x63, 0x00, 0x04, 0 },
    { 0x11, 0, 0x05, 0, 0x63, 0x00, 0x05, 0 }, { 0x13, 0, 0x16, 0, 0x63, 0x00, 0x06, 0 },
    { 0x14, 0, 0x12, 0, 0x63, 0x00, 0x07, 0 }, { 0x1b, 0, 0x1a, 0, 0x63, 0x02, 0x08, 0 },
    { 0x18, 0, 0x11, 0, 0x63, 0x00, 0x09, 0 }, { 0x16, 0, 0x1b, 0, 0x63, 0x00, 0x0a, 0 } };

const std::uint8_t sortRows[17][8] = {
    { 0x01, 0, 0x06, 0, 0x01, 0, 0, 0 }, { 0x15, 0, 0x07, 0, 0x01, 0, 0, 0 },
    { 0x0e, 0, 0x08, 0, 0x01, 0, 0, 0 }, { 0x08, 0, 0x09, 0, 0x01, 0, 0, 0 },
    { 0x09, 0, 0x0a, 0, 0x01, 0, 0, 0 }, { 0x0a, 0, 0x0b, 0, 0x01, 0, 0, 0 },
    { 0x0f, 0, 0x0d, 0, 0x01, 0, 0, 0 }, { 0x0d, 0, 0x0f, 0, 0x01, 0, 0, 0 },
    { 0x17, 0, 0x10, 0, 0x01, 0, 0, 0 }, { 0x16, 0, 0x11, 0, 0x01, 0, 0, 0 },
    { 0x19, 0, 0x00, 0, 0x00, 0x01, 0, 0 }, { 0x1a, 0, 0x01, 0, 0x00, 0x02, 0, 0 },
    { 0x02, 0, 0x02, 0, 0x00, 0x03, 0, 0 }, { 0x03, 0, 0x03, 0, 0x00, 0x04, 0, 0 },
    { 0x05, 0, 0x04, 0, 0x00, 0x05, 0, 0 }, { 0x06, 0, 0x05, 0, 0x00, 0x06, 0, 0 },
    { 0x0b, 0, 0x0c, 0, 0x00, 0x07, 0, 0 } };

//==============================================================================
using RowMaker = std::function<Bytes (size_t positionInPage)>;

struct Table
{
    std::uint32_t type = 0;
    bool fixedStyle = false;
    std::optional<std::uint32_t> sequence;     // colors and columns keep rekordbox's fixed 2 and 3
    std::vector<RowMaker> rows;
};

// Layout of a fresh rekordbox export: page 0, then for every table its index
// page followed by its data pages, then one zero page per table as its
// empty candidate. Sequence numbers only have to be consistent: the header's
// is above every data page's.
bool assemble (const std::vector<Table>& tables, Bytes& out, juce::String& error)
{
    std::vector<Bytes> pages (1);   // page 0 is filled last
    std::vector<TablePointer> pointers;
    std::uint32_t nextSequence = 10, maxSequence = 3;

    struct Pending { std::uint32_t type; size_t indexPosition; std::vector<DataPage> data; };
    std::vector<Pending> pending;

    for (const auto& t : tables)
    {
        const auto indexPosition = pages.size();
        pages.emplace_back();
        std::vector<DataPage> data;

        const auto newPage = [&]
        {
            DataPage p;
            p.index = (std::uint32_t) (pages.size() + data.size());
            p.table = t.type;
            p.fixedStyle = t.fixedStyle;
            return p;
        };

        auto page = newPage();

        for (size_t i = 0; i < t.rows.size(); ++i)
        {
            const auto row = t.rows[i] (page.rows.size());

            if (row.size() > maxRow)
            {
                error = "Row " + juce::String ((int) i + 1) + " of database table " + juce::String ((int) t.type)
                      + " is too long (" + juce::String ((int) row.size()) + " bytes).";
                return false;
            }

            if (! page.tryPush (row))
            {
                data.push_back (page);
                page = newPage();
                const auto fitted = page.tryPush (t.rows[i] (0));
                jassertquiet (fitted);
            }
        }

        if (! page.rows.empty())
            data.push_back (page);

        for (auto& p : data)
        {
            p.sequence = t.sequence.has_value() ? *t.sequence : ++nextSequence;
            maxSequence = std::max (maxSequence, p.sequence);
            pages.emplace_back();
        }

        pending.push_back ({ t.type, indexPosition, std::move (data) });
    }

    const auto firstEmpty = (std::uint32_t) pages.size();

    for (size_t i = 0; i < pending.size(); ++i)
    {
        auto& [type, indexPosition, data] = pending[i];
        const auto empty = firstEmpty + (std::uint32_t) i;
        const auto firstData = data.empty() ? std::optional<std::uint32_t>() : data.front().index;
        const auto last = data.empty() ? (std::uint32_t) indexPosition : data.back().index;
        pointers.push_back ({ type, empty, (std::uint32_t) indexPosition, last });
        pages[indexPosition] = indexPage ((std::uint32_t) indexPosition, type, firstData.value_or (empty), firstData);

        for (size_t k = 0; k < data.size(); ++k)
        {
            data[k].next = k + 1 < data.size() ? data[k].index + 1 : empty;
            pages[data[k].index] = data[k].toBytes();
        }
    }

    for (size_t i = 0; i < tables.size(); ++i)
        pages.emplace_back (pdbPageSize, 0);

    pages[0] = headerPage ((std::uint32_t) tables.size(), (std::uint32_t) pages.size(), maxSequence + 1, pointers);

    out.clear();
    out.reserve (pages.size() * pdbPageSize);

    for (const auto& p : pages)
        append (out, p);

    return true;
}
} // namespace

Bytes encodeDeviceSqlString (const juce::String& text)
{
    // As rekordbox 7 writes them (measured by baken): every ASCII string
    // shorter than 127 bytes in the short form, everything else UTF-16LE.
    // The long ASCII form (0x40) never appears in rekordbox's exports.
    const auto ascii = text.getNumBytesAsUTF8() == (size_t) text.length();
    Bytes out;

    if (ascii && text.length() < 0x7f)
    {
        out.push_back ((std::uint8_t) (((text.length() + 1) << 1) | 1));
        const auto* raw = text.toRawUTF8();
        out.insert (out.end(), raw, raw + text.length());
        return out;
    }

    const auto units = utf16 (text);
    out.push_back (0x90);
    put16 (out, (std::uint16_t) (units.size() * 2 + 4));
    out.push_back (0);

    for (auto u : units)
        put16 (out, u);

    return out;
}

bool buildExportPdb (const StickLibrary& library, const PdbOptions& options, Bytes& out, juce::String& error)
{
    // Every track, by id.
    std::vector<const StickTrack*> tracks;

    for (const auto& t : library.tracks)
        tracks.push_back (&t);

    std::sort (tracks.begin(), tracks.end(), [] (auto* a, auto* b) { return a->id < b->id; });

    // Artists: one row per distinct name, ids in order of first use.
    std::vector<std::pair<std::uint32_t, juce::String>> artists;
    std::vector<TrackRow> trackRows;

    for (const auto* t : tracks)
    {
        std::uint32_t artistId = 0;
        const auto artist = t->artist.trim();

        if (artist.isNotEmpty())
        {
            for (const auto& [id, name] : artists)
                if (name == artist)
                    artistId = id;

            if (artistId == 0)
            {
                artistId = (std::uint32_t) artists.size() + 1;
                artists.emplace_back (artistId, artist);
            }
        }

        TrackRow row;
        row.id = (std::uint32_t) t->id;
        row.sampleRate = (std::uint32_t) t->sampleRate;
        row.fileSize = (std::uint32_t) juce::jlimit ((juce::int64) 0, (juce::int64) 0xffffffff, t->bytes);
        row.artistId = artistId;
        row.bitrate = (std::uint32_t) t->bitrateKbps;
        row.tempo = (std::uint32_t) std::llround (t->bpm * 100.0);
        row.sampleDepth = (std::uint16_t) (t->fileType == "mp3" ? 16 : t->bitsPerSample);
        row.durationSeconds = (std::uint16_t) std::floor (t->seconds);
        row.fileType = t->fileType == "mp3" ? fileTypeMp3 : fileTypeWav;
        row.dateAdded = options.exportDate;
        const auto found = options.analyzePaths.find (t->id);
        row.analyzePath = found != options.analyzePaths.end() ? found->second : juce::String();
        row.title = t->title;
        row.filePath = "/" + t->path;
        row.fileName = t->path.fromLastOccurrenceOf ("/", false, false);
        trackRows.push_back (row);
    }

    std::vector<Table> tables (numTables);

    for (std::uint32_t i = 0; i < numTables; ++i)
        tables[i].type = i;

    for (const auto& row : trackRows)
        tables[0].rows.push_back ([row] (size_t position) { return encodeTrack (row, position); });

    for (const auto& [id, name] : artists)
        tables[2].rows.push_back ([id, name] (size_t position) { return artistRow (position, id, name); });

    tables[6].fixedStyle = true;
    tables[6].sequence = 2;

    for (std::uint8_t i = 0; i < 8; ++i)
        tables[6].rows.push_back ([i] (size_t) { return colorRow ((std::uint8_t) (i + 1), colourNames[i]); });

    // Playlists, all at the top level, in the library's order.
    for (size_t p = 0; p < library.playlists.size(); ++p)
    {
        const auto& playlist = library.playlists[p];
        const auto id = (std::uint32_t) p + 1;
        tables[7].rows.push_back ([id, p, name = playlist.name] (size_t)
                                  { return playlistNodeRow (0, (std::uint32_t) p, id, false, name); });

        std::uint32_t entry = 0;

        for (auto trackId : playlist.trackIds)
            if (library.find (trackId) != nullptr)
            {
                const auto index = ++entry;
                tables[8].rows.push_back ([index, trackId, id] (size_t)
                                          { return playlistEntryRow (index, (std::uint32_t) trackId, id); });
            }
    }

    tables[16].fixedStyle = true;
    tables[16].sequence = 3;

    for (const auto& c : columns)
        tables[16].rows.push_back ([c] (size_t)
        {
            // Names are wrapped in U+FFFA / U+FFFB as rekordbox stores them.
            return columnRow (c.id, c.code, juce::String::charToString (0xfffa) + c.name + juce::String::charToString (0xfffb));
        });

    for (const auto& row : categoryRows)
        tables[17].rows.push_back ([&row] (size_t) { return Bytes (std::begin (row), std::end (row)); });

    for (const auto& row : sortRows)
        tables[18].rows.push_back ([&row] (size_t) { return Bytes (std::begin (row), std::end (row)); });

    const auto trackCount = (std::uint32_t) trackRows.size();
    tables[19].rows.push_back ([trackCount, &options] (size_t)
                               { return propertyRow (trackCount, options.exportDate, options.deviceName); });

    return assemble (tables, out, error);
}
} // namespace cdj::pioneer
