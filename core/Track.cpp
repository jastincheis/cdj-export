#include "Track.h"

#include "Mp3.h"

#include <algorithm>
#include <map>
#include <set>

namespace cdj
{
juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager manager;
    static const bool registered = (manager.registerBasicFormats(), true);
    juce::ignoreUnused (registered);
    return manager;
}

void titleFromFileName (const juce::File& file, juce::String& artist, juce::String& title)
{
    const auto name = file.getFileNameWithoutExtension().trim();
    const auto dash = name.indexOf (" - ");

    if (dash > 0)
    {
        artist = name.substring (0, dash).trim();
        title = name.substring (dash + 3).trim();
    }
    else
    {
        artist = {};
        title = name;
    }
}

void readWavCues (const juce::StringPairArray& meta, juce::int64 total,
                  std::vector<Marker>& markers, std::vector<Region>& regions)
{
    std::map<int, juce::int64> offsets;
    std::map<int, juce::String> labels;

    for (int i = 0; i < meta["NumCuePoints"].getIntValue(); ++i)
    {
        const auto c = "Cue" + juce::String (i);
        offsets[meta[c + "Identifier"].getIntValue()] = meta[c + "Offset"].getLargeIntValue();
    }

    for (int i = 0; i < meta["NumCueLabels"].getIntValue(); ++i)
    {
        const auto l = "CueLabel" + juce::String (i);
        labels[meta[l + "Identifier"].getIntValue()] = meta[l + "Text"];
    }

    std::set<int> regionIds;

    for (int i = 0; i < meta["NumCueRegions"].getIntValue(); ++i)
    {
        const auto k = "CueRegion" + juce::String (i);
        const auto id = meta[k + "Identifier"].getIntValue();
        const auto length = meta[k + "SampleLength"].getLargeIntValue();

        if (offsets.count (id) == 0 || length <= 0)
            continue;

        regionIds.insert (id);
        const auto start = juce::jlimit ((juce::int64) 0, total, offsets[id]);
        const auto end = juce::jlimit ((juce::int64) 0, total, start + length);
        auto name = labels.count (id) != 0 && labels[id].isNotEmpty() ? labels[id] : meta[k + "Text"];

        if (end > start)
            regions.push_back ({ start, end, name.trim() });
    }

    for (const auto& [id, position] : offsets)
        if (regionIds.count (id) == 0 && position >= 0 && position <= total)
        {
            const auto label = labels.count (id) != 0 ? labels[id].trim() : juce::String();
            const auto plain = label.startsWith ("Marker ") && label.substring (7).containsOnly ("0123456789");
            markers.push_back ({ position, plain ? juce::String() : label });
        }

    std::stable_sort (markers.begin(), markers.end(), [] (const auto& a, const auto& b) { return a.sample < b.sample; });
    markers.erase (std::unique (markers.begin(), markers.end(),
                                [] (const auto& a, const auto& b) { return a.sample == b.sample; }),
                   markers.end());
    std::stable_sort (regions.begin(), regions.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
}

namespace
{
bool readMarkersFrom (const juce::File& wav, Track& track)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (wav));

    if (reader == nullptr)
        return false;

    readWavCues (reader->metadataValues, reader->lengthInSamples, track.markers, track.regions);
    track.markerRate = reader->sampleRate;
    track.markerSource = wav;
    return true;
}

juce::File siblingWav (const juce::File& file)
{
    for (const auto* ext : { ".wav", ".WAV", ".Wav" })
    {
        const auto candidate = file.withFileExtension (ext);

        if (candidate.existsAsFile())
            return candidate;
    }

    return {};
}
} // namespace

