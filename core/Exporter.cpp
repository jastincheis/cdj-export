#include "Exporter.h"

#include "Drives.h"
#include "Mp3.h"
#include "RekordboxXml.h"

#include <cmath>
#include <set>

namespace cdj
{
juce::String programVersion()
{
   #ifdef CDJ_VERSION
    return CDJ_VERSION;
   #else
    return "0.0.0";
   #endif
}

bool isCdjReadyWav (const Track& t)
{
    return t.extension == "wav" && ! t.floatingPoint && (t.bitsPerSample == 16 || t.bitsPerSample == 24)
        && (std::abs (t.sampleRate - 44100.0) < 0.5 || std::abs (t.sampleRate - 48000.0) < 0.5)
        && (t.channels == 1 || t.channels == 2);
}

namespace
{
bool isCdjReadyMp3 (const Track& t)
{
    return t.extension == "mp3" && (std::abs (t.sampleRate - 44100.0) < 0.5 || std::abs (t.sampleRate - 48000.0) < 0.5);
}

constexpr int mp3DecoderDelay = 529;    // what every MP3 decoder adds on top of the encoder delay

juce::String fingerprint (const juce::File& f)
{
    return juce::String (f.getSize()) + ":" + juce::String (f.getLastModificationTime().toMilliseconds());
}

// Target sample rate for a converted WAV: keep 44.1/48 kHz families.
double cdjRate (double rate)
{
    return std::fmod (rate, 48000.0) < 0.5 ? 48000.0 : 44100.0;
}

bool readWholeFile (const juce::File& file, juce::AudioBuffer<float>& audio, double& rate, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        error = "Can't read \"" + file.getFileName() + "\".";
        return false;
    }

    const auto channels = (int) juce::jmin (2u, reader->numChannels);
    audio.setSize (channels, (int) reader->lengthInSamples);
    reader->read (&audio, 0, (int) reader->lengthInSamples, 0, true, channels > 1);
    rate = reader->sampleRate;
    return true;
}

void resample (juce::AudioBuffer<float>& audio, double from, double to)
{
    if (std::abs (from - to) < 0.5)
        return;

    const auto ratio = from / to;
    const auto outLength = (int) std::llround ((double) audio.getNumSamples() / ratio);
    juce::AudioBuffer<float> out (audio.getNumChannels(), outLength);

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        juce::WindowedSincInterpolator interpolator;
        interpolator.process (ratio, audio.getReadPointer (ch), out.getWritePointer (ch), outLength,
                              audio.getNumSamples(), 0);
    }

    audio = std::move (out);
}

bool writeWav (const juce::File& destination, const juce::AudioBuffer<float>& audio, double rate, int bits,
               juce::String& error)
{
    destination.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (destination.createOutputStream());

    if (stream == nullptr)
    {
        error = "Can't write \"" + destination.getFullPathName() + "\".";
        return false;
    }

    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (rate)
                             .withNumChannels (audio.getNumChannels())
                             .withBitsPerSample (bits);
    auto writer = wav.createWriterFor (stream, options);

    if (writer == nullptr)
    {
        error = "Can't write a " + juce::String (bits) + "-bit WAV at " + juce::String (rate) + " Hz.";
        return false;
    }

    // Round instead of truncating, and keep inside the integer range.
    juce::AudioBuffer<float> safe (audio);

    for (int ch = 0; ch < safe.getNumChannels(); ++ch)
        juce::FloatVectorOperations::clip (safe.getWritePointer (ch), safe.getReadPointer (ch), -1.0f, 1.0f,
                                           safe.getNumSamples());

    if (! writer->writeFromAudioSampleBuffer (safe, 0, safe.getNumSamples()))
    {
        error = "Writing \"" + destination.getFileName() + "\" failed (stick full?).";
        return false;
    }

    return true;
}

juce::String uniquePath (const juce::String& folder, const juce::String& name, const juce::String& ext,
                         std::set<juce::String>& used)
{
    auto base = fatSafeName (name);

    for (int n = 1;; ++n)
    {
        const auto candidate = folder + "/" + (n == 1 ? base : base + " (" + juce::String (n) + ")") + "." + ext;

        if (used.insert (candidate.toLowerCase()).second)
            return candidate;
    }
}
// How much later than on its own timeline everything sits in the file
// that ends up on the stick (MP3 encoder + decoder delay), in ms.
struct Offsets
{
    double markersMs = 0.0, gridMs = 0.0;
    bool unknownDelay = false;
};

