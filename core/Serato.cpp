#include "Serato.h"

#include <algorithm>

namespace cdj
{
namespace
{
// ---------------------------------------------------------------- bytes

struct Bytes
{
    juce::MemoryOutputStream out;

    void u8 (int v) { out.writeByte ((char) (juce::uint8) v); }
    void u32be (juce::uint32 v) { out.writeIntBigEndian ((int) v); }
    void f32be (float v) { out.writeFloatBigEndian (v); }
    void raw (const void* d, size_t n) { out.write (d, n); }
    void raw (const juce::MemoryBlock& b) { out.write (b.getData(), b.getSize()); }
    void ascii (const char* s, bool terminate) { out.write (s, strlen (s)); if (terminate) u8 (0); }

    void utf8 (const juce::String& s, bool terminate)
    {
        out.write (s.toRawUTF8(), s.getNumBytesAsUTF8());
        if (terminate) u8 (0);
    }

    void rgb (juce::uint32 c) { u8 ((int) (c >> 16) & 0xff); u8 ((int) (c >> 8) & 0xff); u8 ((int) c & 0xff); }

    juce::MemoryBlock block() { return out.getMemoryBlock(); }
};

juce::uint32 readBe32 (const juce::uint8* p) { return ((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16) | ((juce::uint32) p[2] << 8) | p[3]; }
juce::uint32 readLe32 (const juce::uint8* p) { return ((juce::uint32) p[3] << 24) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[1] << 8) | p[0]; }
juce::uint32 readSyncsafe (const juce::uint8* p) { return ((juce::uint32) (p[0] & 0x7f) << 21) | ((juce::uint32) (p[1] & 0x7f) << 14) | ((juce::uint32) (p[2] & 0x7f) << 7) | (p[3] & 0x7f); }

void writeSyncsafe (Bytes& b, juce::uint32 v)
{
    b.u8 ((int) (v >> 21) & 0x7f);
    b.u8 ((int) (v >> 14) & 0x7f);
    b.u8 ((int) (v >> 7) & 0x7f);
    b.u8 ((int) v & 0x7f);
}

juce::String bpmText (double bpm)
{
    auto s = juce::String (bpm, 2);

    while (s.contains (".") && (s.endsWithChar ('0') || s.endsWithChar ('.')))
        s = s.dropLastCharacters (1);

    return s;
}

// ---------------------------------------------------------------- ID3

struct Frame
{
    juce::String id;
    juce::MemoryBlock body;
    int flags = 0;
};

struct Id3
{
    int major = 4;
    std::vector<Frame> frames;
};

// Index just past a string terminator starting at `pos` (1 byte for
// ISO-8859-1 / UTF-8, an aligned 2-byte 0 for UTF-16).
size_t skipString (const juce::uint8* d, size_t pos, size_t size, int encoding)
{
    if (encoding == 1 || encoding == 2)
    {
        while (pos + 1 < size && ! (d[pos] == 0 && d[pos + 1] == 0))
            pos += 2;
        return juce::jmin (size, pos + 2);
    }

    while (pos < size && d[pos] != 0)
        ++pos;
    return juce::jmin (size, pos + 1);
}

juce::String geobDescription (const juce::MemoryBlock& body)
{
    const auto* d = (const juce::uint8*) body.getData();
    const auto size = body.getSize();

    if (size < 4)
        return {};

    const int encoding = d[0];
    auto pos = skipString (d, 1, size, 0);          // MIME type: always ISO-8859-1
    pos = skipString (d, pos, size, encoding);      // file name
    const auto start = pos;
    const auto end = skipString (d, pos, size, encoding);

    if (encoding == 1 || encoding == 2)
        return {};      // Serato writes ISO-8859-1; a UTF-16 one isn't Serato's

    return juce::String::fromUTF8 ((const char*) d + start, (int) juce::jmax ((size_t) 0, end - start - 1));
}

// Parses an ID3v2.3 / 2.4 tag. Anything else (v2.2, unsynchronised,
// extended header) gives no frames: the tag is then replaced as a whole.
Id3 parseId3 (const juce::MemoryBlock& tag)
{
    Id3 r;
    const auto* d = (const juce::uint8*) tag.getData();
    const auto size = tag.getSize();

    if (size < 10 || memcmp (d, "ID3", 3) != 0)
        return r;

    const int major = d[3], flags = d[5];

    if ((major != 3 && major != 4) || (flags & 0xc0) != 0)
        return r;

    r.major = major;
    const auto end = juce::jmin (size, (size_t) readSyncsafe (d + 6) + 10);

    for (size_t pos = 10; pos + 10 <= end;)
    {
        if (d[pos] == 0)
            break;      // padding

        const auto id = juce::String ((const char*) d + pos, 4);
        const auto frameSize = major == 4 ? readSyncsafe (d + pos + 4) : readBe32 (d + pos + 4);
        const auto bodyStart = pos + 10;

        if (frameSize == 0 || bodyStart + frameSize > end)
            break;

        Frame f;
        f.id = id;
        f.flags = (d[pos + 8] << 8) | d[pos + 9];
        f.body = juce::MemoryBlock (d + bodyStart, frameSize);
        r.frames.push_back (std::move (f));
        pos = bodyStart + frameSize;
    }

    return r;
}

Frame geob (const juce::String& description, const juce::MemoryBlock& data)
{
    Bytes b;
    b.u8 (0);                                   // ISO-8859-1
    b.ascii ("application/octet-stream", true);
    b.u8 (0);                                   // no file name
    b.utf8 (description, true);
    b.raw (data);
    return { "GEOB", b.block(), 0 };
}

Frame textFrame (const juce::String& id, const juce::String& text, int major)
{
    Bytes b;

    if (text.containsOnly ("0123456789."))
    {
        b.u8 (0);
        b.utf8 (text, false);
    }
    else if (major == 4)
    {
        b.u8 (3);                               // UTF-8
        b.utf8 (text, false);
    }
    else
    {
        b.u8 (1);                               // UTF-16 with BOM
        b.u8 (0xff);
        b.u8 (0xfe);

        for (auto* p = text.toUTF16().getAddress(); *p != 0; ++p)
        {
            b.u8 (*p & 0xff);
            b.u8 (*p >> 8);
        }
    }

    return { id, b.block(), 0 };
}

juce::MemoryBlock buildId3 (const Id3& tag)
{
    Bytes frames;

    for (const auto& f : tag.frames)
    {
        frames.ascii (f.id.toRawUTF8(), false);

        if (tag.major == 4)
            writeSyncsafe (frames, (juce::uint32) f.body.getSize());
        else
            frames.u32be ((juce::uint32) f.body.getSize());

        frames.u8 ((f.flags >> 8) & 0xff);
        frames.u8 (f.flags & 0xff);
        frames.raw (f.body);
    }

    auto body = frames.block();
    Bytes b;
    b.ascii ("ID3", false);
    b.u8 (tag.major);
    b.u8 (0);
    b.u8 (0);
    writeSyncsafe (b, (juce::uint32) body.getSize());
    b.raw (body);
    return b.block();
}

// ---------------------------------------------------------------- files

bool copyRange (juce::FileInputStream& in, juce::int64 from, juce::int64 length, juce::OutputStream& out)
{
    if (! in.setPosition (from))
        return false;

    return out.writeFromInputStream (in, length) == length;
}

struct Chunk
{
    juce::String id;
    juce::int64 start = 0, size = 0;    // start = the 8-byte header
};

// Rewrites `file` as `part`: an MP3 with the new tag in front, or a WAV
// with the new tag as its "id3 " chunk. Also returns the old tag.
bool rewrite (const juce::File& file, const juce::File& part, const std::function<juce::MemoryBlock (const juce::MemoryBlock&)>& makeTag,
              juce::String& error)
{
    juce::FileInputStream in (file);

    if (in.failedToOpen())
    {
        error = "Can't read \"" + file.getFileName() + "\" on the stick.";
        return false;
    }

    const auto total = in.getTotalLength();
    part.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out (part.createOutputStream());

    if (out == nullptr || out->failedToOpen())
    {
        error = "Can't write on the stick (write-protected?).";
        return false;
    }

    juce::uint8 head[12] = {};
    in.read (head, 12);
    const auto failed = [&] (const juce::String& e) { out.reset(); part.deleteFile(); error = e; return false; };

    if (file.hasFileExtension ("wav"))
    {
        if (memcmp (head, "RIFF", 4) != 0 || memcmp (head + 8, "WAVE", 4) != 0)
            return failed ("\"" + file.getFileName() + "\" is not a plain WAV file.");

        std::vector<Chunk> chunks;
        juce::MemoryBlock oldTag;

        for (juce::int64 pos = 12; pos + 8 <= total;)
        {
            juce::uint8 h[8];
            in.setPosition (pos);
            in.read (h, 8);
            Chunk c;
            c.id = juce::String ((const char*) h, 4);
            c.start = pos;
            c.size = juce::jmin ((juce::int64) readLe32 (h + 4), total - pos - 8);

            if (c.id.equalsIgnoreCase ("id3 "))
            {
                oldTag.setSize ((size_t) c.size);
                in.read (oldTag.getData(), (int) c.size);
            }
            else
                chunks.push_back (c);

            pos += 8 + c.size + (c.size & 1);
        }

        const auto tag = makeTag (oldTag);
        out->write ("RIFF\0\0\0\0WAVE", 12);

        for (const auto& c : chunks)
        {
            if (! copyRange (in, c.start, 8 + c.size, *out))
                return failed ("Copying \"" + file.getFileName() + "\" failed (stick full?).");

            if ((c.size & 1) != 0)
                out->writeByte (0);
        }

        out->write ("id3 ", 4);
        out->writeInt ((int) tag.getSize());
        out->write (tag.getData(), tag.getSize());

        if ((tag.getSize() & 1) != 0)
            out->writeByte (0);

        const auto riffSize = out->getPosition() - 8;

        if (riffSize > 0xffffffffLL)
            return failed ("\"" + file.getFileName() + "\" is too big for a WAV file.");

        out->setPosition (4);
        out->writeInt ((int) (juce::uint32) riffSize);
    }
    else
    {
        juce::int64 audioStart = 0;
        juce::MemoryBlock oldTag;

        if (memcmp (head, "ID3", 3) == 0)
        {
            audioStart = 10 + readSyncsafe (head + 6) + ((head[5] & 0x10) != 0 ? 10 : 0);
            audioStart = juce::jmin (audioStart, total);
            oldTag.setSize ((size_t) audioStart);
            in.setPosition (0);
            in.read (oldTag.getData(), (int) audioStart);
        }

        const auto tag = makeTag (oldTag);
        out->write (tag.getData(), tag.getSize());

        if (! copyRange (in, audioStart, total - audioStart, *out))
            return failed ("Copying \"" + file.getFileName() + "\" failed (stick full?).");
    }

    out->flush();

    if (out->getStatus().failed())
        return failed ("Writing \"" + file.getFileName() + "\" failed (stick full?).");

    out.reset();
    return true;
}

// ---------------------------------------------------------------- _Serato_ files

void field (Bytes& b, const char* tag, const juce::MemoryBlock& data)
{
    b.ascii (tag, false);
    b.u32be ((juce::uint32) data.getSize());
    b.raw (data);
}

juce::MemoryBlock utf16be (const juce::String& s)
{
    Bytes b;

    for (auto* p = s.toUTF16().getAddress(); *p != 0; ++p)
    {
        b.u8 (*p >> 8);
        b.u8 (*p & 0xff);
    }

    return b.block();
}

void textField (Bytes& b, const char* tag, const juce::String& s) { field (b, tag, utf16be (s)); }

juce::MemoryBlock u32Block (juce::uint32 v)
{
    Bytes b;
    b.u32be (v);
    return b.block();
}

juce::MemoryBlock byteBlock (int v)
{
    Bytes b;
    b.u8 (v);
    return b.block();
}

juce::String crateFileName (const StickPlaylist& p) { return fatSafeName (p.name.replace ("%%", "%"), 80) + ".crate"; }
} // namespace

juce::uint32 seratoHotCueRgb (int index)
{
    static const juce::uint32 colours[] = { 0xcc0000, 0xcc8800, 0x0000cc, 0xcccc00, 0x00cc00, 0xcc00cc, 0x00cccc, 0x8800cc };
    return colours[juce::jlimit (0, 7, index)];
}

juce::MemoryBlock seratoMarkers2 (const StickTrack& track)
{
    Bytes p;
    p.u8 (1);
    p.u8 (1);

    const auto entry = [&] (const char* name, const juce::MemoryBlock& data)
    {
        p.ascii (name, true);
        p.u32be ((juce::uint32) data.getSize());
        p.raw (data);
    };

    {
        Bytes c;
        c.u8 (0);
        c.rgb (0xffffff);       // track colour: none (white)
        entry ("COLOR", c.block());
    }

    // Hot cues: pads 1–8 = A–H. A hot loop keeps its pad as a cue at its start.
    std::vector<const Cue*> loops;

    for (const auto& cue : track.cues)
    {
        if (cue.isLoop())
            loops.push_back (&cue);

        if (cue.kind != Cue::Kind::hot || cue.hotIndex < 0 || cue.hotIndex > 7)
            continue;

        Bytes c;
        c.u8 (0);
        c.u8 (cue.hotIndex);
        c.u32be ((juce::uint32) juce::jmax ((juce::int64) 0, cue.startMs));
        c.u8 (0);
        c.rgb (seratoHotCueRgb (cue.hotIndex));
        c.u8 (0);
        c.u8 (0);
        c.utf8 (cue.name.substring (0, 32), true);
        entry ("CUE", c.block());
    }

    // Saved loops: every loop once, in time order, up to 8. A loop that is
    // both a memory and a hot cue on the CDJ is in the list twice.
    std::stable_sort (loops.begin(), loops.end(), [] (auto* a, auto* b) { return a->startMs < b->startMs; });
    loops.erase (std::unique (loops.begin(), loops.end(), [] (auto* a, auto* b) { return a->startMs == b->startMs && a->endMs == b->endMs; }),
                 loops.end());

    for (size_t i = 0; i < loops.size() && i < 8; ++i)
    {
        Bytes c;
        c.u8 (0);
        c.u8 ((int) i);
        c.u32be ((juce::uint32) juce::jmax ((juce::int64) 0, loops[i]->startMs));
        c.u32be ((juce::uint32) juce::jmax ((juce::int64) 0, loops[i]->endMs));
        c.u32be (0xffffffff);
        c.u8 (0);
        c.rgb (0x27aae1);
        c.u8 (0);
        c.u8 (0);               // not locked
        c.utf8 (loops[i]->name, true);
        entry ("LOOP", c.block());
    }

    entry ("BPMLOCK", byteBlock (track.bpm > 0.0 ? 1 : 0));
    p.u8 (0);

    // Base64 with '=' written as 'A' and a line feed every 72 characters,
    // after a 01 01 header; at least 470 bytes, filled with zeros.
    auto base64 = juce::Base64::toBase64 (p.block().getData(), p.block().getSize()).replaceCharacter ('=', 'A');
    juce::String lines;

    for (int i = 0; i < base64.length(); i += 72)
        lines << (i > 0 ? "\n" : "") << base64.substring (i, i + 72);

    Bytes tag;
    tag.u8 (1);
    tag.u8 (1);
    tag.ascii (lines.toRawUTF8(), false);
    tag.u8 (0);

    auto block = tag.block();

    if (block.getSize() < 470)
        block.setSize (470, true);

    return block;
}

juce::MemoryBlock seratoBeatGrid (const StickTrack& track)
{
    if (track.bpm <= 0.0)
        return {};

    // One constant tempo: a single terminal marker at the first downbeat.
    Bytes b;
    b.u8 (1);
    b.u8 (0);
    b.u32be (1);
    b.f32be ((float) (track.firstBeatMs / 1000.0));
    b.f32be ((float) track.bpm);
    b.u8 (0);
    return b.block();
}

juce::MemoryBlock seratoId3Tag (const StickTrack& track, const juce::MemoryBlock& oldTag)
{
    const auto old = parseId3 (oldTag);
    Id3 tag;
    tag.major = old.major;
    bool hasTitle = false, hasArtist = false;

    for (const auto& f : old.frames)
    {
        if (f.id == "TBPM" || (f.id == "GEOB" && geobDescription (f.body).startsWith ("Serato ")))
            continue;

        hasTitle = hasTitle || f.id == "TIT2";
        hasArtist = hasArtist || f.id == "TPE1";
        tag.frames.push_back (f);
    }

    if (! hasTitle && track.title.isNotEmpty())
        tag.frames.push_back (textFrame ("TIT2", track.title, tag.major));

    if (! hasArtist && track.artist.isNotEmpty())
        tag.frames.push_back (textFrame ("TPE1", track.artist, tag.major));

    if (track.bpm > 0.0)
        tag.frames.push_back (textFrame ("TBPM", bpmText (track.bpm), tag.major));

    tag.frames.push_back (geob ("Serato Markers2", seratoMarkers2 (track)));

    const auto grid = seratoBeatGrid (track);

    if (grid.getSize() > 0)
        tag.frames.push_back (geob ("Serato BeatGrid", grid));

    return buildId3 (tag);
}

bool writeSeratoTags (const juce::File& audioFile, const StickTrack& track, juce::String& error)
{
    const auto part = audioFile.getSiblingFile (audioFile.getFileName() + ".part");

    if (! rewrite (audioFile, part, [&] (const juce::MemoryBlock& old) { return seratoId3Tag (track, old); }, error))
        return false;

    if (! part.moveFileTo (audioFile))
    {
        part.deleteFile();
        error = "Can't rename \"" + part.getFileName() + "\" on the stick.";
        return false;
    }

    return true;
}

juce::MemoryBlock readId3Tag (const juce::File& file)
{
    juce::FileInputStream in (file);

    if (in.failedToOpen())
        return {};

    juce::uint8 head[12] = {};
    in.read (head, 12);

    if (memcmp (head, "ID3", 3) == 0)
    {
        const auto size = (int) (10 + readSyncsafe (head + 6));
        juce::MemoryBlock tag ((size_t) size);
        in.setPosition (0);
        in.read (tag.getData(), size);
        return tag;
    }

    if (memcmp (head, "RIFF", 4) == 0)
        for (juce::int64 pos = 12; pos + 8 <= in.getTotalLength();)
        {
            juce::uint8 h[8];
            in.setPosition (pos);
            in.read (h, 8);
            const auto size = (juce::int64) readLe32 (h + 4);

            if (memcmp (h, "id3 ", 4) == 0 || memcmp (h, "ID3 ", 4) == 0)
            {
                juce::MemoryBlock tag ((size_t) size);
                in.read (tag.getData(), (int) size);
                return tag;
            }

            pos += 8 + size + (size & 1);
        }

    return {};
}

juce::MemoryBlock findGeob (const juce::MemoryBlock& tagData, const juce::String& description)
{
    for (const auto& f : parseId3 (tagData).frames)
        if (f.id == "GEOB" && geobDescription (f.body) == description)
        {
            // Skip encoding, MIME, file name and description.
            const auto* d = (const juce::uint8*) f.body.getData();
            auto pos = skipString (d, 1, f.body.getSize(), 0);
            pos = skipString (d, pos, f.body.getSize(), d[0]);
            pos = skipString (d, pos, f.body.getSize(), d[0]);
            return juce::MemoryBlock (d + pos, f.body.getSize() - pos);
        }

    return {};
}

juce::MemoryBlock seratoCrate (const StickLibrary& library, const StickPlaylist& playlist)
{
    Bytes b;
    textField (b, "vrsn", "1.0/Serato ScratchLive Crate");

    {
        Bytes sort;
        textField (sort, "tvcn", "song");
        field (sort, "brev", byteBlock (0));
        field (b, "osrt", sort.block());
    }

    for (const auto* column : { "song", "artist", "bpm", "length" })
    {
        Bytes c;
        textField (c, "tvcn", column);
        textField (c, "tvcw", "0");
        field (b, "ovct", c.block());
    }

    for (auto id : playlist.trackIds)
        if (const auto* t = library.find (id))
        {
            Bytes track;
            textField (track, "ptrk", t->path);
            field (b, "otrk", track.block());
        }

    return b.block();
}

juce::MemoryBlock seratoDatabase (const StickLibrary& library)
{
    Bytes b;
    textField (b, "vrsn", "2.0/Serato Scratch LIVE Database");
    const auto now = (juce::uint32) (juce::Time::currentTimeMillis() / 1000);

    for (const auto& t : library.tracks)
    {
        Bytes o;
        textField (o, "ttyp", t.fileType);
        textField (o, "pfil", t.path);
        textField (o, "tsng", t.title);

        if (t.artist.isNotEmpty())
            textField (o, "tart", t.artist);

        if (t.bpm > 0.0)
            textField (o, "tbpm", juce::String (t.bpm, 2));

        textField (o, "tadd", juce::String (now));
        field (o, "uadd", u32Block (now));
        field (o, "bbgl", byteBlock (t.bpm > 0.0 ? 1 : 0));
        field (b, "otrk", o.block());
    }

    return b.block();
}

juce::File seratoFolder (const juce::File& root) { return root.getChildFile ("_Serato_"); }
juce::File seratoMarkerFile (const juce::File& root) { return seratoFolder (root).getChildFile ("adormis-cdj-export.txt"); }

bool hasForeignSeratoLibrary (const juce::File& root)
{
    return seratoFolder (root).isDirectory() && ! seratoMarkerFile (root).existsAsFile();
}

bool writeSeratoLibrary (StickLibrary& library, const juce::File& root, juce::String& error)
{
    if (hasForeignSeratoLibrary (root))
    {
        error = "the stick already has a Serato library (a _Serato_ folder this program didn't write).";
        return false;
    }

    for (auto& t : library.tracks)
    {
        const auto file = root.getChildFile (t.path);

        if (! file.existsAsFile())
            continue;

        if (! writeSeratoTags (file, t, error))
            return false;

        t.bytes = file.getSize();
    }

    const auto folder = seratoFolder (root);
    const auto subcrates = folder.getChildFile ("Subcrates");

    if (! subcrates.createDirectory())
    {
        error = "can't create the _Serato_ folder (write-protected?).";
        return false;
    }

    // Crates this program wrote last time and no longer has go first.
    juce::StringArray written;

    for (const auto& line : juce::StringArray::fromLines (seratoMarkerFile (root).loadFileAsString()))
        if (line.startsWith ("crate: "))
            subcrates.getChildFile (line.fromFirstOccurrenceOf ("crate: ", false, false)).deleteFile();

    for (const auto& p : library.playlists)
    {
        const auto name = crateFileName (p);

        if (! replaceFileSafely (subcrates.getChildFile (name), seratoCrate (library, p), error))
            return false;

        written.add ("crate: " + name);
    }

    if (! replaceFileSafely (folder.getChildFile ("database V2"), seratoDatabase (library), error))
        return false;

    juce::String marker;
    marker << "Written by " << library.writtenBy << " at " << library.writtenAt << ".\n"
           << "Delete the _Serato_ folder to let Serato start its own library on this stick.\n";

    for (const auto& w : written)
        marker << w << "\n";

    return replaceFileSafely (seratoMarkerFile (root), juce::MemoryBlock (marker.toRawUTF8(), marker.getNumBytesAsUTF8()), error);
}
} // namespace cdj
