// Builds real exports into a temporary folder that stands in for a stick and
// checks what ends up there. Run: ./cdj_tests (exit code 0 = all passed).

#include "CuePlan.h"
#include "Drives.h"
#include "Exporter.h"
#include "Format.h"
#include "Library.h"
#include "Mp3.h"
#include "RekordboxXml.h"
#include "Tempo.h"

#include <cstdlib>
#include <iostream>

using namespace cdj;

namespace
{
int failures = 0, checks = 0;
juce::File realMusic;     // the user's Music folder, found before HOME is moved

void check (bool ok, const juce::String& what)
{
    ++checks;

    if (! ok)
    {
        ++failures;
        std::cout << "FAIL: " << what << std::endl;
    }
}

// A WAV with a click at every marker, written the way Adormis Audio writes
// markers (cue points + labels, regions as labelled text with a length).
juce::File makeWav (const juce::File& folder, const juce::String& name, double rate, int bits, double seconds,
                    const std::vector<std::pair<juce::int64, juce::String>>& markers,
                    const std::vector<Region>& regions = {}, bool floating = false)
{
    const auto file = folder.getChildFile (name);
    const auto length = (int) (rate * seconds);
    juce::AudioBuffer<float> audio (2, length);
    audio.clear();

    for (const auto& [pos, label] : markers)
        if (pos < length)
            for (int ch = 0; ch < 2; ++ch)
                audio.setSample (ch, (int) pos, 0.9f);

    std::unordered_map<juce::String, juce::String> meta;
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

    for (size_t i = 0; i < markers.size(); ++i)
        add ((int) i + 1, markers[i].first, markers[i].second);

    for (size_t i = 0; i < regions.size(); ++i)
    {
        const auto id = (int) (markers.size() + i + 1);
        add (id, regions[i].start, regions[i].name);
        const auto k = "CueRegion" + juce::String ((int) i);
        meta[k + "Identifier"] = juce::String (id);
        meta[k + "SampleLength"] = juce::String (regions[i].end - regions[i].start);
        meta[k + "Purpose"] = juce::String ((juce::int64) 0x206e6772);
        meta[k + "Text"] = regions[i].name;
    }

    meta["NumCuePoints"] = juce::String (cue);
    meta["NumCueLabels"] = juce::String (label);
    meta["NumCueRegions"] = juce::String ((int) regions.size());

    file.deleteFile();
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                .withSampleRate (rate)
                                                .withNumChannels (2)
                                                .withBitsPerSample (floating ? 32 : bits)
                                                .withMetadataValues (meta)
                                                .withSampleFormat (floating ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                            : juce::AudioFormatWriterOptions::SampleFormat::integral));
    writer->writeFromAudioSampleBuffer (audio, 0, length);
    return file;
}

Track load (const juce::File& f)
{
    Track t;
    juce::String error;
    check (loadTrack (f, t, error), "load " + f.getFileName() + ": " + error);
    return t;
}

void testFormatRules()
{
    check (cleanVolumeLabel ("My Stick ă!") == "MYSTICK", "volume label cleaned: " + cleanVolumeLabel ("My Stick ă!"));
    check (cleanVolumeLabel ("abcdefghijklmnop") == "ABCDEFGHIJK", "volume label at most 11 characters");
    check (cleanVolumeLabel ("") == "ADORMIS", "empty label gets a default");

    FormatPlan small, big, huge;
    small.diskBytes = 16LL << 30;
    big.diskBytes = 64LL << 30;
    huge.diskBytes = 4LL << 40;
    check (formatLimitation (small, FileSystem::fat32).isEmpty(), "FAT32 on 16 GB is fine everywhere");
    check (formatLimitation (big, FileSystem::exfat).isEmpty(), "exFAT on 64 GB is fine everywhere");
   #if JUCE_WINDOWS
    check (formatLimitation (big, FileSystem::fat32).isNotEmpty(), "Windows: FAT32 on 64 GB not offered yet");
   #else
    check (formatLimitation (big, FileSystem::fat32).isEmpty(), "FAT32 on 64 GB is fine here");
   #endif
    check (formatLimitation (huge, FileSystem::fat32).isNotEmpty(), "over 2 TB refused (MBR)");
}

