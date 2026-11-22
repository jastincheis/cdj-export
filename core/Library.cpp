#include "Library.h"

namespace cdj
{
const StickTrack* StickLibrary::find (int id) const
{
    for (const auto& t : tracks)
        if (t.id == id)
            return &t;

    return nullptr;
}

int StickLibrary::nextId() const
{
    int id = 0;

    for (const auto& t : tracks)
        id = juce::jmax (id, t.id);

    return id + 1;
}

namespace
{
juce::var cueToJson (const Cue& c)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("kind", c.kind == Cue::Kind::hot ? "hot" : "memory");
    o->setProperty ("hot", c.hotIndex);
    o->setProperty ("startMs", c.startMs);
    o->setProperty ("endMs", c.endMs);
    o->setProperty ("name", c.name);
    o->setProperty ("colour", c.colour);
    return juce::var (o);
}

Cue cueFromJson (const juce::var& v)
{
    Cue c;
    c.kind = v["kind"].toString() == "hot" ? Cue::Kind::hot : Cue::Kind::memory;
    c.hotIndex = (int) v["hot"];
    c.startMs = (juce::int64) v["startMs"];
    c.endMs = (juce::int64) v["endMs"];
    c.name = v["name"].toString();
    c.colour = (int) v["colour"];
    return c;
}
} // namespace

juce::var StickLibrary::toJson() const
{
    juce::Array<juce::var> trackList;

    for (const auto& t : tracks)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", t.id);
        o->setProperty ("path", t.path);
        o->setProperty ("title", t.title);
        o->setProperty ("artist", t.artist);
        o->setProperty ("fileType", t.fileType);
        o->setProperty ("seconds", t.seconds);
        o->setProperty ("sampleRate", t.sampleRate);
        o->setProperty ("bitsPerSample", t.bitsPerSample);
        o->setProperty ("channels", t.channels);
        o->setProperty ("bitrateKbps", t.bitrateKbps);
        o->setProperty ("bytes", t.bytes);
        o->setProperty ("bpm", t.bpm);
        o->setProperty ("firstBeatMs", t.firstBeatMs);
        o->setProperty ("source", t.source);
        o->setProperty ("sourceFingerprint", t.sourceFingerprint);

        juce::Array<juce::var> cueList;

        for (const auto& c : t.cues)
            cueList.add (cueToJson (c));

        o->setProperty ("cues", cueList);
        trackList.add (juce::var (o));
    }

    juce::Array<juce::var> playlistList;

    for (const auto& p : playlists)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", p.name);
        juce::Array<juce::var> ids;

        for (auto id : p.trackIds)
            ids.add (id);

        o->setProperty ("tracks", ids);
        playlistList.add (juce::var (o));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "adormis-cdj-export");
    root->setProperty ("formatVersion", 1);
    root->setProperty ("writtenBy", writtenBy);
    root->setProperty ("writtenAt", writtenAt);
    root->setProperty ("tracks", trackList);
    root->setProperty ("playlists", playlistList);

    juce::Array<juce::var> formatList;

    for (const auto& f : formats)
        formatList.add (f);

    root->setProperty ("formats", formatList);
    return juce::var (root);
}

