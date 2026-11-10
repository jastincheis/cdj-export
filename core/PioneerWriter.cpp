#include "PioneerWriter.h"

#include "Drives.h"
#include "PioneerAnlz.h"
#include "PioneerPdb.h"
#include "PioneerSettings.h"

#include <cmath>
#include <map>

namespace cdj
{
namespace
{
bool writeBytes (const juce::File& file, const pioneer::Bytes& bytes, juce::String& error)
{
    if (! file.getParentDirectory().createDirectory()
        || ! file.replaceWithData (bytes.data(), bytes.size())
        || file.getSize() != (juce::int64) bytes.size())
    {
        error = "Can't write \"" + file.getFileName() + "\" on the stick (full or pulled out?).";
        return false;
    }

    return true;
}

juce::String stickName (const juce::File& root)
{
    for (const auto& d : findDrives())
        if (d.root == root)
            return d.label;

    return {};
}

bool build (const StickLibrary& library, const juce::File& root, const juce::File& pioneer,
            const juce::File& fresh, juce::String& error)
{
    std::vector<const StickTrack*> tracks;

    for (const auto& t : library.tracks)
        tracks.push_back (&t);

    std::sort (tracks.begin(), tracks.end(), [] (auto* a, auto* b) { return a->id < b->id; });

    // Analysis files. Two tracks whose paths hash to the same folder are
    // numbered ANLZ0000, ANLZ0001… in track order, as rekordbox does.
    pioneer::PdbOptions options;
    options.exportDate = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");
    options.deviceName = stickName (root);
    std::map<juce::String, int> taken;

    for (const auto* t : tracks)
    {
        const auto usbPath = "/" + t->path;
        const auto audioFile = root.getChildFile (t->path);
        const auto folder = pioneer::anlzFolder (usbPath);
        const auto name = "ANLZ" + juce::String (taken[folder]++).paddedLeft ('0', 4);

        pioneer::Measured measured;
        juce::String unreadable;

        if (! pioneer::measureAudio (audioFile, measured, unreadable))
        {
            // A file that went missing from the stick (or can't be decoded
            // here) gets a flat waveform rather than stopping the export:
            // its grid and cues are still right.
            measured = {};
            measured.sampleRate = juce::jmax (1, t->sampleRate);
            measured.frames = (juce::int64) std::llround (t->seconds * measured.sampleRate);
            measured.columns.resize ((size_t) juce::jmax (1.0, std::ceil (t->seconds * 150.0)));
        }

        const auto frames = t->fileType == "mp3" ? pioneer::mp3AudioFrames (audioFile) : 0u;
        const auto files = pioneer::buildAnlzFiles (*t, usbPath, measured, frames);
        // folder is "/PIONEER/USBANLZ/…"; on the stick it goes into the new folder for now.
        const auto dir = fresh.getChildFile (folder.fromFirstOccurrenceOf ("/PIONEER/", false, false));

        if (! writeBytes (dir.getChildFile (name + ".DAT"), files.dat, error)
            || ! writeBytes (dir.getChildFile (name + ".EXT"), files.ext, error)
            || ! writeBytes (dir.getChildFile (name + ".2EX"), files.twoEx, error))
            return false;

        options.analyzePaths[t->id] = folder + "/" + name + ".DAT";
    }

    pioneer::Bytes pdb;

    if (! pioneer::buildExportPdb (library, options, pdb, error)
        || ! writeBytes (fresh.getChildFile ("rekordbox/export.pdb"), pdb, error))
        return false;

    // My Settings: rekordbox's defaults, or what a player saved on the stick.
    for (const auto& name : pioneer::settingsFileNames())
    {
        juce::MemoryBlock old;

        if (pioneer.getChildFile (name).loadFileAsData (old) && pioneer::savedByPlayer (old))
        {
            if (! writeBytes (fresh.getChildFile (name), pioneer::Bytes (static_cast<const std::uint8_t*> (old.getData()),
                                                                         static_cast<const std::uint8_t*> (old.getData()) + old.getSize()),
                              error))
                return false;
        }
        else if (! writeBytes (fresh.getChildFile (name), pioneer::defaultSettingsFile (name), error))
        {
            return false;
        }
    }

    // The list of tracks marks the stick as ours; the exporter rewrites it
    // after this, but the new folder must never be without it.
    const auto marker = stickMarkerFile (root);

    if (marker.existsAsFile() && ! marker.copyFileTo (fresh.getChildFile (marker.getFileName())))
    {
        error = "Can't copy \"" + marker.getFileName() + "\" on the stick.";
        return false;
    }

    return true;
}
} // namespace

bool writePioneerLibrary (const StickLibrary& library, const juce::File& root, juce::String& error)
{
    const auto pioneer = root.getChildFile ("PIONEER");
    const auto fresh = root.getChildFile ("PIONEER.adormis-new");
    const auto old = root.getChildFile ("PIONEER.adormis-old");

    // Leftovers of an export that was interrupted.
    fresh.deleteRecursively();

    if (pioneer.isDirectory())
        old.deleteRecursively();

    if (! fresh.createDirectory())
    {
        error = "Can't create a folder on the stick (write-protected?).";
        return false;
    }

    if (! build (library, root, pioneer, fresh, error))
    {
        fresh.deleteRecursively();
        return false;
    }

    // The swap: two renames, so the stick always has one whole library.
    if (pioneer.isDirectory() && ! pioneer.moveFileTo (old))
    {
        fresh.deleteRecursively();
        error = "Can't replace the PIONEER folder on the stick (a file in it is open?).";
        return false;
    }

    if (! fresh.moveFileTo (pioneer))
    {
        if (old.isDirectory())
            old.moveFileTo (pioneer);

        fresh.deleteRecursively();
        error = "Can't put the new PIONEER folder in place on the stick.";
        return false;
    }

    old.deleteRecursively();
    return true;
}
} // namespace cdj