void testCueMaths()
{
    check (samplesToMs (44100, 44100.0) == 1000, "1 s at 44.1 kHz = 1000 ms");
    check (samplesToMs (22, 44100.0) == 0, "22 samples round down");
    check (samplesToMs (23, 44100.0) == 1, "23 samples round up (0.52 ms)");
    check (samplesToMs (48000LL * 3600 * 2 + 24, 48000.0) == 7200001, "no drift after 2 hours");
    check (samplesToMs (96000, 96000.0) == 1000, "96 kHz");
}

void testMarkersAndCues (const juce::File& dir)
{
    std::vector<std::pair<juce::int64, juce::String>> markers;

    for (int i = 0; i < 10; ++i)
        markers.push_back ({ (juce::int64) (44100 * (i + 1)), i == 2 ? "Marker 3" : "Drop " + juce::String (i + 1) });

    const auto f = makeWav (dir, "Artist X - Track One.wav", 44100.0, 24, 14.0, markers,
                            { { 44100 * 12, 44100 * 13, "Outro loop" } });
    const auto t = load (f);

    check (t.markers.size() == 10, "10 markers read, got " + juce::String ((int) t.markers.size()));
    check (t.regions.size() == 1, "1 region read");
    check (t.artist == "Artist X" && t.title == "Track One", "artist/title from file name");
    check (t.markers.size() > 2 && t.markers[2].name.isEmpty(), "\"Marker 3\" means no name");
    check (t.markers.size() > 0 && t.markers[0].name == "Drop 1", "marker name kept");

    CueOptions o;
    auto cues = planCues (t, o);
    int hot = 0, memory = 0, loops = 0;

    for (const auto& c : cues)
    {
        if (c.kind == Cue::Kind::hot) ++hot; else ++memory;
        if (c.isLoop()) ++loops;
    }

    check (hot == 8, "8 hot cues, got " + juce::String (hot));
    check (memory == 11, "11 memory cues (10 markers + 1 region), got " + juce::String (memory));
    check (loops == 1, "region became a loop (memory only, it's the 11th point)");
    check (cues[0].hotIndex == 0 && cues[0].startMs == 1000 && cues[0].name == "Drop 1", "hot A at 1.000 s");
    check (cues[0].hotLetter() == "A", "letter A");

    o.hotCueCount = 3;
    o.memoryCues = false;
    cues = planCues (t, o);
    check (cues.size() == 3 && cues[2].hotLetter() == "C", "only A-C when asked");

    o.offsetMs = 25.0;
    cues = planCues (t, o);
    check (cues[0].startMs == 1025, "MP3 offset added");
}

void testNames()
{
    check (fatSafeName ("AC/DC: Back?") == "AC_DC_ Back_", "FAT-unsafe characters, got " + fatSafeName ("AC/DC: Back?"));
    check (fatSafeName ("con") == "_con", "reserved Windows name");
    check (fatSafeName ("Track. ") == "Track", "trailing dot/space");
    check (fatSafeName (juce::String::repeatedString ("a", 300)).length() == 100, "length limit");
    const juce::File withAccent (juce::String (juce::CharPointer_UTF8 ("/Volumes/STICK/Contents/Adormis/A B \xc4\x83.wav")));
    check (rekordboxLocation (withAccent) == "file://localhost/Volumes/STICK/Contents/Adormis/A%20B%20%C4%83.wav",
           "rekordbox location encoding: " + rekordboxLocation (withAccent));
}