bool loadTrack (const juce::File& file, Track& track, juce::String& error)
{
    track = {};
    track.file = file;
    track.extension = file.getFileExtension().trimCharactersAtStart (".").toLowerCase();

    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));

    if (reader == nullptr)
    {
        error = "\"" + file.getFileName() + "\" is not an audio file this program can read.";
        return false;
    }

    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        error = "\"" + file.getFileName() + "\" has no audio in it.";
        return false;
    }

    track.sampleRate = reader->sampleRate;
    track.lengthSamples = reader->lengthInSamples;
    track.channels = (int) reader->numChannels;
    track.bitsPerSample = (int) reader->bitsPerSample;
    track.floatingPoint = reader->usesFloatingPointData;
    track.compressed = track.extension != "wav" && track.extension != "aif" && track.extension != "aiff"
                       && track.extension != "flac";

    titleFromFileName (file, track.artist, track.title);

    // WAV LIST INFO tags, when the file has them.
    const auto& meta = reader->metadataValues;

    if (meta["INAM"].trim().isNotEmpty())
        track.title = meta["INAM"].trim();

    if (meta["IART"].trim().isNotEmpty())
        track.artist = meta["IART"].trim();

    if (track.extension == "wav")
    {
        readWavCues (meta, track.lengthSamples, track.markers, track.regions);
        track.markerRate = track.sampleRate;
        track.markerSource = file;
    }
    else
    {
        // An MP3 carries no Sound Forge markers: take them from the WAV it was
        // made from, when it sits next to it with the same name.
        const auto wav = siblingWav (file);

        if (wav != juce::File() && readMarkersFrom (wav, track))
        {
            track.notes.add ("Markers taken from " + wav.getFileName());

            if (track.extension == "mp3")
            {
                const auto delay = mp3EncoderDelay (file);
                track.markerShiftSeconds = delay >= 0 ? (delay + 529) / track.sampleRate : 0.0;
            }
        }
        else
        {
            track.markerRate = track.sampleRate;    // markers added here sit on this file's timeline
            track.notes.add ("No markers: put the WAV with the same name next to this file to use its markers");
        }
    }

    recallTrack (track);

    if (track.markers.empty() && track.regions.empty() && track.extension == "wav")
        track.notes.add ("This WAV has no markers");

    return true;
}

namespace
{
juce::File storeFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Adormis CDJ Export").getChildFile ("tracks.json");
}

juce::var loadStore()
{
    juce::var v;

    if (juce::JSON::parse (storeFile().loadFileAsString(), v).failed() || ! v.isObject())
        v = juce::var (new juce::DynamicObject());

    return v;
}

// Markers live in this program's list only for files that can't hold them.
bool keepsOwnMarkers (const Track& t)
{
    return t.markerSource == juce::File() || (t.markerSource == t.file && t.extension != "wav");
}

juce::String cueText (const juce::String& name, int index)
{
    return name.isNotEmpty() ? name : "Marker " + juce::String (index);
}
} // namespace

void rememberTrack (const Track& t)
{
    auto store = loadStore();
    auto* entry = new juce::DynamicObject();
    entry->setProperty ("length", t.lengthSamples);
    entry->setProperty ("bpm", t.bpm);
    entry->setProperty ("firstBeat", t.firstBeat);
    entry->setProperty ("byHand", t.gridByHand);

    if (keepsOwnMarkers (t))
    {
        juce::Array<juce::var> list;

        for (const auto& m : t.markers)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("seconds", t.markerSeconds (m.sample));
            o->setProperty ("name", m.name);
            list.add (juce::var (o));
        }

        entry->setProperty ("markers", list);
    }

    store.getDynamicObject()->setProperty (t.file.getFullPathName(), juce::var (entry));
    storeFile().getParentDirectory().createDirectory();
    storeFile().replaceWithText (juce::JSON::toString (store));
}

void recallTrack (Track& t)
{
    const auto entry = loadStore()[juce::Identifier (t.file.getFullPathName())];

    if (! entry.isObject() || (juce::int64) entry["length"] != t.lengthSamples)
        return;

    t.bpm = (double) entry["bpm"];
    t.firstBeat = (juce::int64) entry["firstBeat"];
    t.gridByHand = (bool) entry["byHand"];

    if (keepsOwnMarkers (t))
        if (const auto* list = entry["markers"].getArray())
        {
            t.markers.clear();
            t.markerRate = t.sampleRate;
            t.markerSource = juce::File();

            for (const auto& m : *list)
                t.markers.push_back ({ (juce::int64) std::llround ((double) m["seconds"] * t.sampleRate), m["name"].toString() });
        }
}

