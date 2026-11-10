#include "Drives.h"

#if JUCE_WINDOWS
 #include <windows.h>
#elif JUCE_MAC
 #include <sys/mount.h>
 #include <sys/param.h>
#else
 #include <sys/statvfs.h>
#endif

namespace cdj
{
juce::File stickMarkerFile (const juce::File& root)
{
    return root.getChildFile ("PIONEER").getChildFile ("adormis-cdj-export.json");
}

StickContent inspectStick (const juce::File& root)
{
    const auto pioneer = root.getChildFile ("PIONEER");

    if (! pioneer.isDirectory())
        return StickContent::empty;

    if (stickMarkerFile (root).existsAsFile())
        return StickContent::adormis;

    // A PIONEER folder we didn't write. Even an empty-looking one may hold
    // rekordbox's settings or a library; leave it alone.
    return StickContent::rekordbox;
}

juce::String Drive::describe() const
{
    auto text = label.isNotEmpty() ? label : root.getFullPathName();
    text << "  (" << root.getFullPathName();

    if (fileSystem.isNotEmpty())
        text << ", " << fileSystem;

    if (totalBytes > 0)
        text << ", " << juce::File::descriptionOfSizeInBytes (freeBytes) << " free of "
             << juce::File::descriptionOfSizeInBytes (totalBytes);

    text << ")";

    switch (content)
    {
        case StickContent::adormis:   text << "  - written by Adormis before"; break;
        case StickContent::rekordbox: text << "  - has a rekordbox library"; break;
        case StickContent::empty:     break;
    }

    return text;
}

juce::String Drive::fileSystemWarning() const
{
    const auto fs = fileSystem.toLowerCase();

    if (fs.isEmpty() || fs.contains ("fat") || fs == "msdos" || fs == "vfat")
    {
        if (fs.contains ("exfat"))
            return "exFAT: newer CDJs read it, the first CDJ-2000 does not. FAT32 works on all of them.";

        return {};
    }

    if (fs.contains ("hfs"))
        return "Mac (HFS+) format: CDJ-2000 and newer read it, but FAT32 is the safest choice.";

    return "This stick is formatted as " + fileSystem + ", which CDJs can't read. Format it as FAT32 first.";
}

#if JUCE_WINDOWS
std::vector<Drive> findDrives()
{
    std::vector<Drive> drives;
    const auto mask = GetLogicalDrives();

    for (int i = 2; i < 26; ++i)    // skip A: and B:
    {
        if ((mask & (1u << i)) == 0)
            continue;

        const juce::String rootPath = juce::String::charToString ((juce::juce_wchar) ('A' + i)) + ":\\";

        if (GetDriveTypeW (rootPath.toWideCharPointer()) != DRIVE_REMOVABLE)
            continue;

        wchar_t name[MAX_PATH + 1] = {}, fs[MAX_PATH + 1] = {};

        if (! GetVolumeInformationW (rootPath.toWideCharPointer(), name, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH))
            continue;   // card reader with no card, etc.

        Drive d;
        d.root = juce::File (rootPath);
        d.label = juce::String (name);
        d.fileSystem = juce::String (fs);

        ULARGE_INTEGER freeBytes {}, totalBytes {};

        if (GetDiskFreeSpaceExW (rootPath.toWideCharPointer(), &freeBytes, &totalBytes, nullptr))
        {
            d.totalBytes = (juce::int64) totalBytes.QuadPart;
            d.freeBytes = (juce::int64) freeBytes.QuadPart;
        }

        d.content = inspectStick (d.root);
        drives.push_back (d);
    }

    return drives;
}

#elif JUCE_MAC
std::vector<Drive> findDrives()
{
    std::vector<Drive> drives;

    for (const auto& volume : juce::File ("/Volumes").findChildFiles (juce::File::findDirectories, false))
    {
        struct statfs info {};

        if (statfs (volume.getFullPathName().toRawUTF8(), &info) != 0)
            continue;

        // The system disk is mounted at "/" and appears in /Volumes as a link.
        if (juce::String (info.f_mntonname) == "/" || (info.f_flags & MNT_LOCAL) == 0)
            continue;

        Drive d;
        d.root = volume;
        d.label = volume.getFileName();
        d.fileSystem = juce::String (info.f_fstypename);
        d.totalBytes = (juce::int64) info.f_blocks * (juce::int64) info.f_bsize;
        d.freeBytes = (juce::int64) info.f_bavail * (juce::int64) info.f_bsize;
        d.content = inspectStick (d.root);
        drives.push_back (d);
    }

    return drives;
}

#else
namespace
{
// /dev/sdb1 → true when the whole disk is removable or sits on a USB bus.
bool isRemovableDevice (const juce::String& device)
{
    if (! device.startsWith ("/dev/"))
        return false;

    auto name = device.fromLastOccurrenceOf ("/", false, false);
    const auto partition = juce::File ("/sys/class/block").getChildFile (name);

    if (! partition.exists())
        return false;

    // A partition's sysfs folder sits inside its disk's.
    auto disk = partition.getLinkedTarget();

    if (disk.getChildFile ("partition").existsAsFile())
        disk = disk.getParentDirectory();

    if (disk.getChildFile ("removable").loadFileAsString().trim() == "1")
        return true;

    return disk.getFullPathName().contains ("/usb");
}

juce::String unescapeMount (const juce::String& s)
{
    // /proc/mounts writes spaces etc. as \040 (octal).
    juce::String out;

    for (int i = 0; i < s.length(); ++i)
    {
        if (s[i] == '\\' && i + 3 < s.length() && s.substring (i + 1, i + 4).containsOnly ("01234567"))
        {
            const auto code = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out += juce::String::charToString ((juce::juce_wchar) code);
            i += 3;
        }
        else
        {
            out += juce::String::charToString (s[i]);
        }
    }

    return out;
}
} // namespace

std::vector<Drive> findDrives()
{
    std::vector<Drive> drives;
    juce::StringArray lines;
    lines.addLines (juce::File ("/proc/self/mounts").loadFileAsString());

    for (const auto& line : lines)
    {
        const auto parts = juce::StringArray::fromTokens (line, " ", "");

        if (parts.size() < 3 || ! isRemovableDevice (parts[0]))
            continue;

        const auto mountPoint = unescapeMount (parts[1]);
        juce::File root (mountPoint);

        if (! root.isDirectory())
            continue;

        Drive d;
        d.root = root;
        d.label = root.getFileName();
        d.fileSystem = parts[2];

        struct statvfs info {};

        if (statvfs (mountPoint.toRawUTF8(), &info) == 0)
        {
            d.totalBytes = (juce::int64) info.f_blocks * (juce::int64) info.f_frsize;
            d.freeBytes = (juce::int64) info.f_bavail * (juce::int64) info.f_frsize;
        }

        d.content = inspectStick (d.root);
        drives.push_back (d);
    }

    return drives;
}
#endif
} // namespace cdj
