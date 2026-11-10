#include "ExportComponent.h"

#include "FormatDialog.h"
#include "Mp3.h"
#include "PioneerWriter.h"

namespace cdj
{
namespace
{
const juce::Colour background (0xff1d1f24), panel (0xff262931), ink (0xffe6e8ee), muted (0xff9aa0ad),
                   accent (0xff4aa3ff), warn (0xffffb347), bad (0xffff6b6b);

juce::String timeText (double seconds)
{
    const auto s = (int) std::floor (seconds);
    return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}
} // namespace

// The playlist: a list that takes audio files dropped from outside and rows
// dragged inside it to reorder.
class ExportComponent::PlaylistView : public juce::Component,
                                      public juce::FileDragAndDropTarget,
                                      public juce::DragAndDropTarget,
                                      private juce::ListBoxModel
{
public:
    explicit PlaylistView (ExportComponent& o) : owner (o)
    {
        list.setModel (this);
        list.setRowHeight (40);
        list.setMultipleSelectionEnabled (true);
        list.setColour (juce::ListBox::backgroundColourId, panel);
        list.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff3a3e48));
        list.setOutlineThickness (1);
        addAndMakeVisible (list);
    }

    void refresh()
    {
        list.updateContent();
        list.repaint();
    }

    juce::ListBox list;

    void resized() override { list.setBounds (getLocalBounds()); }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (owner.tracks.empty())
        {
            g.setColour (muted);
            g.setFont (juce::FontOptions (16.0f));
            g.drawFittedText ("Drag WAV or MP3 files here\n(markers become hot cues and memory cues)",
                              getLocalBounds().reduced (20), juce::Justification::centred, 3);
        }

        if (dropLine >= 0)
        {
            const auto y = list.getRowPosition (juce::jmin (dropLine, getNumRows()), false).getY()
                           - (dropLine >= getNumRows() ? -list.getRowHeight() : 0);
            g.setColour (accent);
            g.fillRect (2, juce::jlimit (0, getHeight() - 2, y - 1), getWidth() - 4, 3);
        }
    }

    // Files from the desktop / file manager.
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void fileDragEnter (const juce::StringArray&, int, int) override { highlight = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { highlight = false; repaint(); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        highlight = false;
        owner.addFiles (files);
    }

    // Rows dragged inside the list.
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description == "cdj-rows"; }

    void itemDragMove (const SourceDetails& d) override
    {
        dropLine = list.getInsertionIndexForPosition (d.localPosition.x, d.localPosition.y);
        repaint();
    }

    void itemDragExit (const SourceDetails&) override { dropLine = -1; repaint(); }

    void itemDropped (const SourceDetails& d) override
    {
        auto insertAt = list.getInsertionIndexForPosition (d.localPosition.x, d.localPosition.y);
        dropLine = -1;

        auto& t = owner.tracks;
        const auto selected = list.getSelectedRows();
        std::vector<Track> moving, staying;

        for (int i = 0; i < (int) t.size(); ++i)
        {
            if (selected.contains (i))
            {
                moving.push_back (t[(size_t) i]);
                if (i < insertAt)
                    --insertAt;
            }
            else
            {
                staying.push_back (t[(size_t) i]);
            }
        }

        insertAt = juce::jlimit (0, (int) staying.size(), insertAt);
        staying.insert (staying.begin() + insertAt, moving.begin(), moving.end());
        t = std::move (staying);

        refresh();
        owner.reattachEditor();
        list.selectRangeOfRows (insertAt, insertAt + (int) moving.size() - 1);
        owner.updateState();
    }

