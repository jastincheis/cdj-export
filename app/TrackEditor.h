#pragma once

#include "CuePlan.h"
#include "Track.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>

// One track up close: waveform, beat grid, markers, playback.
//   Space  play / stop          W / E   start / end of the file
//   Up / Down  zoom in / out    Left / Right  move (stops on markers; Shift faster; Ctrl next marker)
//   M or double-click  add a marker      double-click a marker  remove it
//   click a marker to select it, Delete removes it        R  rename the selected marker
//   B  first beat (the "1") at the cursor        Ctrl+S  save markers into the file
//   K  metronome on / off        G  snap markers to the beat grid (Shift: the opposite, once)
namespace cdj
{
class TrackEditor : public juce::Component,
                    private juce::Timer
{
public:
    TrackEditor();
    ~TrackEditor() override;

    // The editor works on the playlist's own Track; null shows nothing.
    void setTrack (Track* track);
    Track* getTrack() const { return track; }

    // Markers, tempo or first beat changed (the playlist shows the cue summary).
    std::function<void()> onChange;
    // How the cues will come out, to label the markers A, B, C… as on the CDJ.
    std::function<CueOptions()> cueOptions;

    bool keyPressed (const juce::KeyPress&) override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;

    void stop();

private:
    struct Peaks;
    class Loader;

    // Plays the file and adds a click on every beat of the grid (a higher
    // one on the "1" of each bar), sample-accurate with the music.
    class Metronome : public juce::AudioSource
    {
    public:
        explicit Metronome (juce::AudioTransportSource& t) : transport (t) {}

        void prepareToPlay (int block, double rate) override { sampleRate = rate; transport.prepareToPlay (block, rate); }
        void releaseResources() override { transport.releaseResources(); }
        void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

        std::atomic<bool> enabled { false };
        std::atomic<double> bpm { 0.0 }, firstBeat { 0.0 };    // first beat in seconds

    private:
        juce::AudioTransportSource& transport;
        double sampleRate = 44100.0;
        int clickLeft = 0, clickDone = 0;
        bool clickAccent = false;
    };

    void timerCallback() override;
    void startLoading();
    void loaded (std::shared_ptr<Peaks> peaks, std::optional<double> bpm, std::optional<juce::int64> firstBeat);
    void detectTempo();

    juce::Rectangle<int> waveArea() const;
    double xToSeconds (float x) const;
    float secondsToX (double s) const;
    void setView (double start, double length);
    void showSeconds (double s);
    void zoom (double factor, double around);
    void togglePlay();
    double playhead() const;

    // Markers (index into track->markers), in file seconds.
    int markerAt (float x, float y) const;
    int markerNearCursor() const;
    void addMarker (double seconds, bool invertSnap = false);
    // The nearest beat when Snap to grid is on (Shift does the opposite).
    double snap (double seconds, bool invert) const;
    void deleteMarker (int index);
    void renameMarker (int index);
    void sortMarkers (int& keepIndex);
    void jumpMarker (int direction);
    void scrub (int direction, double fractionOfView);
    void markersEdited();

    void setFirstBeat (double seconds);
    void setBpm (double bpm, bool byHand);
    void gridEdited();
    void saveMarkersNow();
    void updateControls();
    void updateMetronome();

    Track* track = nullptr;
    std::shared_ptr<Peaks> peaks;
    std::unique_ptr<Loader> loader;
    juce::AudioBuffer<float> mono;      // for tempo detection
    double monoRate = 0.0;

    double viewStart = 0.0, viewLength = 1.0;
    double cursor = 0.0, playStart = 0.0;
    int dragging = -1;
    int selected = -1;          // marker picked with a click (index into track->markers)
    bool draggingFirstBeat = false, dragMoved = false;

    juce::AudioDeviceManager devices;
    juce::AudioSourcePlayer sourcePlayer;
    juce::AudioTransportSource transport;
    Metronome metronome { transport };
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::TimeSliceThread readAhead { "cdj read-ahead" };
    bool audioReady = false;

    juce::Label bpmLabel { {}, "Tempo" }, bpmValue, gridNote, keysHint;
    juce::ToggleButton metronomeButton { "Metronome (K)" }, snapButton { "Snap to grid (G)" };
    juce::TextButton detectButton { "Detect" }, halfButton { "/2" }, doubleButton { "x2" },
                     firstBeatButton { "First beat here (B)" }, earlierButton { "-5 ms" }, laterButton { "+5 ms" },
                     saveButton { "Save markers (Ctrl+S)" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackEditor)
};
} // namespace cdj
