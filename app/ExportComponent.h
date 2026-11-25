#pragma once

#include "../core/Drives.h"
#include "../core/Exporter.h"
#include "TrackEditor.h"

#include <juce_gui_extra/juce_gui_extra.h>

// The whole "Export for CDJ" window: playlist, stick, format, cue options and
// the Export button. Standalone app and Adormis Audio both show this.
namespace cdj
{
class ExportComponent : public juce::Component,
                        public juce::DragAndDropContainer,
                        private juce::Timer
{
public:
    ExportComponent();
    ~ExportComponent() override;

    void addFiles (const juce::StringArray& paths);

    // Markers edited here and not yet saved into their files.
    bool hasUnsavedMarkers() const;
    // Saves them all; false (with the reasons) if any failed.
    bool saveAllMarkers (juce::String& errors);

    bool keyPressed (const juce::KeyPress&) override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class PlaylistView;
    friend class PlaylistView;

    void timerCallback() override;
    void refreshDrives();
    void updateState();
    void chooseFiles();
    void removeSelected();
    void moveSelected (int delta);
    void startExport();
    const Drive* selectedDrive() const;
    ExportOptions currentOptions() const;
    juce::String cueSummary (const Track& t) const;
    void showInEditor (int row);
    void reattachEditor();      // after the track list changed (its storage may have moved)
    void runExport();

    std::vector<Track> tracks;
    std::vector<Drive> drives;

    juce::Label playlistLabel { {}, "Playlist name" };
    juce::TextEditor playlistName;
    std::unique_ptr<PlaylistView> playlist;
    TrackEditor editor;
    juce::File editorFile;
    juce::TextButton addButton { "Add files..." }, removeButton { "Remove" }, upButton { "Move up" },
                     downButton { "Move down" };

    juce::Label stickLabel { {}, "Stick" };
    juce::ComboBox stickBox;
    juce::TextButton formatButton { "Format..." }, mountButton { "Mount" }, refreshButton { "Refresh" }, ejectButton { "Eject" };
    juce::File justMounted;     // select this stick at the next refresh
    juce::Label stickNote;
    juce::ToggleButton replaceToggle { "Replace everything on this stick (otherwise this playlist is added to it)" };

    juce::Label forLabel { {}, "For" };
    juce::ToggleButton rekordboxToggle { "rekordbox / Pioneer CDJ" };
    juce::ToggleButton seratoToggle { "Serato DJ" };

    juce::Label formatLabel { {}, "Files on the stick" };
    juce::ComboBox formatBox;

    juce::ToggleButton hotToggle { "Hot cues from the first markers" };
    juce::ComboBox hotCountBox;
    juce::ToggleButton memoryToggle { "Memory cues from all markers" };
    juce::ToggleButton loopsToggle { "Regions become loops" };
    juce::ToggleButton coloursToggle { "Coloured hot cues" };
    juce::ToggleButton mp3DelayToggle { "Shift MP3 cues by the encoder delay" };
    juce::ToggleButton xmlToggle { "Also write a rekordbox XML on the stick" };

    juce::TextButton exportButton { "Export to stick" };
    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::String lastDriveSignature;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportComponent)
};
} // namespace cdj