void testExport (const juce::File& dir)
{
    const auto stick = dir.getChildFile ("stick");
    stick.createDirectory();

    const auto a = makeWav (dir, "A - Ready.wav", 44100.0, 16, 5.0, { { 44100, "Intro" }, { 88200, "" } });
    const auto b = makeWav (dir, "B - Float96.wav", 96000.0, 24, 4.0, { { 96000, "One" } }, {}, true);
    std::vector<Track> playlist { load (a), load (b) };

    check (isCdjReadyWav (playlist[0]), "16-bit 44.1 WAV is CDJ-ready");
    check (! isCdjReadyWav (playlist[1]), "float 96 kHz WAV is not");

    ExportOptions options;
    options.playlistName = "Set 1";
    auto report = exportToStick (playlist, stick, options, {});
    check (report.ok, "export 1: " + report.error);
    check (report.tracksCopied == 1 && report.tracksConverted == 1, "one copied, one converted");
    check (inspectStick (stick) == StickContent::adormis, "stick marked as written by Adormis");

    StickLibrary lib;
    juce::String error;
    check (StickLibrary::load (stickMarkerFile (stick), lib, error), "manifest loads: " + error);
    check (lib.tracks.size() == 2 && lib.playlists.size() == 1, "2 tracks, 1 playlist");

    if (lib.tracks.size() == 2)
    {
        const auto copied = stick.getChildFile (lib.tracks[0].path);
        const auto converted = stick.getChildFile (lib.tracks[1].path);
        check (copied.existsAsFile() && copied.getSize() == a.getSize(), "copied byte for byte");
        check (lib.tracks[0].path == "Contents/Adormis/A - Ready.wav", "path " + lib.tracks[0].path);

        const auto t = load (converted);
        check (t.sampleRate == 48000.0 && t.bitsPerSample == 24 && ! t.floatingPoint, "96k float → 48k 24-bit");
        check (std::abs (t.seconds() - 4.0) < 0.001, "length kept after resampling");
        check (lib.tracks[1].cues.size() == 2 && lib.tracks[1].cues[0].startMs == 1000, "cue at 1 s after resampling");
        check (lib.tracks[0].cues.size() == 4, "2 markers → 2 hot + 2 memory");
    }

    for (const auto& f : stick.findChildFiles (juce::File::findFiles, true, "*.part"))
        check (false, "leftover temp file " + f.getFullPathName());

    // rekordbox XML
    const auto xml = juce::parseXML (stick.getChildFile ("Adormis CDJ Export.xml"));
    check (xml != nullptr, "XML written and parses");

    if (xml != nullptr)
    {
        const auto* collection = xml->getChildByName ("COLLECTION");
        check (collection != nullptr && collection->getIntAttribute ("Entries") == 2, "XML collection has 2 tracks");

        if (collection != nullptr && collection->getFirstChildElement() != nullptr)
        {
            const auto* first = collection->getFirstChildElement();
            int hot = 0, memory = 0;

            for (auto* m : first->getChildWithTagNameIterator ("POSITION_MARK"))
                (m->getIntAttribute ("Num") >= 0 ? hot : memory)++;

            check (hot == 2 && memory == 2, "XML: 2 hot (Num 0,1) + 2 memory (Num -1)");
            check (first->getChildByName ("POSITION_MARK")->getStringAttribute ("Start") == "1.000", "XML Start in seconds");
        }
    }

    // A second export adds a playlist and reuses the identical track.
    ExportOptions second = options;
    second.playlistName = "Set 2";
    std::vector<Track> playlist2 { load (a) };
    report = exportToStick (playlist2, stick, second, {});
    check (report.ok && report.tracksReused == 1, "export 2 adds a playlist and reuses the track: " + report.error);
    check (StickLibrary::load (stickMarkerFile (stick), lib, error) && lib.playlists.size() == 2 && lib.tracks.size() == 2,
           "2 playlists, still 2 tracks");

    second.playlistName = "set 1";
    report = exportToStick (playlist2, stick, second, {});
    check (! report.ok, "same playlist name refused");

    // Cancel half way: nothing new stays on the stick.
    const auto before = stick.findChildFiles (juce::File::findFiles, true).size();
    const auto c = makeWav (dir, "C - New.wav", 44100.0, 24, 2.0, {});
    std::vector<Track> playlist3 { load (c), load (b) };
    ExportOptions third = options;
    third.playlistName = "Set 3";
    int calls = 0;
    report = exportToStick (playlist3, stick, third, [&] (double, const juce::String&) { return ++calls < 2; });
    check (report.cancelled, "cancel reported");
    check (stick.findChildFiles (juce::File::findFiles, true).size() == before, "cancel left nothing behind");

    // Replace everything: old files that are no longer listed are removed.
    third.replaceEverything = true;
    report = exportToStick ({ load (c) }, stick, third, {});
    check (report.ok, "replace everything: " + report.error);
    check (StickLibrary::load (stickMarkerFile (stick), lib, error) && lib.tracks.size() == 1 && lib.playlists.size() == 1,
           "only the new playlist is left");
    check (! stick.getChildFile ("Contents/Adormis/A - Ready.wav").exists(), "old file removed");

    // A stick with a rekordbox library is never touched.
    const auto other = dir.getChildFile ("rekordbox-stick");
    other.getChildFile ("PIONEER/rekordbox").createDirectory();
    other.getChildFile ("PIONEER/rekordbox/export.pdb").replaceWithText ("x");
    report = exportToStick (playlist2, other, options, {});
    check (! report.ok && inspectStick (other) == StickContent::rekordbox, "rekordbox stick refused");
    check (other.findChildFiles (juce::File::findFiles, true).size() == 1, "rekordbox stick unchanged");
}