private:
    int getNumRows() override { return (int) owner.tracks.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) owner.tracks.size())
            return;

        const auto& t = owner.tracks[(size_t) row];
        g.fillAll (selected ? accent.withAlpha (0.25f) : (row % 2 == 0 ? panel : panel.brighter (0.03f)));

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (8, 3);
        g.setColour (muted);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (juce::String (row + 1), area.removeFromLeft (28), juce::Justification::centredLeft);

        auto right = area.removeFromRight (juce::jmin (260, width / 3));
        auto top = area.removeFromTop (area.getHeight() / 2);

        g.setColour (ink);
        g.setFont (juce::FontOptions (15.0f));
        const auto name = t.artist.isNotEmpty() ? t.artist + " - " + t.title : t.title;
        g.drawText (name, top, juce::Justification::centredLeft, true);

        g.setColour (muted);
        g.setFont (juce::FontOptions (12.5f));
        auto info = t.extension.toUpperCase() + "  " + timeText (t.seconds()) + "  "
                  + juce::String (t.sampleRate / 1000.0, 1) + " kHz";

        if (! t.compressed)
            info << "  " << t.bitsPerSample << (t.floatingPoint ? "-bit float" : "-bit");

        if (! t.notes.isEmpty())
            info << "   " << t.notes.joinIntoString ("; ");

        g.drawText (info, area, juce::Justification::centredLeft, true);

        g.setColour (t.markers.empty() && t.regions.empty() ? warn : ink.withAlpha (0.85f));
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (owner.cueSummary (t), right, juce::Justification::centredRight, 2);
    }

    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override { return "cdj-rows"; }

    void deleteKeyPressed (int) override { owner.removeSelected(); }
    void backgroundClicked (const juce::MouseEvent&) override { list.deselectAllRows(); }
    void selectedRowsChanged (int row) override
    {
        if (list.getNumSelectedRows() == 1)
            owner.showInEditor (row);

        owner.updateState();
    }

    void paint (juce::Graphics& g) override
    {
        if (highlight)
        {
            g.setColour (accent);
            g.drawRect (getLocalBounds(), 2);
        }
    }

    ExportComponent& owner;
    bool highlight = false;
    int dropLine = -1;
};

