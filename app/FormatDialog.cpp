#include "FormatDialog.h"

namespace cdj
{
namespace
{
const juce::Colour background (0xff1d1f24), panel (0xff262931), ink (0xffe6e8ee), muted (0xff9aa0ad),
                   warn (0xffffb347), bad (0xffff6b6b);

class FormatComponent : public juce::Component
{
public:
    FormatComponent (FormatPlan p, std::function<void (juce::File)> d) : plan (std::move (p)), done (std::move (d))
    {
        confirmWord = plan.drive.label.trim().isNotEmpty() ? plan.drive.label.trim() : juce::String ("FORMAT");

        title.setText ("Format " + plan.drive.root.getFullPathName(), juce::dontSendNotification);
        title.setFont (juce::FontOptions (18.0f, juce::Font::bold));

        juce::String info;
        info << (plan.model.isNotEmpty() ? plan.model : juce::String ("USB stick")) << ", "
             << juce::File::descriptionOfSizeInBytes (plan.diskBytes) << "  (" << plan.disk << ")\n"
             << "EVERYTHING on this stick will be erased: music, the PIONEER folder, all of it.";
        details.setText (info, juce::dontSendNotification);

        fsBox.addItem ("FAT32 - every CDJ (CDJ-2000, NXS, NXS2, CDJ-3000)", 1);
        fsBox.addItem ("exFAT - newer CDJs only (NXS2 with recent firmware, CDJ-3000); not the first CDJ-2000", 2);
        fsBox.setSelectedId (1, juce::dontSendNotification);
        fsBox.onChange = [this] { update(); };

        nameLabel.setText ("New name", juce::dontSendNotification);
        nameEditor.setText (cleanVolumeLabel (plan.drive.label.isNotEmpty() ? plan.drive.label : "ADORMIS"));
        nameEditor.setInputRestrictions (11, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-");
        nameEditor.onTextChange = [this] { update(); };

        confirmLabel.setText ("To confirm, type the stick's current name: " + confirmWord, juce::dontSendNotification);
        confirmEditor.onTextChange = [this] { update(); };
        confirmEditor.onReturnKey = [this] { if (formatButton.isEnabled()) start(); };

        formatButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffc0392b));
        formatButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        formatButton.onClick = [this] { start(); };
        cancelButton.onClick = [this] { close(); };

        for (auto* l : { &title, &details, &nameLabel, &confirmLabel, &note })
        {
            l->setColour (juce::Label::textColourId, ink);
            l->setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (*l);
        }

        details.setColour (juce::Label::textColourId, warn);
        note.setColour (juce::Label::textColourId, bad);

        for (auto* e : { &nameEditor, &confirmEditor })
        {
            e->setColour (juce::TextEditor::backgroundColourId, panel);
            e->setColour (juce::TextEditor::textColourId, ink);
            addAndMakeVisible (*e);
        }

        addAndMakeVisible (fsBox);
        addAndMakeVisible (formatButton);
        addAndMakeVisible (cancelButton);
        update();
        setSize (640, 330);
    }

    void paint (juce::Graphics& g) override { g.fillAll (background); }

    void resized() override
    {
        auto a = getLocalBounds().reduced (16);
        title.setBounds (a.removeFromTop (28));
        details.setBounds (a.removeFromTop (44));
        a.removeFromTop (6);
        fsBox.setBounds (a.removeFromTop (28));
        a.removeFromTop (10);
        auto row = a.removeFromTop (28);
        nameLabel.setBounds (row.removeFromLeft (90));
        nameEditor.setBounds (row.removeFromLeft (200));
        a.removeFromTop (12);
        confirmLabel.setBounds (a.removeFromTop (22));
        confirmEditor.setBounds (a.removeFromTop (28).removeFromLeft (290));
        a.removeFromTop (6);
        note.setBounds (a.removeFromTop (46));
        auto buttons = a.removeFromBottom (34);
        formatButton.setBounds (buttons.removeFromRight (150));
        buttons.removeFromRight (10);
        cancelButton.setBounds (buttons.removeFromRight (100));
    }

private:
    FileSystem chosen() const { return fsBox.getSelectedId() == 2 ? FileSystem::exfat : FileSystem::fat32; }

    void update()
    {
        const auto limitation = formatLimitation (plan, chosen());
        note.setText (limitation, juce::dontSendNotification);
        formatButton.setEnabled (limitation.isEmpty() && confirmEditor.getText().trim() == confirmWord);
    }

    void close()
    {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
            w->exitModalState (0);
    }

    void start()
    {
        struct Job : public juce::ThreadWithProgressWindow
        {
            Job (FormatPlan p, FileSystem f, juce::String l, std::function<void (juce::File)> d)
                : ThreadWithProgressWindow ("Formatting the stick", true, false),
                  plan (std::move (p)), fs (f), label (std::move (l)), done (std::move (d))
            {
            }

            void run() override
            {
                setProgress (-1.0);
                result = formatStick (plan, fs, label, [this] (const juce::String& s) { setStatusMessage (s); });
            }

            void threadComplete (bool) override
            {
                if (result.ok)
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Stick formatted",
                                                            "The stick is now " + juce::String (fs == FileSystem::fat32 ? "FAT32" : "exFAT")
                                                                + ", named " + cleanVolumeLabel (label) + ".");
                }
                else if (result.manualCommand.isNotEmpty())
                {
                    juce::SystemClipboard::copyTextToClipboard (result.manualCommand);
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Stick not formatted",
                                                            result.error + "\n\nThe command that does it is now in the clipboard: "
                                                            "paste it into a terminal (it asks for your password).");
                }
                else
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Stick not formatted", result.error);
                }

                if (done)
                    done (result.ok ? result.newRoot : juce::File());

                delete this;
            }

            FormatPlan plan;
            FileSystem fs;
            juce::String label;
            std::function<void (juce::File)> done;
            FormatResult result;
        };

        (new Job (plan, chosen(), nameEditor.getText(), done))->launchThread();
        close();
    }

    FormatPlan plan;
    std::function<void (juce::File)> done;
    juce::String confirmWord;

    juce::Label title, details, nameLabel, confirmLabel, note;
    juce::ComboBox fsBox;
    juce::TextEditor nameEditor, confirmEditor;
    juce::TextButton formatButton { "Erase and format" }, cancelButton { "Cancel" };
};
} // namespace

void showFormatDialog (juce::Component& parent, const Drive& drive, std::function<void (juce::File)> done)
{
    FormatPlan plan;
    juce::String error;

    if (! planFormat (drive, plan, error))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Can't format this", error);
        return;
    }

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new FormatComponent (plan, std::move (done)));
    o.dialogTitle = "Format stick";
    o.dialogBackgroundColour = background;
    o.componentToCentreAround = &parent;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}
} // namespace cdj
