#include "TrackEditor.h"

#include "Tempo.h"

#include <atomic>
#include <map>

namespace cdj
{
namespace
{
const juce::Colour panel (0xff262931), wave (0xff6fb3ff), waveDim (0xff3d5f86), ink (0xffe6e8ee), muted (0xff9aa0ad),
                   beatLine (0x30ffffff), barLine (0x70ffffff), markerColour (0xffffb347), cursorColour (0xffffffff),
                   playColour (0xff4cd964), firstBeatColour (0xffff5a5a), regionColour (0x2266ddaa);

constexpr int flagHeight = 18, rulerHeight = 16;
constexpr juce::int64 maxAnalysisSeconds = 6 * 60;    // tempo is found on the first 6 minutes

// Small editor preferences (snap), kept next to the remembered tracks.
juce::File settingsFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Adormis CDJ Export").getChildFile ("settings.json");
}

juce::var setting (const juce::String& key)
{
    juce::var v;
    juce::JSON::parse (settingsFile().loadFileAsString(), v);
    return v[juce::Identifier (key)];
}

void setSetting (const juce::String& key, const juce::var& value)
{
    juce::var v;

    if (juce::JSON::parse (settingsFile().loadFileAsString(), v).failed() || ! v.isObject())
        v = juce::var (new juce::DynamicObject());

    v.getDynamicObject()->setProperty (juce::Identifier (key), value);
    settingsFile().getParentDirectory().createDirectory();
    settingsFile().replaceWithText (juce::JSON::toString (v));
}

juce::String clock (double s)
{
    const auto ms = (juce::int64) std::llround (s * 1000.0);
    return juce::String (ms / 60000) + ":" + juce::String ((ms / 1000) % 60).paddedLeft ('0', 2) + "."
         + juce::String (ms % 1000).paddedLeft ('0', 3);
}
} // namespace

struct TrackEditor::Peaks
{
    static constexpr int block = 64;
    double rate = 0.0;
    juce::int64 length = 0;
    std::vector<float> lo, hi;

    double seconds() const { return rate > 0.0 ? (double) length / rate : 0.0; }
};

// Reads the file once on a background thread: waveform peaks for drawing,
// a mono copy of the start for tempo detection, and the tempo when the
// track has none yet.
class TrackEditor::Loader : public juce::Thread
{
public:
    Loader (TrackEditor& e, juce::File f, bool detect)
        : Thread ("cdj track loader"), editor (&e), file (std::move (f)), detectTempo (detect)
    {
        startThread();
    }

    ~Loader() override { stopThread (10000); }

    void run() override
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));

        if (reader == nullptr)
            return;

        auto peaks = std::make_shared<Peaks>();
        peaks->rate = reader->sampleRate;
        peaks->length = reader->lengthInSamples;
        const auto blocks = (size_t) ((peaks->length + Peaks::block - 1) / Peaks::block);
        peaks->lo.assign (blocks, 0.0f);
        peaks->hi.assign (blocks, 0.0f);

        const auto monoLength = (int) juce::jmin (peaks->length, (juce::int64) (maxAnalysisSeconds * reader->sampleRate));
        auto mono = std::make_shared<juce::AudioBuffer<float>> (1, monoLength);
        mono->clear();

        constexpr int chunk = 1 << 16;     // a multiple of Peaks::block
        const auto channels = (int) juce::jmin (2u, reader->numChannels);
        juce::AudioBuffer<float> buffer (channels, chunk);

        for (juce::int64 start = 0; start < peaks->length; start += chunk)
        {
            if (threadShouldExit())
                return;

            const auto count = (int) juce::jmin ((juce::int64) chunk, peaks->length - start);
            reader->read (&buffer, 0, count, start, true, channels > 1);

            for (int i = 0; i < count; i += Peaks::block)
            {
                const auto n = juce::jmin (Peaks::block, count - i);
                float lo = 0.0f, hi = 0.0f;

                for (int ch = 0; ch < channels; ++ch)
                {
                    const auto r = juce::FloatVectorOperations::findMinAndMax (buffer.getReadPointer (ch, i), n);
                    lo = juce::jmin (lo, r.getStart());
                    hi = juce::jmax (hi, r.getEnd());
                }

                const auto b = (size_t) ((start + i) / Peaks::block);
                peaks->lo[b] = lo;
                peaks->hi[b] = hi;
            }

            if (start < monoLength)
            {
                const auto n = (int) juce::jmin ((juce::int64) count, monoLength - start);

                for (int ch = 0; ch < channels; ++ch)
                    mono->addFrom (0, (int) start, buffer, ch, 0, n, 1.0f / (float) channels);
            }
        }

        std::optional<double> bpm;
        std::optional<juce::int64> firstBeat;

        if (detectTempo && ! threadShouldExit())
        {
            bpm = measureTempo (*mono, { 0, mono->getNumSamples() }, reader->sampleRate);

            if (bpm.has_value())
                firstBeat = findFirstDownbeat (*mono, reader->sampleRate, *bpm);
        }

        const auto rate = reader->sampleRate;
        juce::MessageManager::callAsync ([e = editor, peaks, mono, bpm, firstBeat, rate]
        {
            if (auto* target = e.getComponent())
            {
                target->mono = std::move (*mono);
                target->monoRate = rate;
                target->loaded (peaks, bpm, firstBeat);
            }
        });
    }