void testMp3 (const juce::File& dir)
{
    if (! mp3EncodingAvailable())
    {
        std::cout << "(MP3 tests skipped: no LAME in this build)" << std::endl;
        return;
    }

    const auto src = makeWav (dir, "D - Mp3 me.wav", 44100.0, 24, 6.0, { { 44100 * 2, "Two" } });
    const auto stick = dir.getChildFile ("mp3-stick");
    stick.createDirectory();

    ExportOptions options;
    options.format = ExportOptions::Format::mp3;
    options.playlistName = "MP3";
    const auto report = exportToStick ({ load (src) }, stick, options, {});
    check (report.ok, "MP3 export: " + report.error);

    if (report.ok && ! report.library.tracks.empty())
    {
        const auto& st = report.library.tracks[0];
        const auto mp3 = stick.getChildFile (st.path);
        const auto delay = mp3EncoderDelay (mp3);
        check (delay > 0 && delay < 3000, "LAME header has the encoder delay: " + juce::String (delay));
        check (mp3Bitrate (mp3) == 320, "320 kbps CBR, got " + juce::String (mp3Bitrate (mp3)));

        const auto expected = 2000 + (juce::int64) std::llround ((delay + 529) * 1000.0 / 44100.0);
        check (! st.cues.empty() && st.cues[0].startMs == expected,
               "MP3 cue shifted by the delay: " + juce::String (st.cues.empty() ? -1 : st.cues[0].startMs)
                   + " vs " + juce::String (expected));
        check (st.path.endsWithIgnoreCase (".mp3"), "MP3 on the stick");

        // Where the click really is in the decoded MP3 (JUCE's decoder keeps the delay).
        std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (mp3));
        juce::AudioBuffer<float> audio (1, (int) reader->lengthInSamples);
        reader->read (&audio, 0, audio.getNumSamples(), 0, true, false);
        const auto peak = audio.findMinMax (0, 0, audio.getNumSamples());
        int at = 0;
        float best = 0.0f;

        for (int i = 0; i < audio.getNumSamples(); ++i)
            if (std::abs (audio.getSample (0, i)) > best)
            {
                best = std::abs (audio.getSample (0, i));
                at = i;
            }

        juce::ignoreUnused (peak);
        const auto clickMs = at * 1000.0 / reader->sampleRate;
        std::cout << "  MP3 click decoded at " << clickMs << " ms, cue at " << st.cues[0].startMs
                  << " ms (encoder delay " << delay << ")" << std::endl;
        check (std::abs (clickMs - (double) st.cues[0].startMs) <= 2.0, "cue on the click in the decoded MP3 (±2 ms)");
    }
}