ExportComponent::ExportComponent()
{
    playlist = std::make_unique<PlaylistView> (*this);
    addAndMakeVisible (*playlist);

    editor.onChange = [this] { playlist->refresh(); updateState(); };
    editor.cueOptions = [this] { return currentOptions().cues; };
    addAndMakeVisible (editor);

    for (auto* l : { &playlistLabel, &stickLabel, &formatLabel })
    {
        l->setColour (juce::Label::textColourId, muted);
        addAndMakeVisible (*l);
    }

    playlistName.setText ("Adormis " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
    playlistName.setColour (juce::TextEditor::backgroundColourId, panel);
    playlistName.setColour (juce::TextEditor::textColourId, ink);
    playlistName.onTextChange = [this] { updateState(); };
    addAndMakeVisible (playlistName);

    addButton.onClick = [this] { chooseFiles(); };
    removeButton.onClick = [this] { removeSelected(); };
    upButton.onClick = [this] { moveSelected (-1); };
    downButton.onClick = [this] { moveSelected (1); };

    for (auto* b : { &addButton, &removeButton, &upButton, &downButton })
        addAndMakeVisible (*b);

    stickBox.setTextWhenNoChoicesAvailable ("No USB stick found - plug one in");
    stickBox.setTextWhenNothingSelected ("Choose a stick");
    stickBox.onChange = [this] { updateState(); };
    addAndMakeVisible (stickBox);

    formatButton.onClick = [this]
    {
        if (const auto* d = selectedDrive())
        {
            editor.stop();
            juce::Component::SafePointer<ExportComponent> safe (this);
            showFormatDialog (*this, *d, [safe] (juce::File)
            {
                if (safe != nullptr)
                {
                    safe->lastDriveSignature.clear();
                    safe->refreshDrives();
                }
            });
        }
    };
    addAndMakeVisible (formatButton);

    stickNote.setColour (juce::Label::textColourId, warn);
    stickNote.setJustificationType (juce::Justification::topLeft);
    stickNote.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (stickNote);
    addChildComponent (replaceToggle);
    replaceToggle.onClick = [this] { updateState(); };

    formatBox.addItem ("Keep the format (WAV and MP3 as they are, others become WAV)", 1);
    formatBox.addItem ("All WAV", 2);
    formatBox.addItem (mp3EncodingAvailable() ? "All MP3, 320 kbps" : "All MP3 (not in this version)", 3);
    formatBox.setItemEnabled (3, mp3EncodingAvailable());
    formatBox.setSelectedId (1, juce::dontSendNotification);
    formatBox.onChange = [this] { updateState(); };
    addAndMakeVisible (formatBox);

    for (int i = 1; i <= 8; ++i)
        hotCountBox.addItem (i == 1 ? "A" : "A-" + juce::String::charToString ((juce::juce_wchar) ('A' + i - 1)), i);

    hotCountBox.setSelectedId (8, juce::dontSendNotification);
    hotCountBox.onChange = [this] { updateState(); };
    addAndMakeVisible (hotCountBox);

    hotToggle.setToggleState (true, juce::dontSendNotification);
    memoryToggle.setToggleState (true, juce::dontSendNotification);
    loopsToggle.setToggleState (true, juce::dontSendNotification);
    mp3DelayToggle.setToggleState (true, juce::dontSendNotification);
    xmlToggle.setToggleState (true, juce::dontSendNotification);

    for (auto* t : { &hotToggle, &memoryToggle, &loopsToggle, &coloursToggle, &mp3DelayToggle, &xmlToggle, &replaceToggle })
    {
        t->setColour (juce::ToggleButton::textColourId, ink);
        t->setColour (juce::ToggleButton::tickColourId, accent);
        t->setColour (juce::ToggleButton::tickDisabledColourId, muted);

        if (t != &replaceToggle)
        {
            t->onClick = [this] { updateState(); };
            addAndMakeVisible (*t);
        }
    }

    exportButton.setColour (juce::TextButton::buttonColourId, accent);
    exportButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    exportButton.onClick = [this] { startExport(); };
    addAndMakeVisible (exportButton);

    statusLabel.setColour (juce::Label::textColourId, muted);
    addAndMakeVisible (statusLabel);

    refreshDrives();
    startTimer (1500);
    setSize (1180, 900);
}

ExportComponent::~ExportComponent()
{
    stopTimer();
}

void ExportComponent::paint (juce::Graphics& g)
{
    g.fillAll (background);
}

void ExportComponent::resized()
{
    auto area = getLocalBounds().reduced (14);

    auto row = area.removeFromTop (28);
    playlistLabel.setBounds (row.removeFromLeft (110));
    playlistName.setBounds (row.removeFromLeft (320));
    area.removeFromTop (8);

    auto bottom = area.removeFromBottom (300);
    auto top = area.removeFromTop (juce::jmax (150, area.getHeight() * 2 / 5));
    playlist->setBounds (top.withTrimmedBottom (40));
    auto buttons = top.removeFromBottom (32);
    area.removeFromTop (8);
    editor.setBounds (area);

    for (auto* b : { &addButton, &removeButton, &upButton, &downButton })
    {
        b->setBounds (buttons.removeFromLeft (110));
        buttons.removeFromLeft (8);
    }

    bottom.removeFromTop (10);
    row = bottom.removeFromTop (28);
    stickLabel.setBounds (row.removeFromLeft (110));
    formatButton.setBounds (row.removeFromRight (100));
    row.removeFromRight (8);
    stickBox.setBounds (row);
    stickNote.setBounds (bottom.removeFromTop (36).withTrimmedLeft (110));
    replaceToggle.setBounds (bottom.removeFromTop (26).withTrimmedLeft (106));

    bottom.removeFromTop (4);
    row = bottom.removeFromTop (28);
    formatLabel.setBounds (row.removeFromLeft (110));
    formatBox.setBounds (row.removeFromLeft (460));

    bottom.removeFromTop (8);
    auto cues = bottom.removeFromTop (88).withTrimmedLeft (106);
    auto left = cues.removeFromLeft (cues.getWidth() / 2);
    auto hotRow = left.removeFromTop (28);
    hotToggle.setBounds (hotRow.removeFromLeft (250));
    hotCountBox.setBounds (hotRow.removeFromLeft (80).reduced (0, 2));
    memoryToggle.setBounds (left.removeFromTop (28));
    loopsToggle.setBounds (left.removeFromTop (28));
    coloursToggle.setBounds (cues.removeFromTop (28));
    mp3DelayToggle.setBounds (cues.removeFromTop (28));
    xmlToggle.setBounds (cues.removeFromTop (28));

    bottom.removeFromTop (6);
    row = bottom.removeFromTop (40);
    exportButton.setBounds (row.removeFromRight (200));
    statusLabel.setBounds (row.withTrimmedRight (10));
}

void ExportComponent::addFiles (const juce::StringArray& paths)
{
    juce::StringArray problems;
    juce::Array<juce::File> files;

    for (const auto& p : paths)
    {
        juce::File f (p);

        if (f.isDirectory())
            files.addArray (f.findChildFiles (juce::File::findFiles, true, "*.wav;*.mp3;*.aif;*.aiff;*.flac"));
        else
            files.add (f);
    }

    for (const auto& f : files)
    {
        Track t;
        juce::String error;

        if (loadTrack (f, t, error))
            tracks.push_back (std::move (t));
        else
            problems.add (error);
    }

    reattachEditor();
    playlist->refresh();
    updateState();

    if (editor.getTrack() == nullptr && ! tracks.empty())
        playlist->list.selectRow ((int) tracks.size() - 1);

    if (! problems.isEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Some files were not added",
                                                problems.joinIntoString ("\n"));
}

void ExportComponent::chooseFiles()
{
    chooser = std::make_unique<juce::FileChooser> ("Add audio files", juce::File(), "*.wav;*.mp3;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
                          {
                              juce::StringArray paths;

                              for (const auto& f : fc.getResults())
                                  paths.add (f.getFullPathName());

                              addFiles (paths);
                          });
}

void ExportComponent::removeSelected()
{
    const auto selected = playlist->list.getSelectedRows();

    for (int i = (int) tracks.size(); --i >= 0;)
        if (selected.contains (i))
            tracks.erase (tracks.begin() + i);

    playlist->list.deselectAllRows();
    reattachEditor();
    playlist->refresh();
    updateState();
}

void ExportComponent::moveSelected (int delta)
{
    const auto row = playlist->list.getSelectedRow();
    const auto target = row + delta;

    if (row < 0 || target < 0 || target >= (int) tracks.size())
        return;

    std::swap (tracks[(size_t) row], tracks[(size_t) target]);
    reattachEditor();
    playlist->refresh();
    playlist->list.selectRow (target);
}

void ExportComponent::showInEditor (int row)
{
    if (row < 0 || row >= (int) tracks.size())
        return;

    editorFile = tracks[(size_t) row].file;
    editor.setTrack (&tracks[(size_t) row]);
    editor.grabKeyboardFocus();
}

void ExportComponent::reattachEditor()
{
    for (auto& t : tracks)
        if (t.file == editorFile)
        {
            editor.setTrack (&t);
            return;
        }

    editorFile = juce::File();
    editor.setTrack (nullptr);
}

bool ExportComponent::hasUnsavedMarkers() const
{
    return std::any_of (tracks.begin(), tracks.end(), [] (const auto& t) { return t.markersChanged; });
}

bool ExportComponent::saveAllMarkers (juce::String& errors)
{
    for (auto& t : tracks)
        if (t.markersChanged)
        {
            juce::String error;

            if (! saveMarkers (t, error))
                errors << error << "\n";
        }

    playlist->refresh();
    return errors.isEmpty();
}

bool ExportComponent::keyPressed (const juce::KeyPress& key)
{
    // Keys the playlist doesn't use go to the track editor.
    return editor.getTrack() != nullptr && editor.keyPressed (key);
}

void ExportComponent::timerCallback()
{
    refreshDrives();
}

void ExportComponent::refreshDrives()
{
    auto found = findDrives();
    juce::String signature;

    for (const auto& d : found)
        signature << d.root.getFullPathName() << "|" << (int) d.content << "|" << d.freeBytes / (1 << 20) << ";";

    if (signature == lastDriveSignature)
        return;

    lastDriveSignature = signature;
    const auto previous = selectedDrive() != nullptr ? selectedDrive()->root : juce::File();
    drives = std::move (found);

    stickBox.clear (juce::dontSendNotification);
    int select = 0;

    for (int i = 0; i < (int) drives.size(); ++i)
    {
        stickBox.addItem (drives[(size_t) i].describe(), i + 1);

        if (drives[(size_t) i].root == previous)
            select = i + 1;
    }

    // One stick: just pick it.
    if (select == 0 && drives.size() == 1)
        select = 1;

    stickBox.setSelectedId (select, juce::dontSendNotification);
    updateState();
}

const Drive* ExportComponent::selectedDrive() const
{
    const auto i = stickBox.getSelectedId() - 1;
    return i >= 0 && i < (int) drives.size() ? &drives[(size_t) i] : nullptr;
}

ExportOptions ExportComponent::currentOptions() const
{
    ExportOptions o;
    o.format = formatBox.getSelectedId() == 2 ? ExportOptions::Format::wav
             : formatBox.getSelectedId() == 3 ? ExportOptions::Format::mp3
                                              : ExportOptions::Format::keep;
    o.cues.hotCues = hotToggle.getToggleState();
    o.cues.hotCueCount = hotCountBox.getSelectedId();
    o.cues.memoryCues = memoryToggle.getToggleState();
    o.cues.regionsAsLoops = loopsToggle.getToggleState();
    o.cues.colours = coloursToggle.getToggleState();
    o.mp3DelayCorrection = mp3DelayToggle.getToggleState();
    o.rekordboxXml = xmlToggle.getToggleState();
    o.playlistName = playlistName.getText().trim();
    o.replaceEverything = replaceToggle.getToggleState();
    return o;
}

juce::String ExportComponent::cueSummary (const Track& t) const
{
    if (t.markers.empty() && t.regions.empty())
        return "no markers";

    const auto cues = planCues (t, currentOptions().cues);
    juce::String hot, text;
    int memory = 0;

    for (const auto& c : cues)
    {
        if (c.kind == Cue::Kind::hot)
            hot << c.hotLetter();
        else
            ++memory;
    }

    if (hot.isNotEmpty())
        text << "hot " << hot.substring (0, 1) << (hot.length() > 1 ? "-" + hot.getLastCharacters (1) : juce::String());

    if (memory > 0)
        text << (text.isEmpty() ? "" : ", ") << memory << " memory";

    const auto n = (int) (t.markers.size() + t.regions.size());
    return juce::String (n) + (n == 1 ? " marker" : " markers") + (text.isEmpty() ? juce::String() : ": " + text);
}

void ExportComponent::updateState()
{
    const auto* d = selectedDrive();
    juce::String note;

    if (d != nullptr)
    {
        note = d->fileSystemWarning();

        if (d->content == StickContent::rekordbox)
            note = "This stick has a rekordbox library. It won't be touched: use an empty stick, or Format... it "
                   "(that erases everything on it).";
    }

    stickNote.setText (note, juce::dontSendNotification);
    replaceToggle.setVisible (d != nullptr && d->content == StickContent::adormis);

    const auto canExport = ! tracks.empty() && d != nullptr && d->content != StickContent::rekordbox
                           && playlistName.getText().trim().isNotEmpty();
    exportButton.setEnabled (canExport);
    formatButton.setEnabled (d != nullptr);
    hotCountBox.setEnabled (hotToggle.getToggleState());

    const auto selected = playlist->list.getNumSelectedRows();
    removeButton.setEnabled (selected > 0);
    upButton.setEnabled (selected == 1);
    downButton.setEnabled (selected == 1);

    juce::String status;
    double seconds = 0.0;

    for (const auto& t : tracks)
        seconds += t.seconds();

    if (! tracks.empty())
        status << (int) tracks.size() << (tracks.size() == 1 ? " track, " : " tracks, ") << timeText (seconds);

    statusLabel.setText (status, juce::dontSendNotification);
    playlist->refresh();
}

void ExportComponent::startExport()
{
    if (selectedDrive() == nullptr || tracks.empty())
        return;

    editor.stop();

    if (! hasUnsavedMarkers())
    {
        runExport();
        return;
    }

    juce::Component::SafePointer<ExportComponent> safe (this);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("Save the markers first?")
                                      .withMessage ("Some markers were changed here. Save them into their files "
                                                    "before exporting? (The stick gets the new markers either way.)")
                                      .withButton ("Save and export")
                                      .withButton ("Export without saving")
                                      .withButton ("Cancel"),
                                  [safe] (int result)
                                  {
                                      if (safe == nullptr || result == 0)
                                          return;

                                      if (result == 1)
                                      {
                                          juce::String errors;

                                          if (! safe->saveAllMarkers (errors))
                                          {
                                              juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                                      "Markers not saved", errors);
                                              return;
                                          }
                                      }

                                      safe->runExport();
                                  });
}