private:
    juce::Component::SafePointer<TrackEditor> editor;
    juce::File file;
    bool detectTempo;
};

void TrackEditor::Metronome::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    const auto start = transport.getCurrentPosition();     // where this block begins in the file
    transport.getNextAudioBlock (info);

    const auto tempo = bpm.load();

    if (! enabled.load() || ! transport.isPlaying() || tempo <= 0.0 || sampleRate <= 0.0)
    {
        clickLeft = 0;
        return;
    }

    const auto period = 60.0 / tempo;
    const auto first = firstBeat.load();
    const auto length = (double) info.numSamples / sampleRate;
    const auto clickLength = (int) (0.03 * sampleRate);

    // Beats that start inside this block, by the sample they fall on.
    std::vector<std::pair<int, bool>> starts;

    for (auto k = (juce::int64) std::ceil ((start - first) / period); first + (double) k * period < start + length; ++k)
    {
        const auto at = first + (double) k * period;

        if (at >= start)
            starts.push_back ({ (int) ((at - start) * sampleRate), ((k % 4) + 4) % 4 == 0 });
    }

    size_t next = 0;

    for (int i = 0; i < info.numSamples; ++i)
    {
        if (next < starts.size() && starts[next].first == i)
        {
            clickLeft = clickLength;
            clickDone = 0;
            clickAccent = starts[next].second;
            ++next;
        }

        if (clickLeft <= 0)
            continue;

        const auto t = (double) clickDone / sampleRate;
        const auto freq = clickAccent ? 1760.0 : 1100.0;
        const auto v = (float) ((clickAccent ? 0.55 : 0.4) * std::sin (2.0 * juce::MathConstants<double>::pi * freq * t)
                                * std::exp (-t * 120.0));

        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->addSample (ch, info.startSample + i, v);

        --clickLeft;
        ++clickDone;
    }
}

TrackEditor::TrackEditor()
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);

    for (auto* l : { &bpmLabel, &gridNote, &keysHint })
    {
        l->setColour (juce::Label::textColourId, muted);
        addAndMakeVisible (*l);
    }

    keysHint.setText ("Space play/stop   W/E start/end   Up/Down zoom   Left/Right markers   M or double-click: add marker   "
                      "double-click a marker or select it + Delete: remove   R rename   B first beat   K metronome   "
                      "G snap to grid (Shift: opposite)   drag a flag to move it",
                      juce::dontSendNotification);
    keysHint.setFont (juce::FontOptions (12.0f));

    bpmValue.setEditable (true);
    bpmValue.setColour (juce::Label::textColourId, ink);
    bpmValue.setColour (juce::Label::backgroundColourId, panel);
    bpmValue.setColour (juce::Label::outlineColourId, juce::Colour (0xff3a3e48));
    bpmValue.setJustificationType (juce::Justification::centred);
    bpmValue.onTextChange = [this]
    {
        const auto v = bpmValue.getText().retainCharacters ("0123456789.,").replaceCharacter (',', '.').getDoubleValue();

        if (v >= 40.0 && v <= 300.0)
            setBpm (v, true);
        else
            updateControls();
    };
    addAndMakeVisible (bpmValue);

    metronomeButton.setColour (juce::ToggleButton::textColourId, ink);
    metronomeButton.setColour (juce::ToggleButton::tickColourId, playColour);
    metronomeButton.setWantsKeyboardFocus (false);
    metronomeButton.onClick = [this] { metronome.enabled = metronomeButton.getToggleState(); };
    addAndMakeVisible (metronomeButton);

    snapButton.setColour (juce::ToggleButton::textColourId, ink);
    snapButton.setColour (juce::ToggleButton::tickColourId, markerColour);
    snapButton.setWantsKeyboardFocus (false);
    snapButton.setToggleState ((bool) setting ("snapToGrid"), juce::dontSendNotification);
    snapButton.onClick = [this] { setSetting ("snapToGrid", snapButton.getToggleState()); };
    addAndMakeVisible (snapButton);

    detectButton.onClick = [this] { detectTempo(); };
    halfButton.onClick = [this] { if (track != nullptr && track->bpm > 0.0) setBpm (track->bpm / 2.0, true); };
    doubleButton.onClick = [this] { if (track != nullptr && track->bpm > 0.0) setBpm (track->bpm * 2.0, true); };
    firstBeatButton.onClick = [this] { setFirstBeat (transport.isPlaying() ? playhead() : cursor); };
    earlierButton.onClick = [this] { if (track != nullptr) setFirstBeat ((double) track->firstBeat / track->sampleRate - 0.005); };
    laterButton.onClick = [this] { if (track != nullptr) setFirstBeat ((double) track->firstBeat / track->sampleRate + 0.005); };
    saveButton.onClick = [this] { saveMarkersNow(); };

    for (auto* b : { &detectButton, &halfButton, &doubleButton, &firstBeatButton, &earlierButton, &laterButton, &saveButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }

    readAhead.startThread();
    updateControls();
}