// A drum loop: kick on "1" (louder) and "3", snare on "2" and "4", hi-hat on
// every eighth; the first downbeat at `first` seconds.
juce::AudioBuffer<float> drumLoop (double rate, double bpm, double first, double seconds)
{
    const auto length = (int) (rate * seconds);
    juce::AudioBuffer<float> audio (2, length);
    audio.clear();
    juce::Random random (42);
    const auto beat = 60.0 / bpm;

    auto hit = [&] (double at, int kind)
    {
        const auto start = (int) std::llround (at * rate);

        for (int i = 0; i < (int) (rate * 0.25) && start + i < length; ++i)
        {
            if (start + i < 0)
                continue;

            const auto t = i / rate;
            float v = 0.0f;

            if (kind == 0 || kind == 3)
                v = (kind == 0 ? 0.9f : 0.45f) * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * 55.0 * t) * std::exp (-t * 18.0));
            else if (kind == 1) v = 0.5f * (random.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-t * 30.0);
            else                v = 0.15f * (random.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-t * 120.0);

            for (int ch = 0; ch < 2; ++ch)
                audio.addSample (ch, start + i, v);
        }
    };

    for (int k = -8; first + k * beat / 2.0 < seconds; ++k)
    {
        const auto at = first + k * beat / 2.0;

        if (at < 0.0)
            continue;

        const auto beatIndex = ((k % 8) + 8) % 8;    // eighths within the bar

        if (beatIndex == 0) hit (at, 0);        // accented kick on the "1"
        if (beatIndex == 4) hit (at, 3);        // softer kick on the "3"
        if (beatIndex == 2 || beatIndex == 6) hit (at, 1);
        hit (at, 2);
    }

    return audio;
}

void testTempo()
{
    const double rate = 44100.0, bpm = 126.0, first = 0.731;
    auto audio = drumLoop (rate, bpm, first, 40.0);
    const auto measured = measureTempo (audio, { 0, audio.getNumSamples() }, rate);
    check (measured.has_value() && std::abs (*measured - bpm) < 0.15,
           "tempo detected: " + (measured.has_value() ? juce::String (*measured, 2) : juce::String ("none")));

    if (measured.has_value())
    {
        const auto downbeat = findFirstDownbeat (audio, rate, *measured);
        const auto error = downbeat.has_value() ? (double) *downbeat / rate - first : 99.0;
        std::cout << "  first downbeat found " << juce::String (error * 1000.0, 1) << " ms from the real one" << std::endl;
        check (std::abs (error) < 0.010, "first downbeat within 10 ms");
    }

    // A downbeat right at the start must give 0, not one bar later.
    auto atZero = drumLoop (rate, 120.0, 0.0, 30.0);
    const auto zero = findFirstDownbeat (atZero, rate, 120.0);
    check (zero.has_value() && *zero < (juce::int64) (0.005 * rate),
           "downbeat at 0 s found at the start: " + juce::String (zero.has_value() ? (double) *zero / rate : -1.0, 3));

    // A real song, when it's on this computer (not a pass/fail check).
    const auto song = realMusic.getChildFile ("Monkeys Spinning Monkeys.mp3");

    if (song.existsAsFile())
    {
        std::unique_ptr<juce::AudioFormatReader> r (formats().createReaderFor (song));
        juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&b, 0, b.getNumSamples(), 0, true, true);
        const auto bpm = measureTempo (b, { 0, b.getNumSamples() }, r->sampleRate);
        const auto fb = bpm.has_value() ? findFirstDownbeat (b, r->sampleRate, *bpm) : std::optional<juce::int64>();
        std::cout << "  " << song.getFileName() << ": " << (bpm.has_value() ? juce::String (*bpm, 2) : juce::String ("no")) << " BPM, first downbeat "
                  << (fb.has_value() ? juce::String ((double) *fb / r->sampleRate, 3) : juce::String ("-")) << " s" << std::endl;
    }

    juce::AudioBuffer<float> silence (1, (int) (rate * 20.0));
    silence.clear();
    check (! measureTempo (silence, { 0, silence.getNumSamples() }, rate).has_value(), "no tempo in silence");
}

