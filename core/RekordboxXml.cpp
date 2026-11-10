#include "RekordboxXml.h"

namespace cdj
{
juce::String rekordboxLocation (const juce::File& file)
{
    auto path = file.getFullPathName().replaceCharacter ('\\', '/');

    if (! path.startsWith ("/"))
        path = "/" + path;      // E:/x → /E:/x

    // Percent-encode each byte that isn't plain ASCII or a path character.
    juce::String encoded;
    const auto* utf8 = path.toRawUTF8();

    for (auto* p = utf8; *p != 0; ++p)
    {
        const auto c = (unsigned char) *p;

        if (juce::CharacterFunctions::isLetterOrDigit ((char) c) || juce::String ("/-_.~:").containsChar ((juce::juce_wchar) c))
            encoded += juce::String::charToString ((juce::juce_wchar) c);
        else
            encoded += "%" + juce::String::toHexString ((int) c).paddedLeft ('0', 2).toUpperCase();
    }

    return "file://localhost" + encoded;
}

juce::String rekordboxXml (const StickLibrary& library, const juce::File& stickRoot, const juce::String& version)
{
    juce::XmlElement root ("DJ_PLAYLISTS");
    root.setAttribute ("Version", "1.0.0");

    auto* product = root.createNewChildElement ("PRODUCT");
    product->setAttribute ("Name", "Adormis CDJ Export");
    product->setAttribute ("Version", version);
    product->setAttribute ("Company", "Adormis");

    auto* collection = root.createNewChildElement ("COLLECTION");
    collection->setAttribute ("Entries", (int) library.tracks.size());

    for (const auto& t : library.tracks)
    {
        auto* e = collection->createNewChildElement ("TRACK");
        e->setAttribute ("TrackID", t.id);
        e->setAttribute ("Name", t.title);
        e->setAttribute ("Artist", t.artist);
        e->setAttribute ("Kind", t.fileType == "mp3" ? "MP3 File" : "WAV File");
        e->setAttribute ("Size", juce::String (t.bytes));
        e->setAttribute ("TotalTime", (int) std::floor (t.seconds));
        e->setAttribute ("SampleRate", t.sampleRate);
        e->setAttribute ("BitRate", t.bitrateKbps);
        e->setAttribute ("AverageBpm", juce::String (t.bpm, 2));
        e->setAttribute ("Location", rekordboxLocation (stickRoot.getChildFile (t.path)));

        // Beat grid: one tempo from the first downbeat ("Battito" 1 = beat 1 of the bar).
        if (t.bpm > 0.0)
        {
            auto* tempo = e->createNewChildElement ("TEMPO");
            tempo->setAttribute ("Inizio", juce::String (t.firstBeatMs / 1000.0, 3));
            tempo->setAttribute ("Bpm", juce::String (t.bpm, 2));
            tempo->setAttribute ("Metro", "4/4");
            tempo->setAttribute ("Battito", 1);
        }

        for (const auto& c : t.cues)
        {
            auto* m = e->createNewChildElement ("POSITION_MARK");
            m->setAttribute ("Name", c.name);
            m->setAttribute ("Type", c.isLoop() ? 4 : 0);
            m->setAttribute ("Start", juce::String ((double) c.startMs / 1000.0, 3));

            if (c.isLoop())
                m->setAttribute ("End", juce::String ((double) c.endMs / 1000.0, 3));

            m->setAttribute ("Num", c.kind == Cue::Kind::hot ? c.hotIndex : -1);

            if (c.kind == Cue::Kind::hot && c.colour != 0)
            {
                const auto rgb = hotCueRgb (c.hotIndex);
                m->setAttribute ("Red", (int) rgb.r);
                m->setAttribute ("Green", (int) rgb.g);
                m->setAttribute ("Blue", (int) rgb.b);
            }
        }
    }

    auto* playlists = root.createNewChildElement ("PLAYLISTS");
    auto* top = playlists->createNewChildElement ("NODE");
    top->setAttribute ("Type", 0);
    top->setAttribute ("Name", "ROOT");
    top->setAttribute ("Count", (int) library.playlists.size());

    for (const auto& p : library.playlists)
    {
        auto* node = top->createNewChildElement ("NODE");
        node->setAttribute ("Name", p.name);
        node->setAttribute ("Type", 1);
        node->setAttribute ("KeyType", 0);
        node->setAttribute ("Entries", (int) p.trackIds.size());

        for (auto id : p.trackIds)
            node->createNewChildElement ("TRACK")->setAttribute ("Key", id);
    }

    return root.toString();   // UTF-8 is the default encoding
}
} // namespace cdj