TrackEditor::~TrackEditor()
{
    stop();
    loader = nullptr;

    if (audioReady)
        devices.removeAudioCallback (&sourcePlayer);

    sourcePlayer.setSource (nullptr);
    transport.setSource (nullptr);
    readerSource = nullptr;
    readAhead.stopThread (2000);
}

void TrackEditor::setTrack (Track* t)
{
    if (t == track && t != nullptr && peaks != nullptr)
        return;

    stop();
    transport.setSource (nullptr);
    readerSource = nullptr;
    track = t;
    peaks = nullptr;
    mono.setSize (0, 0);
    cursor = playStart = 0.0;
    dragging = selected = -1;

    if (track != nullptr)
    {
        setView (0.0, track->seconds());
        startLoading();

        if (auto* reader = formats().createReaderFor (track->file))
        {
            readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
            transport.setSource (readerSource.get(), 1 << 15, &readAhead, reader->sampleRate, 2);
        }
    }
    else
    {
        loader = nullptr;
    }

    updateControls();
    repaint();
}

void TrackEditor::startLoading()
{
    loader = nullptr;
    loader = std::make_unique<Loader> (*this, track->file, track->bpm <= 0.0);
}

void TrackEditor::loaded (std::shared_ptr<Peaks> p, std::optional<double> bpm, std::optional<juce::int64> firstBeat)
{
    peaks = std::move (p);

    if (track != nullptr && track->bpm <= 0.0 && bpm.has_value())
    {
        track->bpm = *bpm;
        track->firstBeat = firstBeat.value_or (0);
        track->gridByHand = false;
        gridEdited();
    }
    else if (track != nullptr && track->bpm <= 0.0)
    {
        gridNote.setText ("No steady beat found: type the tempo and set the first beat by hand.", juce::dontSendNotification);
    }

    repaint();
}

void TrackEditor::detectTempo()
{
    if (track == nullptr || mono.getNumSamples() == 0)
        return;

    const auto bpm = measureTempo (mono, { 0, mono.getNumSamples() }, monoRate);

    if (! bpm.has_value())
    {
        gridNote.setText ("No steady beat found.", juce::dontSendNotification);
        return;
    }

    track->bpm = *bpm;
    track->firstBeat = findFirstDownbeat (mono, monoRate, *bpm).value_or (track->firstBeat);
    track->gridByHand = false;
    gridEdited();
}

//==============================================================================
juce::Rectangle<int> TrackEditor::waveArea() const
{
    return getLocalBounds().withTrimmedTop (34).withTrimmedBottom (20);
}

double TrackEditor::xToSeconds (float x) const
{
    const auto a = waveArea();
    return viewStart + (double) (x - (float) a.getX()) / (double) juce::jmax (1, a.getWidth()) * viewLength;
}

float TrackEditor::secondsToX (double s) const
{
    const auto a = waveArea();
    return (float) a.getX() + (float) ((s - viewStart) / viewLength * a.getWidth());
}

void TrackEditor::setView (double start, double length)
{
    const auto total = track != nullptr ? juce::jmax (0.01, track->seconds()) : 1.0;
    viewLength = juce::jlimit (0.05, total, length);
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, total - viewLength), start);
    repaint();
}

void TrackEditor::showSeconds (double s)
{
    if (s < viewStart || s > viewStart + viewLength)
        setView (s - viewLength * 0.25, viewLength);
}

void TrackEditor::zoom (double factor, double around)
{
    const auto newLength = viewLength * factor;
    const auto fraction = viewLength > 0.0 ? (around - viewStart) / viewLength : 0.5;
    setView (around - fraction * newLength, newLength);
}