void ExportComponent::runExport()
{
    const auto* d = selectedDrive();

    if (d == nullptr || tracks.empty())
        return;

    struct Job : public juce::ThreadWithProgressWindow
    {
        Job (ExportComponent& o, std::vector<Track> t, juce::File r, ExportOptions op)
            : ThreadWithProgressWindow ("Exporting to " + r.getFullPathName(), true, true),
              owner (o), tracks (std::move (t)), root (std::move (r)), options (std::move (op))
        {
        }

        void run() override
        {
            report = exportToStick (tracks, root, options, [this] (double f, const juce::String& s)
            {
                setProgress (f);
                setStatusMessage (s);
                return ! threadShouldExit();
            }, writePioneerLibrary);
        }

        void threadComplete (bool cancelled) override
        {
            juce::String text;

            if (report.ok)
            {
                text << "\"" << options.playlistName << "\" is on the stick: " << (int) tracks.size() << " tracks";

                if (report.tracksConverted > 0)
                    text << " (" << report.tracksConverted << " converted)";

                text << ".\n\nThe stick is ready for CDJs and rekordbox: playlists, cues, loops, beat grid and waveforms";

                if (report.xmlFile != juce::File())
                    text << ", plus \"" << report.xmlFile.getFileName() << "\" for rekordbox's XML import";

                text << ".";
            }
            else
            {
                text = cancelled ? juce::String ("Cancelled. Nothing on the stick was changed.") : report.error;
            }

            if (! report.warnings.isEmpty())
                text << "\n\nNotes:\n- " << report.warnings.joinIntoString ("\n- ");

            juce::AlertWindow::showMessageBoxAsync (report.ok ? juce::MessageBoxIconType::InfoIcon
                                                              : juce::MessageBoxIconType::WarningIcon,
                                                    report.ok ? "Export finished" : "Export not done", text);
            owner.lastDriveSignature.clear();
            owner.refreshDrives();
            delete this;
        }

        ExportComponent& owner;
        std::vector<Track> tracks;
        juce::File root;
        ExportOptions options;
        ExportReport report;
    };

    (new Job (*this, tracks, d->root, currentOptions()))->launchThread();
}
} // namespace cdj
