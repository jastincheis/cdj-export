#include "Drives.h"

#if JUCE_WINDOWS
 #include <windows.h>
bool ejectDrive (const Drive&, juce::String& error)
{
    error = "On Windows use \"Safely Remove Hardware\" (the USB icon by the clock) before pulling the stick out.";
    return false;
}

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

bool sameDrive (const Drive& a, const Drive& b)
{
    return a.mounted == b.mounted && (a.mounted ? a.root == b.root : a.device == b.device);
}

juce::String Drive::describe() const
{
    if (! mounted)
    {
        auto text = (label.isNotEmpty() ? label : juce::String ("USB stick")) + "  (" + device;

        if (totalBytes > 0)
            text << ", " << juce::File::descriptionOfSizeInBytes (totalBytes);

        return text + ")  - not mounted";
    }

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

bool mountDrive (Drive& drive, juce::String& error)
{
    if (drive.mounted)
        return true;

    error = "Windows mounts sticks by itself; if it doesn't show a drive letter, it needs Format...";
    return false;
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

bool mountDrive (Drive& drive, juce::String& error)
{
    if (drive.mounted)
        return true;

    error = "macOS mounts sticks by itself; if it doesn't appear, it needs Format... (or Disk Utility).";
    return false;
}

bool ejectDrive (const Drive& drive, juce::String& error)
{
    juce::ChildProcess p;
    juce::String out;

    if (p.start (juce::StringArray { "diskutil", "eject", drive.root.getFullPathName() }))
    {
        out = p.readAllProcessOutput();
        p.waitForProcessToFinish (60000);

        if (p.getExitCode() == 0)
            return true;
    }

    error = "The stick could not be ejected (a file on it still open?):\n" + out.trim();
    return false;
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

    // USB sticks with nothing mounted: still listed, to mount or format them.
    for (const auto& disk : juce::File ("/sys/block").findChildFiles (juce::File::findDirectories | juce::File::ignoreHiddenFiles, false))
    {
        const auto name = disk.getFileName();
        const auto device = "/dev/" + name;
        const auto bytes = disk.getChildFile ("size").loadFileAsString().trim().getLargeIntValue() * 512;

        if (bytes <= 0 || ! isRemovableDevice (device))
            continue;

        const auto anyMounted = std::any_of (lines.begin(), lines.end(), [&] (const auto& line)
                                             { return line.startsWith (device + " ") || line.startsWith (device); });

        if (anyMounted)
            continue;

        Drive d;
        d.mounted = false;
        d.device = device;
        d.totalBytes = bytes;
        d.label = (disk.getLinkedTarget().getChildFile ("device/vendor").loadFileAsString().trim() + " "
                   + disk.getLinkedTarget().getChildFile ("device/model").loadFileAsString().trim()).trim();

        // A volume name, when a partition has one.
        for (const auto& link : juce::File ("/dev/disk/by-label").findChildFiles (juce::File::findFiles, false))
            if (link.getLinkedTarget().getFileName().startsWith (name))
            {
                d.label = link.getFileName().replace ("\\x20", " ");
                break;
            }

        drives.push_back (d);
    }

    return drives;
}

bool mountDrive (Drive& drive, juce::String& error)
{
    if (drive.mounted)
        return true;

    // The first partition (or the whole stick) with a file system a computer mounts.
    juce::ChildProcess list;
    juce::String output;

    if (list.start (juce::StringArray { "lsblk", "-nrbpo", "NAME,FSTYPE,SIZE", drive.device }))
        output = list.readAllProcessOutput();

    // The biggest one: an installer's small boot partition is not the stick's storage.
    static const juce::StringArray mountable { "vfat", "exfat", "hfsplus", "ntfs", "ntfs3" };
    juce::String partition;
    juce::int64 biggest = 0;

    for (const auto& line : juce::StringArray::fromLines (output))
    {
        const auto parts = juce::StringArray::fromTokens (line, " ", "");

        if (parts.size() >= 3 && mountable.contains (parts[1]) && parts[2].getLargeIntValue() > biggest)
        {
            biggest = parts[2].getLargeIntValue();
            partition = parts[0];
        }
    }

    // Only a small piece of the stick (an installer's boot partition): not usable storage.
    if (partition.isNotEmpty() && drive.totalBytes > 0 && biggest < drive.totalBytes / 4)
        partition.clear();

    if (partition.isEmpty())
    {
        error = "This stick has nothing a computer or a CDJ can read (for example an installer). "
                "Use Format... to make it a FAT32 stick.";
        return false;
    }

    juce::ChildProcess mount;
    juce::String result;

    if (mount.start (juce::StringArray { "udisksctl", "mount", "-b", partition }))
    {
        result = mount.readAllProcessOutput();
        mount.waitForProcessToFinish (60000);
    }

    // "Mounted /dev/sdb1 at /run/media/user/NAME"
    if (! result.contains (" at "))
    {
        error = "The stick could not be mounted:\n" + result.trim();
        return false;
    }

    drive.root = juce::File (result.fromLastOccurrenceOf (" at ", false, false).trim().trimCharactersAtEnd ("."));
    drive.mounted = true;
    return true;
}

bool ejectDrive (const Drive& drive, juce::String& error)
{
    juce::StringArray lines;
    lines.addLines (juce::File ("/proc/self/mounts").loadFileAsString());

    // Which disk: the device itself, or the one holding the mount folder.
    juce::String device = drive.device;

    if (drive.mounted)
        for (const auto& line : lines)
        {
            const auto parts = juce::StringArray::fromTokens (line, " ", "");

            if (parts.size() >= 2 && juce::File (unescapeMount (parts[1])) == drive.root)
                device = parts[0];
        }

    if (device.isEmpty() || ! isRemovableDevice (device))
    {
        error = "Can't find this stick (already pulled out?).";
        return false;
    }

    auto sys = juce::File ("/sys/class/block").getChildFile (device.fromLastOccurrenceOf ("/", false, false)).getLinkedTarget();

    if (sys.getChildFile ("partition").existsAsFile())
        sys = sys.getParentDirectory();

    const auto disk = "/dev/" + sys.getFileName();

    auto run = [] (const juce::StringArray& args, juce::String& out)
    {
        juce::ChildProcess p;

        if (! p.start (args))
            return false;

        out = p.readAllProcessOutput();
        p.waitForProcessToFinish (120000);
        return p.getExitCode() == 0;
    };

    juce::String out;
    run ({ "sync" }, out);

    // Unmount every partition of the stick (no password needed for USB sticks).
    for (const auto& line : lines)
    {
        const auto parts = juce::StringArray::fromTokens (line, " ", "");

        if (parts.size() >= 2 && (parts[0] == disk || (parts[0].startsWith (disk)
                                   && juce::File ("/sys/class/block").getChildFile (parts[0].fromLastOccurrenceOf ("/", false, false))
                                          .getLinkedTarget().getParentDirectory() == sys)))
            if (! run ({ "udisksctl", "unmount", "-b", parts[0] }, out))
            {
                error = "The stick can't be unmounted (a file on it still open?):\n" + out.trim();
                return false;
            }
    }

    // Power it off: the light goes out and it can be pulled.
    if (! run ({ "udisksctl", "power-off", "-b", disk }, out))
    {
        error = "The stick was unmounted, so it is safe to pull out, but it could not be powered off:\n" + out.trim();
        return false;
    }

    return true;
}
#endif
} // namespace cdj
