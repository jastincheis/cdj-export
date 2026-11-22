#include "AudioBrowser.h"

#include "Track.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace cdj
{
namespace
{
const juce::Colour background (0xff1d1f24), panel (0xff262931), ink (0xffe6e8ee), muted (0xff9aa0ad),
                   accent (0xff4cd964);

const juce::String audioPatterns ("*.wav;*.mp3;*.aif;*.aiff;*.flac");

juce::File& lastFolder()
{
    static juce::File folder;
    return folder;
}

juce::File startFolder()
{
    if (lastFolder().isDirectory())
        return lastFolder();

    const auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    return music.isDirectory() ? music : juce::File::getSpecialLocation (juce::File::userHomeDirectory);
}

juce::String timeText (double seconds)
{
    const auto s = juce::jmax (0, (int) seconds);
    return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}

//==============================================================================
// Right-hand panel of the browser: what the highlighted file is, and its sound.
class AudioPreview : public juce::FilePreviewComponent,
                     private juce::Timer
{
public:
    AudioPreview()
    {
        name.setColour (juce::Label::textColourId, ink);
        name.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        name.setJustificationType (juce::Justification::topLeft);
        info.setColour (juce::Label::textColourId, muted);
        info.setJustificationType (juce::Justification::topLeft);
        hint.setColour (juce::Label::textColourId, muted);
        hint.setJustificationType (juce::Justification::bottomLeft);
        hint.setText ("Click a track to hear it.\nSpace: play / stop\nClick the bar to jump\n\n"
                      "Left: parent folder   Right: open folder\nEnter: add", juce::dontSendNotification);

        playButton.onClick = [this] { toggle(); };
        autoPlay.setToggleState (true, juce::dontSendNotification);
        autoPlay.setColour (juce::ToggleButton::textColourId, ink);

        for (auto* c : std::initializer_list<juce::Component*> { &name, &info, &hint, &playButton, &autoPlay })
            addAndMakeVisible (c);

        readAhead.startThread();
        update();
    }

    ~AudioPreview() override
    {
        stopTimer();
        transport.stop();

        if (audioReady)
            devices.removeAudioCallback (&player);

        player.setSource (nullptr);
        transport.setSource (nullptr);
        source = nullptr;
        readAhead.stopThread (2000);
    }

    void selectedFileChanged (const juce::File& f) override
    {
        if (f == file)
            return;

        transport.stop();
        transport.setSource (nullptr);
        source = nullptr;
        file = juce::File();
        length = 0.0;

        if (f.existsAsFile() && f.hasFileExtension (audioPatterns.removeCharacters ("*")))
        {
            if (auto* reader = formats().createReaderFor (f))
            {
                file = f;
                length = (double) reader->lengthInSamples / reader->sampleRate;
                details = timeText (length) + "   " + juce::String (reader->sampleRate / 1000.0, 1) + " kHz   "
                          + (reader->numChannels == 1 ? "mono" : reader->numChannels == 2 ? "stereo"
                                                                                         : juce::String (reader->numChannels) + " channels")
                          + "   " + juce::File::descriptionOfSizeInBytes (f.getSize());
                const auto rate = reader->sampleRate;
                source = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
                transport.setSource (source.get(), 1 << 15, &readAhead, rate, 2);

                if (autoPlay.getToggleState())
                    play();
            }
            else
            {
                details = "Can't read this file.";
            }
        }

        shown = f;
        update();
    }

    void toggle()
    {
        if (transport.isPlaying())
            transport.stop();
        else
            play();

        update();
    }

    void stop()
    {
        transport.stop();
        update();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (panel);

        // Where the sound is: a bar, click to jump.
        g.setColour (background);
        g.fillRect (bar);

        if (length > 0.0)
        {
            const auto pos = transport.getCurrentPosition() / length;
            g.setColour (accent.withAlpha (0.55f));
            g.fillRect (bar.withWidth (juce::roundToInt (bar.getWidth() * juce::jlimit (0.0, 1.0, pos))));
            g.setColour (ink);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (timeText (transport.getCurrentPosition()) + " / " + timeText (length), bar,
                        juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        name.setBounds (r.removeFromTop (48));
        info.setBounds (r.removeFromTop (24));
        r.removeFromTop (8);
        bar = r.removeFromTop (28);
        r.removeFromTop (8);
        auto row = r.removeFromTop (28);
        playButton.setBounds (row.removeFromLeft (80));
        row.removeFromLeft (10);
        autoPlay.setBounds (row);
        hint.setBounds (r);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (length > 0.0 && bar.contains (e.getPosition()))
        {
            transport.setPosition (length * (e.x - bar.getX()) / juce::jmax (1, bar.getWidth()));

            if (! transport.isPlaying())
                play();

            update();
        }
    }

private:
    void play()
    {
        if (source == nullptr)
            return;

        if (! audioReady)
        {
            if (devices.initialiseWithDefaultDevices (0, 2).isNotEmpty())
            {
                details = "No sound output.";
                return;
            }

            player.setSource (&transport);
            devices.addAudioCallback (&player);
            audioReady = true;
        }

        if (transport.getCurrentPosition() >= length - 0.05)
            transport.setPosition (0.0);

        transport.start();
    }

    void update()
    {
        name.setText (shown.isDirectory() ? shown.getFileName() + "  (folder)" : shown.getFileName(), juce::dontSendNotification);
        info.setText (file.existsAsFile() ? details : shown.isDirectory() ? "Right or Enter opens it." : juce::String(),
                      juce::dontSendNotification);
        playButton.setEnabled (source != nullptr);
        playButton.setButtonText (transport.isPlaying() ? "Stop" : "Play");

        if (transport.isPlaying())
            startTimerHz (20);
        else
            stopTimer();

        repaint();
    }

    void timerCallback() override
    {
        if (! transport.isPlaying())
            update();

        repaint (bar);
    }

    juce::Label name, info, hint;
    juce::TextButton playButton { "Play" };
    juce::ToggleButton autoPlay { "Play on click" };
    juce::Rectangle<int> bar;

    juce::File file, shown;
    juce::String details;
    double length = 0.0;

    juce::AudioDeviceManager devices;
    juce::AudioSourcePlayer player;
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> source;
    juce::TimeSliceThread readAhead { "Preview read-ahead" };
    bool audioReady = false;
};

//==============================================================================
class BrowserComponent : public juce::Component,
                         private juce::FileBrowserListener
{
public:
    explicit BrowserComponent (std::function<void (juce::StringArray)> d) : done (std::move (d))
    {
        browser.addListener (this);
        addAndMakeVisible (browser);

        addButton.onClick = [this] { finish ({}); };
        cancelButton.onClick = [this] { close(); };
        addAndMakeVisible (addButton);
        addAndMakeVisible (cancelButton);
        addButton.setEnabled (false);

        setSize (980, 600);
    }

    ~BrowserComponent() override
    {
        lastFolder() = browser.getRoot();
        browser.removeListener (this);
    }

    void parentHierarchyChanged() override
    {
        // Keys go to the file list from the start.
        if (isShowing())
            if (auto* list = listComponent())
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (list)]
                                                 { if (safe != nullptr) safe->grabKeyboardFocus(); });
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // Typing a name in the file box keeps the arrows for the text.
        if (dynamic_cast<juce::TextEditor*> (getCurrentlyFocusedComponent()) != nullptr)
            return false;

        if (key == juce::KeyPress::leftKey)
        {
            const auto from = browser.getRoot();

            if (from.getParentDirectory() != from)
            {
                browser.goUp();

                if (auto* display = browser.getDisplayComponent())
                    display->setSelectedFile (from);     // waits for the folder to load, then selects it
            }

            return true;
        }

        if (key == juce::KeyPress::rightKey)
        {
            const auto f = browser.getHighlightedFile();

            if (f.isDirectory())
                browser.setRoot (f);

            return true;
        }

        if (key == juce::KeyPress::spaceKey)
        {
            preview.toggle();
            return true;
        }

        return false;
    }

    void paint (juce::Graphics& g) override { g.fillAll (background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto buttons = r.removeFromBottom (32);
        cancelButton.setBounds (buttons.removeFromRight (100));
        buttons.removeFromRight (8);
        addButton.setBounds (buttons.removeFromRight (140));
        r.removeFromBottom (8);
        browser.setBounds (r);
    }

private:
    juce::Component* listComponent() const
    {
        return dynamic_cast<juce::Component*> (browser.getDisplayComponent());
    }

    void selectionChanged() override
    {
        int files = 0;

        for (int i = 0; i < browser.getNumSelectedFiles(); ++i)
            if (browser.getSelectedFile (i).existsAsFile())
                ++files;

        addButton.setEnabled (files > 0);
        addButton.setButtonText (files > 1 ? "Add " + juce::String (files) + " files" : "Add");
    }

    void fileClicked (const juce::File&, const juce::MouseEvent&) override {}
    void browserRootChanged (const juce::File&) override {}

    void fileDoubleClicked (const juce::File& f) override
    {
        // A folder opens by itself; a file is added (with the others selected).
        if (f.existsAsFile())
            finish (f);
    }

    void finish (const juce::File& clicked)
    {
        juce::StringArray paths;

        for (int i = 0; i < browser.getNumSelectedFiles(); ++i)
            if (browser.getSelectedFile (i).existsAsFile())
                paths.add (browser.getSelectedFile (i).getFullPathName());

        if (clicked.existsAsFile() && ! paths.contains (clicked.getFullPathName()))
            paths = { clicked.getFullPathName() };

        if (paths.isEmpty())
            return;

        preview.stop();
        auto callback = done;
        close();

        if (callback)
            callback (paths);
    }

    void close()
    {
        preview.stop();

        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
            w->exitModalState (0);
    }

    std::function<void (juce::StringArray)> done;
    AudioPreview preview;
    juce::WildcardFileFilter filter { audioPatterns, "*", "Audio files" };
    juce::FileBrowserComponent browser { juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                             | juce::FileBrowserComponent::canSelectMultipleItems,
                                         startFolder(), &filter, &preview };
    juce::TextButton addButton { "Add" }, cancelButton { "Cancel" };
};
} // namespace

void showAudioBrowser (juce::Component& parent, std::function<void (juce::StringArray)> done)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new BrowserComponent (std::move (done)));
    o.dialogTitle = "Add audio files";
    o.dialogBackgroundColour = background;
    o.componentToCentreAround = &parent;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}
} // namespace cdj