Offsets offsetsFor (const Track& t, const juce::File& output, const juce::String& type, bool copied, bool correct)
{
    Offsets o;

    if (! correct)
        return o;

    if (type == "mp3" && ! copied && t.extension != "mp3")
    {
        // Encoded here from a WAV: markers and grid were both on the WAV's timeline.
        const auto delay = mp3EncoderDelay (output);
        const auto rate = (double) juce::jmax (1, mp3SampleRate (output));

        if (delay >= 0)
            o.markersMs = o.gridMs = (delay + mp3DecoderDelay) * 1000.0 / rate;
        else
            o.unknownDelay = true;
    }
    else if (t.extension == "mp3")
    {
        // The source MP3 (copied, or decoded to WAV with its delay in it):
        // the grid was set on it, markers from the WAV next to it need the shift.
        o.markersMs = t.markerShiftSeconds * 1000.0;
        o.unknownDelay = t.markerSource != juce::File() && t.markerSource != t.file && t.markerShiftSeconds <= 0.0;
    }

    return o;
}
} // namespace

juce::String outputType (const Track& t, ExportOptions::Format format, bool& copyUnchanged)
{
    copyUnchanged = false;

    switch (format)
    {
        case ExportOptions::Format::keep:
            if (isCdjReadyWav (t) || isCdjReadyMp3 (t))
            {
                copyUnchanged = true;
                return t.extension;
            }
            return "wav";

        case ExportOptions::Format::wav:
            copyUnchanged = isCdjReadyWav (t);
            return "wav";

        case ExportOptions::Format::mp3:
            copyUnchanged = isCdjReadyMp3 (t);
            return "mp3";
    }

    return "wav";
}