bool StickLibrary::fromJson (const juce::var& json, StickLibrary& library, juce::String& error)
{
    library = {};

    if (json["format"].toString() != "adormis-cdj-export")
    {
        error = "The list of tracks on the stick is not one this program wrote.";
        return false;
    }

    if ((int) json["formatVersion"] > 1)
    {
        error = "The stick was written by a newer version of Adormis CDJ Export.";
        return false;
    }

    library.writtenBy = json["writtenBy"].toString();
    library.writtenAt = json["writtenAt"].toString();

    // Sticks written before Serato support carry only rekordbox.
    if (const auto* list = json["formats"].getArray())
        for (const auto& f : *list)
            library.formats.addIfNotAlreadyThere (f.toString());
    else
        library.formats.add ("rekordbox");

    if (const auto* list = json["tracks"].getArray())
        for (const auto& v : *list)
        {
            StickTrack t;
            t.id = (int) v["id"];
            t.path = v["path"].toString();
            t.title = v["title"].toString();
            t.artist = v["artist"].toString();
            t.fileType = v["fileType"].toString();
            t.seconds = (double) v["seconds"];
            t.sampleRate = (int) v["sampleRate"];
            t.bitsPerSample = (int) v["bitsPerSample"];
            t.channels = (int) v["channels"];
            t.bitrateKbps = (int) v["bitrateKbps"];
            t.bytes = (juce::int64) v["bytes"];
            t.bpm = (double) v["bpm"];
            t.firstBeatMs = (double) v["firstBeatMs"];
            t.source = v["source"].toString();
            t.sourceFingerprint = v["sourceFingerprint"].toString();

            if (const auto* cues = v["cues"].getArray())
                for (const auto& c : *cues)
                    t.cues.push_back (cueFromJson (c));

            if (t.id > 0 && t.path.isNotEmpty())
                library.tracks.push_back (std::move (t));
        }

    if (const auto* list = json["playlists"].getArray())
        for (const auto& v : *list)
        {
            StickPlaylist p;
            p.name = v["name"].toString();

            if (const auto* ids = v["tracks"].getArray())
                for (const auto& id : *ids)
                    if (library.find ((int) id) != nullptr)
                        p.trackIds.push_back ((int) id);

            library.playlists.push_back (std::move (p));
        }

    return true;
}

bool StickLibrary::save (const juce::File& file, juce::String& error) const
{
    const auto text = juce::JSON::toString (toJson());
    juce::MemoryBlock data (text.toRawUTF8(), text.getNumBytesAsUTF8());
    return replaceFileSafely (file, data, error);
}

bool StickLibrary::load (const juce::File& file, StickLibrary& library, juce::String& error)
{
    juce::var json;
    const auto result = juce::JSON::parse (file.loadFileAsString(), json);

    if (result.failed())
    {
        error = "The list of tracks on the stick is damaged (" + result.getErrorMessage() + ").";
        return false;
    }

    return fromJson (json, library, error);
}

juce::String fatSafeName (const juce::String& name, int maxCharacters)
{
    juce::String out;

    for (auto c : name)
    {
        if (c < 32 || juce::String ("\\/:*?\"<>|").containsChar (c))
            out += "_";
        else
            out += juce::String::charToString (c);
    }

    out = out.trim().trimCharactersAtEnd (". ");

    if (out.length() > maxCharacters)
        out = out.substring (0, maxCharacters).trim().trimCharactersAtEnd (". ");

    static const juce::StringArray reserved { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                              "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                              "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };

    if (out.isEmpty() || reserved.contains (out.upToFirstOccurrenceOf (".", false, false), true))
        out = "_" + out;

    return out;
}

bool replaceFileSafely (const juce::File& target, const juce::MemoryBlock& data, juce::String& error)
{
    if (! target.getParentDirectory().createDirectory())
    {
        error = "Can't create the folder \"" + target.getParentDirectory().getFullPathName() + "\".";
        return false;
    }

    const auto temp = target.getSiblingFile (target.getFileName() + ".part");

    if (! temp.replaceWithData (data.getData(), data.getSize()))
    {
        temp.deleteFile();
        error = "Can't write \"" + target.getFullPathName() + "\" (stick full or write-protected?).";
        return false;
    }

    // FAT can't rename over an existing file atomically: remove, then rename.
    // The previous copy stays readable until the very last moment.
    if (target.exists() && ! target.deleteFile())
    {
        temp.deleteFile();
        error = "Can't replace \"" + target.getFullPathName() + "\".";
        return false;
    }

    if (! temp.moveFileTo (target))
    {
        error = "Can't rename \"" + temp.getFullPathName() + "\".";
        return false;
    }

    return true;
}
} // namespace cdj