bool saveMarkers (Track& t, juce::String& error)
{
    if (keepsOwnMarkers (t))
    {
        rememberTrack (t);
        t.markersChanged = false;
        return true;
    }

    const auto wav = t.markerSource;
    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (wav));

    if (reader == nullptr)
    {
        error = "Can't read \"" + wav.getFileName() + "\" again to save the markers.";
        return false;
    }

    // Keep every other tag the file has; replace only the cue information.
    std::unordered_map<juce::String, juce::String> meta;

    for (const auto& key : reader->metadataValues.getAllKeys())
        if (! key.startsWith ("Cue") && ! key.startsWith ("NumCue"))
            meta[key] = reader->metadataValues[key];

    int cue = 0, label = 0;
    auto add = [&] (int id, juce::int64 pos, const juce::String& text)
    {
        const auto c = "Cue" + juce::String (cue++);
        meta[c + "Identifier"] = juce::String (id);
        meta[c + "Offset"] = juce::String (pos);
        meta[c + "SampleOffset"] = juce::String (pos);
        const auto l = "CueLabel" + juce::String (label++);
        meta[l + "Identifier"] = juce::String (id);
        meta[l + "Text"] = text;
    };

    for (size_t i = 0; i < t.markers.size(); ++i)
        add ((int) i + 1, t.markers[i].sample, cueText (t.markers[i].name, (int) i + 1));

    for (size_t i = 0; i < t.regions.size(); ++i)
    {
        const auto& r = t.regions[i];
        const auto id = (int) (t.markers.size() + i + 1);
        add (id, r.start, r.name);
        const auto k = "CueRegion" + juce::String ((int) i);
        meta[k + "Identifier"] = juce::String (id);
        meta[k + "SampleLength"] = juce::String (r.end - r.start);
        meta[k + "Purpose"] = juce::String ((juce::int64) 0x206e6772);   // "rgn "
        meta[k + "Text"] = r.name;
    }

    meta["NumCuePoints"] = juce::String (cue);
    meta["NumCueLabels"] = juce::String (label);
    meta["NumCueRegions"] = juce::String ((int) t.regions.size());

    const auto temp = wav.getSiblingFile (wav.getFileNameWithoutExtension() + ".saving-markers.tmp");
    temp.deleteFile();

    {
        std::unique_ptr<juce::OutputStream> out (temp.createOutputStream());
        juce::WavAudioFormat format;
        auto writer = format.createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (reader->sampleRate)
                                                       .withNumChannels ((int) reader->numChannels)
                                                       .withBitsPerSample ((int) reader->bitsPerSample)
                                                       .withMetadataValues (meta)
                                                       .withSampleFormat (reader->usesFloatingPointData
                                                                              ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                              : juce::AudioFormatWriterOptions::SampleFormat::integral));

        // Integer samples are copied as integers: nothing is converted.
        if (writer == nullptr || ! writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples))
        {
            writer.reset();
            temp.deleteFile();
            error = "Saving the markers into \"" + wav.getFileName() + "\" failed (disk full or read-only?).";
            return false;
        }
    }

    const auto originalLength = reader->lengthInSamples;
    reader.reset();

    // The new file must read back with the same length before it replaces the old one.
    // (Read as WAV explicitly: the temporary name doesn't end in .wav.)
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatReader> check (wavFormat.createReaderFor (new juce::FileInputStream (temp), true));
    const auto ok = check != nullptr && check->lengthInSamples == originalLength;
    check.reset();

    if (! ok)
    {
        temp.deleteFile();
        error = "Saving the markers into \"" + wav.getFileName() + "\" failed: the new copy didn't read back "
                "complete. The file was not changed.";
        return false;
    }

    if (! temp.replaceFileIn (wav))
    {
        temp.deleteFile();
        error = "Saving the markers into \"" + wav.getFileName() + "\" failed: it couldn't be replaced "
                "(open in another program, or read-only?). The file was not changed.";
        return false;
    }

    t.markersChanged = false;

    if (wav == t.file)
        t.notes.removeString ("This WAV has no markers");

    return true;
}
} // namespace cdj