//==============================================================================
double TrackEditor::playhead() const
{
    return transport.getCurrentPosition();
}

void TrackEditor::togglePlay()
{
    if (track == nullptr || readerSource == nullptr)
        return;

    if (transport.isPlaying())
    {
        stop();
        return;
    }

    if (! audioReady)
    {
        const auto error = devices.initialiseWithDefaultDevices (0, 2);

        if (error.isNotEmpty())
        {
            gridNote.setText ("No sound output: " + error, juce::dontSendNotification);
            return;
        }

        sourcePlayer.setSource (&metronome);
        devices.addAudioCallback (&sourcePlayer);
        audioReady = true;
    }

    playStart = cursor;
    transport.setPosition (cursor);
    transport.start();
    startTimerHz (30);
}

void TrackEditor::stop()
{
    if (transport.isPlaying())
    {
        transport.stop();
        cursor = playStart;     // like Sound Forge: back to where playback began
    }

    stopTimer();
    repaint();
}

void TrackEditor::timerCallback()
{
    if (! transport.isPlaying())
    {
        stop();
        return;
    }

    const auto p = playhead();

    if (p > viewStart + viewLength || p < viewStart)
        setView (p, viewLength);

    repaint();
}

//==============================================================================
int TrackEditor::markerAt (float x, float y) const
{
    if (track == nullptr)
        return -1;

    const auto a = waveArea();
    int best = -1;
    float bestDistance = 6.0f;

    for (int i = 0; i < (int) track->markers.size(); ++i)
    {
        const auto mx = secondsToX (track->markerInFile (track->markers[(size_t) i].sample));
        const auto onFlag = y >= (float) a.getY() && y < (float) (a.getY() + flagHeight) && x >= mx && x < mx + 70.0f;
        const auto d = std::abs (x - mx);

        if (onFlag && best < 0)
            best = i;

        if (d < bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }

    return best;
}

int TrackEditor::markerNearCursor() const
{
    if (track == nullptr)
        return -1;

    const auto tolerance = juce::jmax (0.001, viewLength / juce::jmax (1, waveArea().getWidth()) * 3.0);

    for (int i = 0; i < (int) track->markers.size(); ++i)
        if (std::abs (track->markerInFile (track->markers[(size_t) i].sample) - cursor) <= tolerance)
            return i;

    return -1;
}

void TrackEditor::sortMarkers (int& keepIndex)
{
    const auto kept = keepIndex >= 0 ? track->markers[(size_t) keepIndex] : Marker();
    std::stable_sort (track->markers.begin(), track->markers.end(),
                      [] (const auto& a, const auto& b) { return a.sample < b.sample; });

    if (keepIndex >= 0)
        for (int i = 0; i < (int) track->markers.size(); ++i)
            if (track->markers[(size_t) i].sample == kept.sample && track->markers[(size_t) i].name == kept.name)
                keepIndex = i;
}

double TrackEditor::snap (double seconds, bool invert) const
{
    if (track == nullptr || track->bpm <= 0.0 || snapButton.getToggleState() == invert)
        return seconds;

    const auto period = 60.0 / track->bpm;
    const auto first = (double) track->firstBeat / track->sampleRate;
    return juce::jlimit (0.0, track->seconds(), first + std::round ((seconds - first) / period) * period);
}

void TrackEditor::addMarker (double seconds, bool invertSnap)
{
    if (track == nullptr)
        return;

    seconds = snap (seconds, invertSnap);

    if (track->markerRate <= 0.0)
        track->markerRate = track->sampleRate;

    int index = (int) track->markers.size();
    track->markers.push_back ({ track->markerFromFile (juce::jlimit (0.0, track->seconds(), seconds)), {} });
    sortMarkers (index);
    selected = index;
    markersEdited();
}

void TrackEditor::deleteMarker (int index)
{
    if (track == nullptr || index < 0 || index >= (int) track->markers.size())
        return;

    track->markers.erase (track->markers.begin() + index);
    selected = -1;
    markersEdited();
}

void TrackEditor::renameMarker (int index)
{
    if (track == nullptr || index < 0 || index >= (int) track->markers.size())
        return;

    auto* w = new juce::AlertWindow ("Marker name", "The name becomes the cue's comment on the CDJ.",
                                     juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", track->markers[(size_t) index].name);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    const auto sample = track->markers[(size_t) index].sample;
    juce::Component::SafePointer<TrackEditor> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, sample] (int result)
    {
        if (result == 1 && safe != nullptr && safe->track != nullptr)
            for (auto& m : safe->track->markers)
                if (m.sample == sample)
                {
                    m.name = w->getTextEditorContents ("name").trim();
                    safe->markersEdited();
                    break;
                }
    }), true);
}