ExportReport exportToStick (const std::vector<Track>& playlist, const juce::File& root,
                            const ExportOptions& options, const ExportProgress& progress,
                            const PioneerWriter& pioneerWriter)
{
    ExportReport report;
    const auto say = [&] (double f, const juce::String& s) { return progress == nullptr || progress (f, s); };

    if (playlist.empty())
    {
        report.error = "The playlist is empty.";
        return report;
    }

    if (! root.isDirectory())
    {
        report.error = "The stick isn't there any more (\"" + root.getFullPathName() + "\").";
        return report;
    }

    // A stick pulled out in the instant between the two renames of the
    // Pioneer writer has only the previous library, set aside: put it back.
    {
        const auto pioneer = root.getChildFile ("PIONEER");
        const auto setAside = root.getChildFile ("PIONEER.adormis-old");

        if (! pioneer.exists() && setAside.isDirectory())
            setAside.moveFileTo (pioneer);
    }

    // Never touch a library someone else made.
    StickLibrary library;
    const auto content = inspectStick (root);

    if (content == StickContent::rekordbox)
    {
        report.error = "This stick already has a rekordbox library on it (a PIONEER folder this program "
                       "didn't write). To keep it safe, nothing was written. Use an empty stick, "
                       "or delete the PIONEER folder yourself if you no longer need it.";
        return report;
    }

    StickLibrary previous;

    if (content == StickContent::adormis)
    {
        juce::String error;

        if (! StickLibrary::load (stickMarkerFile (root), previous, error))
        {
            report.error = error;
            return report;
        }

        if (! options.replaceEverything)
            library = previous;
    }

    if (! options.replaceEverything)
        if (! library.playlists.empty())
            for (const auto& p : library.playlists)
                if (p.name.equalsIgnoreCase (options.playlistName))
                {
                    report.error = "The stick already has a playlist called \"" + options.playlistName
                                 + "\". Pick another name, or choose to replace everything.";
                    return report;
                }

    // Plan every track first: names, type, size, cues.
    struct Job
    {
        const Track* track = nullptr;
        juce::String type, path;
        bool copy = false;
        int reuseId = 0;
    };

    std::vector<Job> jobs;
    std::set<juce::String> usedPaths;

    for (const auto& t : library.tracks)
        usedPaths.insert (t.path.toLowerCase());

    // Replacing everything: still never reuse an old file's name, so a
    // cancelled export can't have overwritten anything the old list points to.
    for (const auto& t : previous.tracks)
        usedPaths.insert (t.path.toLowerCase());

    juce::int64 bytesNeeded = 0;

    for (const auto& t : playlist)
    {
        Job job;
        job.track = &t;
        job.type = outputType (t, options.format, job.copy);

        if (job.type == "mp3" && ! job.copy && ! mp3EncodingAvailable())
        {
            report.error = "This version can't make MP3 files. Choose WAV or \"Keep the format\".";
            return report;
        }

        // The same source, already on the stick in the same type: don't copy it again.
        for (const auto& existing : library.tracks)
            if (existing.source == t.file.getFullPathName() && existing.sourceFingerprint == fingerprint (t.file)
                && existing.fileType == job.type && root.getChildFile (existing.path).existsAsFile())
                job.reuseId = existing.id;

        if (job.reuseId == 0)
        {
            const auto name = t.artist.isNotEmpty() ? t.artist + " - " + t.title : t.title;
            job.path = uniquePath ("Contents/Adormis", name, job.type, usedPaths);
            bytesNeeded += job.copy ? t.file.getSize()
                                    : (juce::int64) (t.seconds() * (job.type == "mp3" ? options.mp3Kbps * 125.0
                                                                                         : 48000.0 * 2 * 3)) + 65536;
        }

        jobs.push_back (job);
    }

    {
        Drive d;
        d.root = root;

        for (const auto& drive : findDrives())
            if (drive.root == root)
                d = drive;

        if (d.freeBytes > 0 && bytesNeeded > d.freeBytes)
        {
            report.error = "Not enough room on the stick: about " + juce::File::descriptionOfSizeInBytes (bytesNeeded)
                         + " needed, " + juce::File::descriptionOfSizeInBytes (d.freeBytes) + " free.";
            return report;
        }
    }

    // Write the audio. Each file is written as "<name>.part" and renamed when
    // complete; on cancel or error every file this run made is removed again.
    juce::Array<juce::File> written;
    const auto undo = [&]
    {
        for (const auto& f : written)
            f.deleteFile();
    };

    StickPlaylist newPlaylist;
    newPlaylist.name = options.playlistName;
    const auto total = (double) jobs.size() + 1.0;

    for (size_t i = 0; i < jobs.size(); ++i)
    {
        const auto& job = jobs[i];
        const auto& t = *job.track;

        if (! say ((double) i / total, (job.reuseId != 0 ? "Already on the stick: " : (job.copy ? "Copying " : "Converting "))
                                           + t.file.getFileName()))
        {
            undo();
            report.cancelled = true;
            report.error = "Cancelled. Nothing on the stick was changed.";
            return report;
        }

        if (job.reuseId != 0)
        {
            // Cues may have changed since: plan them again.
            for (auto& existing : library.tracks)
                if (existing.id == job.reuseId)
                {
                    auto cueOptions = options.cues;
                    const auto copied = existing.fileType == t.extension;
                    const auto o = offsetsFor (t, root.getChildFile (existing.path), existing.fileType, copied,
                                               options.mp3DelayCorrection);
                    cueOptions.offsetMs = o.markersMs;
                    existing.cues = planCues (t, cueOptions);
                    existing.bpm = t.bpm;
                    existing.firstBeatMs = t.bpm > 0.0 ? (double) samplesToMs (t.firstBeat, t.sampleRate) + o.gridMs : 0.0;
                }

            newPlaylist.trackIds.push_back (job.reuseId);
            ++report.tracksReused;
            continue;
        }

        const auto target = root.getChildFile (job.path);
        const auto part = target.getSiblingFile (target.getFileName() + ".part");
        juce::String error;

        if (! target.getParentDirectory().createDirectory())
        {
            undo();
            report.error = "Can't create a folder on the stick (write-protected?).";
            return report;
        }

        StickTrack st;
        st.fileType = job.type;
        double outRate = t.sampleRate;
        int bits = t.bitsPerSample, channels = juce::jmin (2, t.channels);
        auto cueOptions = options.cues;

        if (job.copy)
        {
            part.deleteFile();

            if (! t.file.copyFileTo (part))
                error = "Copying \"" + t.file.getFileName() + "\" failed (stick full or pulled out?).";

            ++report.tracksCopied;
        }
        else
        {
            juce::AudioBuffer<float> audio;
            double rate = 0.0;

            if (readWholeFile (t.file, audio, rate, error))
            {
                if (job.type == "wav")
                {
                    outRate = cdjRate (rate);
                    resample (audio, rate, outRate);
                    bits = (t.bitsPerSample == 16 && ! t.floatingPoint && ! t.compressed) ? 16 : 24;
                    writeWav (part, audio, outRate, bits, error);
                }
                else
                {
                    outRate = cdjRate (rate);
                    resample (audio, rate, outRate);
                    writeMp3 (part, audio, outRate, options.mp3Kbps, error);
                }

                channels = audio.getNumChannels();
            }

            ++report.tracksConverted;
        }

        if (error.isEmpty() && ! part.moveFileTo (target))
            error = "Can't rename \"" + part.getFileName() + "\" on the stick.";

        if (error.isNotEmpty())
        {
            part.deleteFile();
            undo();
            report.error = error + " Nothing on the stick was changed.";
            return report;
        }

        written.add (target);

        // MP3 delay: an MP3 starts with encoder + decoder delay a WAV doesn't have.
        const auto offsets = offsetsFor (t, target, job.type, job.copy, options.mp3DelayCorrection);
        cueOptions.offsetMs = offsets.markersMs;

        if (offsets.unknownDelay)
            report.warnings.add (t.file.getFileName() + ": the MP3 says nothing about its encoder delay, "
                                 "cues may sit a few milliseconds early.");

        st.id = library.nextId();
        st.path = job.path;
        st.title = t.title;
        st.artist = t.artist;
        st.seconds = t.seconds();
        st.sampleRate = (int) std::lround (outRate);
        st.bitsPerSample = job.type == "mp3" ? 16 : bits;
        st.channels = channels;
        st.bitrateKbps = job.type == "mp3" ? juce::jmax (1, mp3Bitrate (target))
                                           : (int) std::lround (outRate * st.bitsPerSample * channels / 1000.0);
        st.bytes = target.getSize();
        st.cues = planCues (t, cueOptions);
        st.bpm = t.bpm;
        st.firstBeatMs = t.bpm > 0.0 ? (double) samplesToMs (t.firstBeat, t.sampleRate) + offsets.gridMs : 0.0;

        if (t.bpm <= 0.0)
            report.warnings.add (t.file.getFileName() + ": no tempo, so no beat grid (sync and beat jumps won't work).");
        st.source = t.file.getFullPathName();
        st.sourceFingerprint = fingerprint (t.file);

        if (t.markers.empty() && t.regions.empty())
            report.warnings.add (t.file.getFileName() + ": no markers, so no cues.");

        library.tracks.push_back (st);
        newPlaylist.trackIds.push_back (st.id);
    }

    library.playlists.push_back (newPlaylist);
    library.writtenBy = "Adormis CDJ Export " + programVersion();
    library.writtenAt = juce::Time::getCurrentTime().toISO8601 (true);

    if (! say ((total - 1.0) / total, "Writing the CDJ library: waveforms, beat grids, cues"))
    {
        undo();
        report.cancelled = true;
        report.error = "Cancelled. Nothing on the stick was changed.";
        return report;
    }

    juce::String error;

    if (pioneerWriter != nullptr && ! pioneerWriter (library, root, error))
    {
        undo();
        report.error = "Writing the CDJ database failed: " + error;
        return report;
    }

    if (options.rekordboxXml)
    {
        report.xmlFile = root.getChildFile ("Adormis CDJ Export.xml");
        const auto xml = rekordboxXml (library, root, programVersion());

        if (! replaceFileSafely (report.xmlFile, juce::MemoryBlock (xml.toRawUTF8(), xml.getNumBytesAsUTF8()), error))
            report.warnings.add ("The rekordbox XML could not be written: " + error);
    }

    // The list of tracks goes last: it's what marks the stick as ours.
    if (! library.save (stickMarkerFile (root), error))
    {
        undo();
        report.error = error;
        return report;
    }

    // Replacing everything: the old files that are no longer listed go now.
    if (options.replaceEverything)
        for (const auto& old : previous.tracks)
        {
            const auto stillUsed = std::any_of (library.tracks.begin(), library.tracks.end(),
                                                [&] (const auto& t) { return t.path.equalsIgnoreCase (old.path); });

            if (! stillUsed)
                root.getChildFile (old.path).deleteFile();
        }

    say (1.0, "Done");
    report.ok = true;
    report.library = library;
    return report;
}
} // namespace cdj
