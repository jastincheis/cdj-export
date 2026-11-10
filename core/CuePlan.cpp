#include "CuePlan.h"

#include <algorithm>
#include <cmath>

namespace cdj
{
juce::int64 samplesToMs (juce::int64 sample, double rate)
{
    if (rate <= 0.0)
        return 0;

    // Integer maths where it's exact (44100, 48000, …), so 1 ms is never lost
    // to floating point on long files.
    const auto r = (juce::int64) std::llround (rate);

    if (std::abs (rate - (double) r) < 1.0e-9)
        return (sample * 1000 + r / 2) / r;

    return (juce::int64) std::llround ((double) sample * 1000.0 / rate);
}

namespace
{
struct Point
{
    juce::int64 start, end;     // samples in the marker rate; end < 0 = not a loop
    juce::String name;
};
} // namespace

std::vector<Cue> planCues (const Track& track, const CueOptions& options)
{
    std::vector<Point> points;

    for (const auto& m : track.markers)
        points.push_back ({ m.sample, -1, m.name });

    for (const auto& r : track.regions)
        points.push_back ({ r.start, options.regionsAsLoops ? r.end : -1, r.name });

    std::stable_sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });

    const auto toMs = [&] (juce::int64 s)
    {
        const auto ms = samplesToMs (s, track.markerRate) + (juce::int64) std::llround (options.offsetMs);
        const auto length = (juce::int64) std::floor (track.seconds() * 1000.0);
        return juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, length), ms);
    };

    std::vector<Cue> cues;
    const auto hotCount = options.hotCues ? juce::jlimit (0, 8, options.hotCueCount) : 0;

    for (size_t i = 0; i < points.size(); ++i)
    {
        Cue c;
        c.startMs = toMs (points[i].start);
        c.endMs = points[i].end >= 0 ? toMs (points[i].end) : -1;
        c.name = points[i].name;

        if ((int) i < hotCount)
        {
            auto hot = c;
            hot.kind = Cue::Kind::hot;
            hot.hotIndex = (int) i;
            hot.colour = options.colours ? defaultHotCueColour ((int) i) : 0;
            cues.push_back (hot);
        }

        if (options.memoryCues)
            cues.push_back (c);
    }

    return cues;
}

int defaultHotCueColour (int hotIndex)
{
    // Hot cue colour codes 0x01–0x3e from rekordbox's hot cue palette (the
    // table in Deep Symmetry's beat-link CueList.findRecordboxColor); 0 = the
    // plain green older players show. One distinct colour per pad, all from
    // the 16 of rekordbox's hot cue menu (the ones the players have their
    // own colour bytes for): green, teal, blue, purple, pink, red, orange, yellow.
    static constexpr int table[] = { 0x16, 0x12, 0x01, 0x38, 0x2d, 0x2a, 0x26, 0x20 };
    return hotIndex >= 0 && hotIndex < 8 ? table[hotIndex] : 0;
}

Rgb hotCueRgb (int hotIndex)
{
    // The RGB that goes with each code above, same table.
    static constexpr Rgb table[] = {
        Rgb { 0x28, 0xe2, 0x14 }, Rgb { 0x10, 0xb1, 0x76 }, Rgb { 0x30, 0x5a, 0xff },
        Rgb { 0xb4, 0x32, 0xff }, Rgb { 0xff, 0x12, 0x7b }, Rgb { 0xe6, 0x28, 0x28 },
        Rgb { 0xe0, 0x64, 0x1b }, Rgb { 0xc3, 0xaf, 0x04 } };
    return hotIndex >= 0 && hotIndex < 8 ? table[hotIndex] : table[0];
}
} // namespace cdj