void TrackEditor::jumpMarker (int direction)
{
    if (track == nullptr)
        return;

    // Markers and region starts, in time order.
    std::vector<double> points;

    for (const auto& m : track->markers)
        points.push_back (track->markerInFile (m.sample));

    for (const auto& r : track->regions)
        points.push_back (track->markerInFile (r.start));

    std::sort (points.begin(), points.end());
    const auto from = transport.isPlaying() ? playhead() : cursor;
    constexpr double epsilon = 0.0005;
    std::optional<double> target;

    if (direction > 0)
    {
        for (auto p : points)
            if (p > from + epsilon) { target = p; break; }
    }
    else
    {
        for (auto it = points.rbegin(); it != points.rend(); ++it)
            if (*it < from - epsilon) { target = *it; break; }
    }

    if (! target.has_value())
        return;

    cursor = *target;
    selected = markerNearCursor();

    if (transport.isPlaying())
        transport.setPosition (cursor);

    showSeconds (cursor);
    repaint();
}

void TrackEditor::markersEdited()
{
    track->markersChanged = true;
    updateControls();
    repaint();

    if (onChange)
        onChange();
}

//==============================================================================
void TrackEditor::setFirstBeat (double seconds)
{
    if (track == nullptr)
        return;

    track->firstBeat = (juce::int64) std::llround (juce::jlimit (0.0, track->seconds(), seconds) * track->sampleRate);
    track->gridByHand = true;

    if (track->bpm <= 0.0)
        gridNote.setText ("Now type the tempo, or press Detect.", juce::dontSendNotification);

    gridEdited();
}

void TrackEditor::setBpm (double bpm, bool byHand)
{
    if (track == nullptr)
        return;

    track->bpm = std::round (bpm * 100.0) / 100.0;
    track->gridByHand = track->gridByHand || byHand;
    gridEdited();
}

void TrackEditor::gridEdited()
{
    rememberTrack (*track);
    updateControls();
    repaint();

    if (onChange)
        onChange();
}

void TrackEditor::saveMarkersNow()
{
    if (track == nullptr)
        return;

    juce::String error;

    if (! saveMarkers (*track, error))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Markers not saved", error);

    updateControls();

    if (onChange)
        onChange();
}

void TrackEditor::updateMetronome()
{
    metronome.bpm = track != nullptr ? track->bpm : 0.0;
    metronome.firstBeat = track != nullptr && track->sampleRate > 0.0 ? (double) track->firstBeat / track->sampleRate : 0.0;
}

void TrackEditor::updateControls()
{
    updateMetronome();

    const auto has = track != nullptr;

    for (auto* c : std::initializer_list<juce::Component*> { &bpmValue, &detectButton, &halfButton, &doubleButton,
                                                             &firstBeatButton, &earlierButton, &laterButton })
        c->setEnabled (has);

    saveButton.setEnabled (has && track->markersChanged);
    saveButton.setButtonText (has && track->markersChanged ? "Save markers (Ctrl+S) *" : "Markers saved");

    if (! has)
    {
        bpmValue.setText ({}, juce::dontSendNotification);
        gridNote.setText ("Select a track in the playlist", juce::dontSendNotification);
        return;
    }

    bpmValue.setText (track->bpm > 0.0 ? juce::String (track->bpm, 2) : juce::String ("?"), juce::dontSendNotification);
    halfButton.setEnabled (track->bpm > 0.0);
    doubleButton.setEnabled (track->bpm > 0.0);

    juce::String note;

    if (track->bpm > 0.0)
        note << "First beat " << clock ((double) track->firstBeat / track->sampleRate)
             << (track->gridByHand ? "  (set by hand)" : "  (detected - check it)");
    else if (peaks == nullptr)
        note << "Finding the tempo...";

    if (note.isNotEmpty())
        gridNote.setText (note, juce::dontSendNotification);
}