void testSaveMarkers (const juce::File& dir)
{
    const auto f = makeWav (dir, "E - Save me.wav", 48000.0, 24, 3.0, { { 48000, "Old" } },
                            { { 96000, 120000, "Loop" } });
    auto t = load (f);

    std::unique_ptr<juce::AudioFormatReader> before (formats().createReaderFor (f));
    juce::AudioBuffer<float> a ((int) before->numChannels, (int) before->lengthInSamples);
    before->read (&a, 0, a.getNumSamples(), 0, true, true);
    before.reset();

    t.markers.push_back ({ 24000, "New one" });
    t.markers.erase (t.markers.begin() + 0);
    t.markers.push_back ({ 60000, {} });
    t.markersChanged = true;
    juce::String error;
    check (saveMarkers (t, error), "markers saved: " + error);
    check (! t.markersChanged, "saved flag cleared");

    const auto again = load (f);
    check (again.markers.size() == 2 && again.markers[0].sample == 24000 && again.markers[0].name == "New one"
               && again.markers[1].sample == 60000 && again.markers[1].name.isEmpty(),
           "markers read back after saving");
    check (again.regions.size() == 1 && again.regions[0].start == 96000 && again.regions[0].end == 120000,
           "region kept");

    std::unique_ptr<juce::AudioFormatReader> after (formats().createReaderFor (f));
    juce::AudioBuffer<float> b ((int) after->numChannels, (int) after->lengthInSamples);
    after->read (&b, 0, b.getNumSamples(), 0, true, true);
    check (after->bitsPerSample == 24 && after->sampleRate == 48000.0, "same format after saving");

    bool same = a.getNumSamples() == b.getNumSamples();

    for (int ch = 0; same && ch < a.getNumChannels(); ++ch)
        for (int i = 0; same && i < a.getNumSamples(); ++i)
            same = a.getSample (ch, i) == b.getSample (ch, i);

    check (same, "audio sample for sample the same after saving markers");
    check (dir.findChildFiles (juce::File::findFiles, false, "*.tmp").isEmpty(), "no temp file left");

    // Tempo and first beat are remembered for the file.
    t.bpm = 127.5;
    t.firstBeat = 1234;
    t.gridByHand = true;
    rememberTrack (t);
    const auto recalled = load (f);
    check (recalled.bpm == 127.5 && recalled.firstBeat == 1234 && recalled.gridByHand, "grid remembered");
}

void testGridExport (const juce::File& dir)
{
    const auto stick = dir.getChildFile ("grid-stick");
    stick.createDirectory();
    const auto f = makeWav (dir, "F - Grid.wav", 44100.0, 16, 4.0, { { 44100, "x" } });
    auto t = load (f);
    t.bpm = 128.0;
    t.firstBeat = 22050;    // 0.5 s

    ExportOptions options;
    options.playlistName = "Grid";
    const auto report = exportToStick ({ t }, stick, options, {});
    check (report.ok && ! report.library.tracks.empty() && report.library.tracks[0].bpm == 128.0
               && report.library.tracks[0].firstBeatMs == 500.0,
           "grid on the stick: 128 BPM from 500 ms");

    const auto xml = juce::parseXML (stick.getChildFile ("Adormis CDJ Export.xml"));
    const auto* tempo = xml != nullptr ? xml->getChildByName ("COLLECTION")->getFirstChildElement()->getChildByName ("TEMPO") : nullptr;
    check (tempo != nullptr && tempo->getStringAttribute ("Inizio") == "0.500" && tempo->getStringAttribute ("Bpm") == "128.00",
           "XML TEMPO Inizio/Bpm");
}
} // namespace

int main()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("cdj-export-tests");
    dir.deleteRecursively();
    dir.createDirectory();

    realMusic = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    // Remembered tempos go to a settings folder inside the test folder,
    // never the user's own.
    setenv ("HOME", dir.getChildFile ("home").getFullPathName().toRawUTF8(), 1);
    dir.getChildFile ("home").createDirectory();

    testCueMaths();
    testFormatRules();
    testNames();
    testMarkersAndCues (dir);
    testExport (dir);
    testMp3 (dir);
    testTempo();
    testSaveMarkers (dir);
    testGridExport (dir);

    std::cout << checks - failures << " of " << checks << " checks passed" << std::endl;

    if (failures == 0)
        dir.deleteRecursively();
    else
        std::cout << "Test files kept in " << dir.getFullPathName() << std::endl;

    return failures == 0 ? 0 : 1;
}
