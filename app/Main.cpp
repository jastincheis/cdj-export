#include "ExportComponent.h"

class AdormisCdjExportApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        window = std::make_unique<MainWindow> (getApplicationName() + " " + getApplicationVersion());

        // Files given on the command line go straight into the playlist.
        juce::StringArray files;

        for (const auto& arg : juce::StringArray::fromTokens (commandLine, true))
        {
            const auto path = arg.unquoted();

            if (juce::File::isAbsolutePath (path) && juce::File (path).exists())
                files.add (path);
        }

        if (! files.isEmpty())
            window->content->addFiles (files);
    }

    void shutdown() override { window = nullptr; }
    void systemRequestedQuit() override { quit(); }

private:
    struct MainWindow : public juce::DocumentWindow
    {
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colour (0xff1d1f24), DocumentWindow::allButtons)
        {
            content = new cdj::ExportComponent();
            setUsingNativeTitleBar (true);
            setContentOwned (content, true);
            setResizable (true, true);
            setResizeLimits (820, 640, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            if (content == nullptr || ! content->hasUnsavedMarkers())
            {
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
                return;
            }

            juce::Component::SafePointer<MainWindow> safe (this);
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                              .withTitle ("Save the markers?")
                                              .withMessage ("Some markers were changed and not saved into their files.")
                                              .withButton ("Save")
                                              .withButton ("Don't save")
                                              .withButton ("Cancel"),
                                          [safe] (int result)
                                          {
                                              if (safe == nullptr || result == 0)
                                                  return;

                                              if (result == 1)
                                              {
                                                  juce::String errors;

                                                  if (! safe->content->saveAllMarkers (errors))
                                                  {
                                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                                              "Markers not saved", errors);
                                                      return;
                                                  }
                                              }

                                              juce::JUCEApplication::getInstance()->systemRequestedQuit();
                                          });
        }

        cdj::ExportComponent* content = nullptr;
    };

    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (AdormisCdjExportApplication)