//==============================================================================
bool TrackEditor::keyPressed (const juce::KeyPress& key)
{
    if (track == nullptr)
        return false;

    const auto code = key.getKeyCode();
    const auto at = transport.isPlaying() ? playhead() : cursor;
    const auto moveTo = [this] (double s)
    {
        cursor = juce::jlimit (0.0, track->seconds(), s);

        if (transport.isPlaying())
            transport.setPosition (cursor);

        showSeconds (cursor);
        repaint();
    };

    if (code == juce::KeyPress::spaceKey)               togglePlay();
    else if (code == 'W' || code == 'w')                { moveTo (0.0); setView (0.0, viewLength); }
    else if (code == 'E' || code == 'e')                { moveTo (track->seconds()); setView (track->seconds() - viewLength, viewLength); }
    else if (code == juce::KeyPress::upKey)             zoom (0.5, at);
    else if (code == juce::KeyPress::downKey)           zoom (2.0, at);
    else if (code == juce::KeyPress::leftKey)           jumpMarker (-1);
    else if (code == juce::KeyPress::rightKey)          jumpMarker (1);
    else if ((code == 'M' || code == 'm') && ! key.getModifiers().isCommandDown())
                                                        addMarker (at, key.getModifiers().isShiftDown());
    else if (code == 'G' || code == 'g')                { snapButton.setToggleState (! snapButton.getToggleState(), juce::dontSendNotification);
                                                          setSetting ("snapToGrid", snapButton.getToggleState()); }
    else if (code == 'B' || code == 'b')                setFirstBeat (at);
    else if (code == 'K' || code == 'k')                { metronomeButton.setToggleState (! metronomeButton.getToggleState(), juce::dontSendNotification);
                                                          metronome.enabled = metronomeButton.getToggleState(); }
    else if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
                                                        deleteMarker (selected >= 0 ? selected : markerNearCursor());
    else if ((code == 'R' || code == 'r') && ! key.getModifiers().isCommandDown())
                                                        renameMarker (selected >= 0 ? selected : markerNearCursor());
    else if ((code == 'S' || code == 's') && key.getModifiers().isCommandDown())
                                                        saveMarkersNow();
    else
        return false;

    return true;
}

void TrackEditor::mouseDown (const juce::MouseEvent& e)
{
    if (track == nullptr || ! waveArea().contains (e.getPosition()))
        return;

    grabKeyboardFocus();
    const auto x = (float) e.x, y = (float) e.y;
    const auto fbX = secondsToX ((double) track->firstBeat / track->sampleRate);
    const auto onFirstBeat = track->bpm > 0.0 && std::abs (x - fbX) < 7.0f && y > (float) (waveArea().getBottom() - rulerHeight - 4);
    const auto marker = onFirstBeat ? -1 : markerAt (x, y);

    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        const auto s = xToSeconds (x);

        if (marker >= 0)
        {
            menu.addItem ("Rename marker...", [this, marker] { renameMarker (marker); });
            menu.addItem ("Delete marker", [this, marker] { deleteMarker (marker); });
            menu.addItem ("First beat on this marker", [this, marker]
                          { setFirstBeat (track->markerInFile (track->markers[(size_t) marker].sample)); });
        }
        else
        {
            menu.addItem ("Add marker here", [this, s] { addMarker (s); });
            menu.addItem ("First beat here (the \"1\" of a bar)", [this, s] { setFirstBeat (s); });
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ e.getScreenX(), e.getScreenY(), 1, 1 })
                                                      .withParentComponent (getTopLevelComponent()));
        return;
    }

    dragMoved = false;

    if (onFirstBeat)
    {
        draggingFirstBeat = true;
        return;
    }

    const auto onMarker = marker >= 0
        && (y < (float) (waveArea().getY() + flagHeight)
            || std::abs (x - secondsToX (track->markerInFile (track->markers[(size_t) marker].sample))) < 4.0f);

    // Double-click (acted on at the second press): on a marker removes it,
    // anywhere else adds one there.
    if (e.getNumberOfClicks() == 2)
    {
        dragging = -1;

        if (onMarker)
            deleteMarker (marker);
        else
            addMarker (juce::jlimit (0.0, track->seconds(), xToSeconds (x)), e.mods.isShiftDown());

        return;
    }

    if (onMarker)
    {
        dragging = selected = marker;
        cursor = track->markerInFile (track->markers[(size_t) marker].sample);
        repaint();
        return;
    }

    selected = -1;
    cursor = juce::jlimit (0.0, track->seconds(), xToSeconds (x));

    if (transport.isPlaying())
        transport.setPosition (cursor);

    repaint();
}

void TrackEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (track == nullptr)
        return;

    auto s = juce::jlimit (0.0, track->seconds(), xToSeconds ((float) e.x));

    // A click that wobbles a pixel or two is still a click, not a move.
    if (! dragMoved && e.getDistanceFromDragStart() < 3)
        return;

    dragMoved = true;

    if (draggingFirstBeat)
    {
        track->firstBeat = (juce::int64) std::llround (s * track->sampleRate);
        track->gridByHand = true;
        updateControls();     // also moves the metronome
        repaint();
        return;
    }

    if (dragging < 0 || dragging >= (int) track->markers.size())
        return;

    // Snap to grid (Shift: the opposite while dragging).
    s = snap (s, e.mods.isShiftDown());

    track->markers[(size_t) dragging].sample = track->markerFromFile (s);
    cursor = s;
    sortMarkers (dragging);
    selected = dragging;
    track->markersChanged = true;
    repaint();
}

