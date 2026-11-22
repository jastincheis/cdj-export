#include "Format.h"

namespace cdj
{
namespace
{
struct Run
{
    int exitCode = -1;
    juce::String output;
};

Run run (const juce::StringArray& args, int timeoutMs = 120000)
{
    Run r;
    juce::ChildProcess p;

    if (! p.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        r.output = "Could not start " + args[0];
        return r;
    }

    r.output = p.readAllProcessOutput();

    if (! p.waitForProcessToFinish (timeoutMs))
    {
        p.kill();
        r.output << "\n(timed out)";
        return r;
    }

    r.exitCode = (int) p.getExitCode();
    return r;
}
} // namespace

juce::String cleanVolumeLabel (const juce::String& label)
{
    auto s = label.toUpperCase().retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-").substring (0, 11);
    return s.isNotEmpty() ? s : juce::String ("ADORMIS");
}

juce::String formatLimitation (const FormatPlan& plan, FileSystem fs)
{
   #if JUCE_WINDOWS
    constexpr juce::int64 limit = 32LL * 1024 * 1024 * 1024;

    if (fs == FileSystem::fat32 && plan.diskBytes > limit)
        return "Windows itself can't make FAT32 on sticks bigger than 32 GB, and this version doesn't do it "
               "its own way yet. Choose exFAT (newer CDJs only), or format this stick as FAT32 on a Mac.";
   #else
    juce::ignoreUnused (plan, fs);
   #endif

    // MBR can't describe more than 2 TiB.
    if (plan.diskBytes > 2199023255552LL)
        return "This drive is bigger than 2 TB: CDJs can't use it.";

    return {};
}

//==============================================================================
#if JUCE_LINUX || JUCE_BSD
namespace
{
juce::File sysBlock (const juce::String& name)
{
    return juce::File ("/sys/class/block").getChildFile (name);
}

// "sdb1" → "sdb"; a stick formatted without partitions ("superfloppy") is its own disk.
juce::String diskOf (const juce::String& partition)
{
    const auto target = sysBlock (partition).getLinkedTarget();
    return target.getChildFile ("partition").existsAsFile() ? target.getParentDirectory().getFileName() : partition;
}

juce::String firstPartition (const juce::String& disk)
{
    // mmcblk0 → mmcblk0p1, nvme0n1 → nvme0n1p1, sdb → sdb1
    return "/dev/" + disk + (juce::CharacterFunctions::isDigit (disk.getLastCharacter()) ? "p1" : "1");
}

juce::StringArray mountLines()
{
    juce::StringArray lines;
    lines.addLines (juce::File ("/proc/self/mounts").loadFileAsString());
    return lines;
}

juce::String unescape (const juce::String& s)
{
    return s.replace ("\\040", " ").replace ("\\011", "\t").replace ("\\134", "\\");
}

juce::int64 diskSize (const juce::String& disk)
{
    return sysBlock (disk).getChildFile ("size").loadFileAsString().trim().getLargeIntValue() * 512;
}
} // namespace

bool planFormat (const Drive& drive, FormatPlan& plan, juce::String& error)
{
    plan = {};
    plan.drive = drive;

    if (! drive.mounted)
    {
        plan.partition = drive.device;      // the whole stick
    }
    else
    {
        for (const auto& line : mountLines())
        {
            const auto parts = juce::StringArray::fromTokens (line, " ", "");

            if (parts.size() >= 2 && juce::File (unescape (parts[1])) == drive.root && parts[0].startsWith ("/dev/"))
                plan.partition = parts[0];
        }
    }

    if (plan.partition.isEmpty())
    {
        error = "Can't find which device \"" + drive.root.getFullPathName() + "\" is.";
        return false;
    }

    const auto disk = diskOf (plan.partition.fromLastOccurrenceOf ("/", false, false));
    const auto diskFolder = sysBlock (disk).getLinkedTarget();
    const auto removable = diskFolder.getChildFile ("removable").loadFileAsString().trim() == "1"
                           || diskFolder.getFullPathName().contains ("/usb");

    if (! removable)
    {
        error = "\"" + (drive.mounted ? drive.root.getFullPathName() : drive.device) + "\" is not a USB stick, so it won't be formatted.";
        return false;
    }

    // Never a disk that holds anything the system runs from.
    static const juce::StringArray systemPlaces { "/", "/boot", "/boot/efi", "/efi", "/home", "/usr", "/var", "/opt" };

    for (const auto& line : mountLines())
    {
        const auto parts = juce::StringArray::fromTokens (line, " ", "");

        if (parts.size() >= 2 && parts[0].startsWith ("/dev/" + disk) && systemPlaces.contains (unescape (parts[1])))
        {
            error = "That disk holds part of the system (" + unescape (parts[1]) + "). It won't be formatted.";
            return false;
        }
    }

    plan.disk = "/dev/" + disk;
    plan.diskBytes = diskSize (disk);
    plan.model = (diskFolder.getChildFile ("device/vendor").loadFileAsString().trim() + " "
                  + diskFolder.getChildFile ("device/model").loadFileAsString().trim()).trim();

    if (plan.diskBytes <= 0)
    {
        error = "Can't read the size of " + plan.disk + ".";
        return false;
    }

    return true;
}

FormatResult formatStick (const FormatPlan& plan, FileSystem fs, const juce::String& rawLabel,
                          const std::function<void (const juce::String&)>& status)
{
    FormatResult result;
    const auto label = cleanVolumeLabel (rawLabel);
    const auto disk = plan.disk.fromLastOccurrenceOf ("/", false, false);

    if (diskSize (disk) != plan.diskBytes)
    {
        result.error = "The stick changed since it was chosen (unplugged?). Nothing was erased.";
        return result;
    }

    // Unmount every partition of the stick (no password needed for USB sticks).
    status ("Unmounting the stick");

    for (const auto& line : mountLines())
    {
        const auto parts = juce::StringArray::fromTokens (line, " ", "");

        if (parts.size() >= 2 && parts[0].startsWith (plan.disk))
        {
            const auto r = run ({ "udisksctl", "unmount", "-b", parts[0] });

            if (r.exitCode != 0)
            {
                result.error = "The stick can't be unmounted (a file on it still open?):\n" + r.output.trim();
                return result;
            }
        }
    }

    const auto partition = firstPartition (disk);
    const auto type = fs == FileSystem::fat32 ? "c" : "7";
    const auto mkfs = fs == FileSystem::fat32 ? "mkfs.fat -F 32 -n '" + label + "' " + partition
                                              : "mkfs.exfat -L '" + label + "' " + partition;

    // One MBR partition across the whole stick, then the file system.
    const auto script = "set -e; wipefs -a -f " + plan.disk + "; printf 'label: dos\\n,,"
                      + juce::String (type) + "\\n' | sfdisk --wipe always --wipe-partitions always " + plan.disk
                      + "; udevadm settle; " + mkfs + "; sync";

    status ("Formatting (asks for your password)");
    const auto r = run ({ "pkexec", "/bin/sh", "-c", script }, 600000);

    if (r.exitCode != 0)
    {
        result.error = r.exitCode == 126 || r.exitCode == 127
                           ? "The password was not given (or no password window could open), so nothing was erased."
                           : "Formatting failed:\n" + r.output.trim();
        result.manualCommand = "sudo sh -c \"" + script.replace ("\"", "\\\"") + "\"";
        return result;
    }

    status ("Mounting the stick again");
    const auto m = run ({ "udisksctl", "mount", "-b", partition });

    // "Mounted /dev/sdb1 at /run/media/user/ADORMIS"
    if (m.exitCode == 0 && m.output.contains (" at "))
        result.newRoot = juce::File (m.output.fromLastOccurrenceOf (" at ", false, false).trim().trimCharactersAtEnd ("."));

    result.ok = true;
    return result;
}

//==============================================================================
#elif JUCE_MAC
namespace
{
std::unique_ptr<juce::XmlElement> diskutilInfo (const juce::String& what)
{
    const auto r = run ({ "diskutil", "info", "-plist", what });
    return r.exitCode == 0 ? juce::parseXML (r.output) : nullptr;
}

// Value after <key>name</key> in a flat plist dictionary.
juce::String plistValue (const juce::XmlElement* plist, const juce::String& key)
{
    if (plist == nullptr)
        return {};

    const auto* dict = plist->getChildByName ("dict");

    for (auto* e = dict != nullptr ? dict->getFirstChildElement() : nullptr; e != nullptr; e = e->getNextElement())
        if (e->hasTagName ("key") && e->getAllSubText() == key)
            if (auto* v = e->getNextElement())
                return v->hasTagName ("true") ? "true" : v->hasTagName ("false") ? "false" : v->getAllSubText();

    return {};
}
} // namespace

bool planFormat (const Drive& drive, FormatPlan& plan, juce::String& error)
{
    plan = {};
    plan.drive = drive;
    const auto volume = diskutilInfo (drive.root.getFullPathName());
    const auto whole = plistValue (volume.get(), "ParentWholeDisk");

    if (whole.isEmpty())
    {
        error = "Can't find which disk \"" + drive.root.getFullPathName() + "\" is.";
        return false;
    }

    const auto disk = diskutilInfo (whole);
    const auto internal = plistValue (disk.get(), "Internal") == "true";
    const auto protocol = plistValue (disk.get(), "BusProtocol");
    const auto system = diskutilInfo ("/");

    if (internal || whole == plistValue (system.get(), "ParentWholeDisk") || (protocol != "USB" && plistValue (disk.get(), "Removable") != "true"))
    {
        error = "\"" + drive.label + "\" is not a USB stick, so it won't be formatted.";
        return false;
    }

    plan.disk = "/dev/" + whole;
    plan.partition = plistValue (volume.get(), "DeviceIdentifier");
    plan.diskBytes = plistValue (disk.get(), "TotalSize").getLargeIntValue();
    plan.model = plistValue (disk.get(), "MediaName");
    return plan.diskBytes > 0;
}

FormatResult formatStick (const FormatPlan& plan, FileSystem fs, const juce::String& rawLabel,
                          const std::function<void (const juce::String&)>& status)
{
    FormatResult result;
    const auto label = cleanVolumeLabel (rawLabel);
    const auto now = diskutilInfo (plan.disk);

    if (plistValue (now.get(), "TotalSize").getLargeIntValue() != plan.diskBytes || plistValue (now.get(), "Internal") == "true")
    {
        result.error = "The stick changed since it was chosen (unplugged?). Nothing was erased.";
        return result;
    }

    status ("Formatting");
    const auto r = run ({ "diskutil", "eraseDisk", fs == FileSystem::fat32 ? "FAT32" : "ExFAT", label, "MBRFormat", plan.disk },
                        600000);

    if (r.exitCode != 0)
    {
        result.error = "Formatting failed:\n" + r.output.trim();
        return result;
    }

    result.ok = true;
    result.newRoot = juce::File ("/Volumes").getChildFile (label);
    return result;
}

//==============================================================================
#elif JUCE_WINDOWS
namespace
{
juce::String letterOf (const Drive& drive)
{
    return drive.root.getFullPathName().substring (0, 1).toUpperCase();
}

Run powershell (const juce::String& command)
{
    return run ({ "powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command });
}
} // namespace

bool planFormat (const Drive& drive, FormatPlan& plan, juce::String& error)
{
    plan = {};
    plan.drive = drive;
    const auto letter = letterOf (drive);

    if (! juce::CharacterFunctions::isLetter (letter[0]))
    {
        error = "Can't tell which drive letter the stick has.";
        return false;
    }

    const auto r = powershell ("$p = Get-Partition -DriveLetter " + letter + "; $d = Get-Disk -Number $p.DiskNumber; "
                               "Write-Output (\"{0}|{1}|{2}|{3}|{4}|{5}\" -f $d.Number, $d.BusType, $d.IsBoot, $d.IsSystem, $d.Size, $d.FriendlyName)");
    const auto fields = juce::StringArray::fromTokens (r.output.trim(), "|", "");

    if (r.exitCode != 0 || fields.size() < 6)
    {
        error = "Windows didn't say which disk " + letter + ": is:\n" + r.output.trim();
        return false;
    }

    if (fields[1] != "USB" || fields[2] == "True" || fields[3] == "True")
    {
        error = letter + ": is not a USB stick (" + fields[1] + "), so it won't be formatted.";
        return false;
    }

    plan.disk = fields[0];
    plan.partition = letter + ":";
    plan.diskBytes = fields[4].getLargeIntValue();
    plan.model = fields[5];
    return plan.diskBytes > 0;
}

FormatResult formatStick (const FormatPlan& plan, FileSystem fs, const juce::String& rawLabel,
                          const std::function<void (const juce::String&)>& status)
{
    FormatResult result;
    const auto label = cleanVolumeLabel (rawLabel);
    const auto fsName = fs == FileSystem::fat32 ? "FAT32" : "exFAT";
    const auto mbrType = fs == FileSystem::fat32 ? "FAT32" : "IFS";
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto log = temp.getChildFile ("adormis-cdj-format.log");
    const auto scriptFile = temp.getChildFile ("adormis-cdj-format.ps1");
    log.deleteFile();

    // Runs as administrator. It checks the disk again (USB, not the system,
    // same size) before erasing anything.
    juce::String s;
    s << "$ErrorActionPreference = 'Stop'\r\n"
      << "$log = '" << log.getFullPathName() << "'\r\n"
      << "try {\r\n"
      << "  $d = Get-Disk -Number " << plan.disk << "\r\n"
      << "  if ($d.BusType -ne 'USB' -or $d.IsBoot -or $d.IsSystem -or $d.Size -ne " << plan.diskBytes << ") {\r\n"
      << "    throw 'The stick changed or is not a USB stick. Nothing was erased.' }\r\n"
      << "  $parts = @(Get-Partition -DiskNumber " << plan.disk << " -ErrorAction SilentlyContinue)\r\n"
      << "  if ($d.PartitionStyle -ne 'MBR' -or $parts.Count -ne 1) {\r\n"
      << "    Clear-Disk -Number " << plan.disk << " -RemoveData -RemoveOEM -Confirm:$false\r\n"
      << "    Initialize-Disk -Number " << plan.disk << " -PartitionStyle MBR -ErrorAction SilentlyContinue\r\n"
      << "    $part = New-Partition -DiskNumber " << plan.disk << " -UseMaximumSize -AssignDriveLetter -MbrType " << mbrType << "\r\n"
      << "  } else { $part = $parts[0] }\r\n"
      << "  Format-Volume -Partition $part -FileSystem " << fsName << " -NewFileSystemLabel '" << label
      << "' -Confirm:$false -Force | Out-Null\r\n"
      << "  $letter = (Get-Partition -DiskNumber " << plan.disk << " | Where-Object DriveLetter | Select-Object -First 1).DriveLetter\r\n"
      << "  Set-Content -Path $log -Value \"OK $letter\" -Encoding UTF8\r\n"
      << "} catch { Set-Content -Path $log -Value (\"ERROR \" + $_.Exception.Message) -Encoding UTF8 }\r\n";
    scriptFile.replaceWithText (s);

    status ("Formatting (Windows asks for permission)");
    const auto r = powershell ("Start-Process powershell.exe -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList "
                               "'-NoProfile','-ExecutionPolicy','Bypass','-File','\"" + scriptFile.getFullPathName() + "\"'");
    scriptFile.deleteFile();
    const auto outcome = log.loadFileAsString().trim();
    log.deleteFile();

    if (outcome.startsWith ("OK"))
    {
        result.ok = true;
        const auto letter = outcome.substring (2).trim();

        if (letter.isNotEmpty())
            result.newRoot = juce::File (letter + ":\\");

        return result;
    }

    result.error = outcome.startsWith ("ERROR") ? "Formatting failed: " + outcome.substring (6)
                 : r.exitCode != 0 ? juce::String ("Permission was not given, so nothing was erased.")
                                   : "Formatting didn't report back: " + r.output.trim();
    return result;
}
#endif
} // namespace cdj