void TrackEditor::mouseUp (const juce::MouseEvent&)
{
    if (draggingFirstBeat && dragMoved)
        gridEdited();

    if (dragging >= 0 && dragMoved)
        markersEdited();

    draggingFirstBeat = false;
    dragging = -1;
    dragMoved = false;
}

void TrackEditor::mouseDoubleClick (const juce::MouseEvent&)
{
    // Handled in mouseDown on the second press (JUCE reports the double-click
    // only at mouse-up, which feels late).
}

void TrackEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto onFlag = track != nullptr && markerAt ((float) e.x, (float) e.y) >= 0;
    setMouseCursor (onFlag ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void TrackEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (track == nullptr)
        return;

    if (e.mods.isCommandDown())
        zoom (w.deltaY > 0 ? 0.8 : 1.25, xToSeconds ((float) e.x));
    else
        setView (viewStart - (std::abs (w.deltaX) > 0.0f ? w.deltaX : w.deltaY) * viewLength * 0.25, viewLength);
}

//==============================================================================
void TrackEditor::resized()
{
    auto top = getLocalBounds().removeFromTop (28);
    metronomeButton.setBounds (top.removeFromLeft (130));
    snapButton.setBounds (top.removeFromLeft (140));
    bpmLabel.setBounds (top.removeFromLeft (50));
    bpmValue.setBounds (top.removeFromLeft (70).reduced (0, 2));
    top.removeFromLeft (6);

    for (auto* b : { &detectButton, &halfButton, &doubleButton })
    {
        b->setBounds (top.removeFromLeft (b == &detectButton ? 70 : 36).reduced (0, 2));
        top.removeFromLeft (4);
    }

    top.removeFromLeft (12);
    firstBeatButton.setBounds (top.removeFromLeft (150).reduced (0, 2));
    top.removeFromLeft (4);
    earlierButton.setBounds (top.removeFromLeft (52).reduced (0, 2));
    top.removeFromLeft (4);
    laterButton.setBounds (top.removeFromLeft (52).reduced (0, 2));
    saveButton.setBounds (top.removeFromRight (180).reduced (0, 2));
    gridNote.setBounds (top.reduced (8, 0));

    keysHint.setBounds (getLocalBounds().removeFromBottom (18));
}

void TrackEditor::paint (juce::Graphics& g)
{
    const auto a = waveArea();
    g.setColour (panel);
    g.fillRect (a);

    if (track == nullptr)
    {
        g.setColour (muted);
        g.drawText ("Select a track to see it here, set its tempo and first beat, and add or move markers.", a,
                    juce::Justification::centred);
        return;
    }

    const auto wave_ = a.withTrimmedTop (flagHeight).withTrimmedBottom (rulerHeight);
    const auto mid = (float) wave_.getCentreY();
    const auto halfHeight = (float) wave_.getHeight() * 0.48f;

    // Regions (loops on the CDJ).
    for (const auto& r : track->regions)
    {
        const auto x1 = secondsToX (track->markerInFile (r.start)), x2 = secondsToX (track->markerInFile (r.end));
        g.setColour (regionColour);
        g.fillRect (juce::Rectangle<float> (x1, (float) wave_.getY(), juce::jmax (1.0f, x2 - x1), (float) wave_.getHeight()));
    }

    // Waveform.
    if (peaks != nullptr && ! peaks->hi.empty())
    {
        const auto samplesPerPixel = viewLength * peaks->rate / juce::jmax (1, a.getWidth());

        for (int px = 0; px < a.getWidth(); ++px)
        {
            const auto s0 = (juce::int64) ((viewStart * peaks->rate) + px * samplesPerPixel);
            const auto s1 = (juce::int64) ((viewStart * peaks->rate) + (px + 1) * samplesPerPixel);
            auto b0 = (size_t) juce::jmax ((juce::int64) 0, s0 / TrackEditor::Peaks::block);
            auto b1 = (size_t) juce::jmax ((juce::int64) b0 + 1, (s1 + TrackEditor::Peaks::block - 1) / TrackEditor::Peaks::block);
            b1 = juce::jmin (b1, peaks->hi.size());

            if (b0 >= b1)
                continue;

            float lo = 0.0f, hi = 0.0f;

            for (auto b = b0; b < b1; ++b)
            {
                lo = juce::jmin (lo, peaks->lo[b]);
                hi = juce::jmax (hi, peaks->hi[b]);
            }

            const auto x = (float) (a.getX() + px);
            g.setColour (wave);
            g.drawVerticalLine ((int) x, mid - hi * halfHeight, mid - lo * halfHeight + 1.0f);
        }
    }
    else
    {
        g.setColour (muted);
        g.drawText ("Reading the file...", wave_, juce::Justification::centred);
    }

    // Beat grid: thin beats, bright bars with their numbers.
    if (track->bpm > 0.0)
    {
        const auto period = 60.0 / track->bpm;
        const auto first = (double) track->firstBeat / track->sampleRate;
        const auto beatPixels = period / viewLength * a.getWidth();
        const auto k0 = (juce::int64) std::floor ((viewStart - first) / period);
        const auto k1 = (juce::int64) std::ceil ((viewStart + viewLength - first) / period);

        if (beatPixels * 4.0 >= 4.0)
            for (auto k = k0; k <= k1; ++k)
            {
                const auto isBar = ((k % 4) + 4) % 4 == 0;

                if (! isBar && beatPixels < 5.0)
                    continue;

                const auto x = secondsToX (first + (double) k * period);
                g.setColour (isBar ? barLine : beatLine);
                g.drawVerticalLine ((int) x, (float) wave_.getY(), (float) wave_.getBottom());

                if (isBar && beatPixels * 4.0 > 28.0)
                {
                    const auto bar = k >= 0 ? k / 4 + 1 : -((-k + 3) / 4);
                    g.setColour (muted);
                    g.setFont (juce::FontOptions (11.0f));
                    g.drawText (juce::String (bar), juce::Rectangle<float> (x + 2.0f, (float) wave_.getBottom(), 40.0f, (float) rulerHeight),
                                juce::Justification::centredLeft);
                }
            }

        // The first beat handle.
        const auto fx = secondsToX (first);
        juce::Path tri;
        tri.addTriangle (fx - 6.0f, (float) a.getBottom(), fx + 6.0f, (float) a.getBottom(), fx, (float) a.getBottom() - 10.0f);
        g.setColour (firstBeatColour);
        g.fillPath (tri);
        g.drawVerticalLine ((int) fx, (float) wave_.getY(), (float) a.getBottom());
    }

    // Markers, with the letter the hot cue will get.
    std::map<juce::int64, juce::String> letters;

    if (cueOptions)
        for (const auto& c : planCues (*track, cueOptions()))
            if (c.kind == Cue::Kind::hot)
                letters[c.startMs] = c.hotLetter();

    for (int i = 0; i < (int) track->markers.size(); ++i)
    {
        const auto& m = track->markers[(size_t) i];
        const auto x = secondsToX (track->markerInFile (m.sample));

        if (x < (float) a.getX() - 80.0f || x > (float) a.getRight())
            continue;

        const auto letter = letters.count (samplesToMs (m.sample, track->markerRate)) != 0
                                ? letters[samplesToMs (m.sample, track->markerRate)] : juce::String();
        auto label = letter.isNotEmpty() ? letter : juce::String ("M");

        if (m.name.isNotEmpty())
            label << " " << m.name;

        const auto isSelected = i == selected;
        g.setColour (isSelected ? juce::Colours::white : markerColour);

        if (isSelected)
            g.fillRect (juce::Rectangle<float> (x - 1.0f, (float) a.getY(), 3.0f, (float) (wave_.getBottom() - a.getY())));
        else
            g.drawVerticalLine ((int) x, (float) a.getY(), (float) wave_.getBottom());
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        const auto w = juce::jmin (160.0f, (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), label) + 10.0f);
        g.fillRect (juce::Rectangle<float> (x, (float) a.getY(), w, (float) flagHeight));
        g.setColour (juce::Colours::black);
        g.drawText (label, juce::Rectangle<float> (x + 4.0f, (float) a.getY(), w - 6.0f, (float) flagHeight),
                    juce::Justification::centredLeft, true);
    }

    // Cursor and playhead.
    g.setColour (cursorColour);
    g.drawVerticalLine ((int) secondsToX (cursor), (float) a.getY(), (float) a.getBottom());

    if (transport.isPlaying())
    {
        g.setColour (playColour);
        g.fillRect (juce::Rectangle<float> (secondsToX (playhead()) - 1.0f, (float) a.getY(), 2.0f, (float) a.getHeight()));
    }

    // Where we are.
    g.setColour (ink);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (clock (transport.isPlaying() ? playhead() : cursor) + "   view " + clock (viewStart) + " - "
                    + clock (viewStart + viewLength),
                a.withTrimmedRight (6).withHeight (flagHeight).translated (0, flagHeight), juce::Justification::centredRight);

    if (hasKeyboardFocus (true))
    {
        g.setColour (juce::Colour (0xff4aa3ff));
        g.drawRect (a, 1);
    }
}
} // namespace cdj
