// measureTempo comes from Adormis Audio (Source/Tempo.cpp, same author,
// AGPLv3); findBeatGrid is new here.

#include "Tempo.h"
#include <juce_dsp/juce_dsp.h>

namespace cdj
{
namespace
{
// Onset strength (log spectral flux above its local average), one value per
// `hop` samples.
std::vector<float> onsetEnvelope (const juce::AudioBuffer<float>& audio, juce::Range<int> range, int hop, double hopSeconds,
                                  int maxBin = 0)
{
    constexpr int order = 10, size = 1 << order;
    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });
    const auto length = range.getLength();

    if (length < size * 2)
        return {};

    std::vector<float> mono ((size_t) length, 0.0f);

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto* in = audio.getReadPointer (ch, range.getStart());

        for (int i = 0; i < length; ++i)
            mono[(size_t) i] += in[i];
    }

    std::vector<float> window ((size_t) size);

    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));

    juce::dsp::FFT fft (order);
    const auto frames = (length - size) / hop + 1;
    std::vector<float> work (2 * size), previous (size / 2, 0.0f), flux ((size_t) frames, 0.0f);

    for (int f = 0; f < frames; ++f)
    {
        std::fill (work.begin(), work.end(), 0.0f);

        for (int i = 0; i < size; ++i)
            work[(size_t) i] = mono[(size_t) (f * hop + i)] * window[(size_t) i];

        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        float sum = 0.0f;

        for (int k = 1; k < (maxBin > 0 ? std::min (maxBin, size / 2) : size / 2); ++k)
        {
            const auto m = std::log (1.0f + 100.0f * work[(size_t) k]);
            sum += std::max (0.0f, m - previous[(size_t) k]);
            previous[(size_t) k] = m;
        }

        flux[(size_t) f] = f > 0 ? sum : 0.0f;
    }

    // Keep only what stands above the local (~0.4 s) average.
    const auto w = std::max (1, (int) std::lround (0.4 / hopSeconds));
    std::vector<double> prefix ((size_t) frames + 1, 0.0);

    for (int i = 0; i < frames; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + flux[(size_t) i];

    std::vector<float> onset ((size_t) frames);

    for (int i = 0; i < frames; ++i)
    {
        const auto a = std::max (0, i - w), b = std::min (frames, i + w + 1);
        onset[(size_t) i] = std::max (0.0f, flux[(size_t) i] - (float) ((prefix[(size_t) b] - prefix[(size_t) a]) / (b - a)));
    }

    return onset;
}
} // namespace

std::optional<double> measureTempo (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double rate)
{
    // ~5.8 ms steps whatever the sample rate.
    const auto hop = std::max (64, juce::roundToInt (rate * 256.0 / 44100.0));
    const auto hopSeconds = (double) hop / rate;
    const auto onset = onsetEnvelope (audio, range, hop, hopSeconds);
    const auto frames = (int) onset.size();
    constexpr double minBpm = 60.0, maxBpm = 200.0;

    // Needs a few bars: 4 beats at the slowest tempo, twice over.
    if (frames < (int) (8.0 * 60.0 / minBpm / hopSeconds) / 2)
        return {};

    // Autocorrelation up to 4 beats of the slowest tempo.
    const auto maxLag = std::min (frames - 2, (int) std::ceil (4.0 * 60.0 / minBpm / hopSeconds) + 2);
    std::vector<double> ac ((size_t) maxLag + 2, 0.0);

    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double s = 0.0;

        for (int i = 0; i + lag < frames; ++i)
            s += (double) onset[(size_t) i] * onset[(size_t) (i + lag)];

        ac[(size_t) lag] = s / (frames - lag);
    }

    auto acAt = [&] (double x)
    {
        if (x < 1.0 || x >= maxLag)
            return 0.0;

        const auto i = (int) x;
        const auto frac = x - i;
        return ac[(size_t) i] * (1.0 - frac) + ac[(size_t) i + 1] * frac;
    };

    // Every candidate tempo: its beat, 2, 3 and 4 beats (the bar), and half a
    // beat (the eighths most music has; this is what tells 80 from 120 when
    // the hi-hat plays eighths). A gentle preference for 80..160.
    double best = 0.0, bestScore = -1.0, total = 0.0;
    int count = 0;

    for (double bpm = minBpm; bpm <= maxBpm; bpm += 0.05)
    {
        const auto lag = 60.0 / bpm / hopSeconds;
        const auto prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 115.0) / 0.85, 2.0));
        const auto score = (acAt (lag) + 0.5 * acAt (2.0 * lag) + 0.33 * acAt (3.0 * lag) + 0.25 * acAt (4.0 * lag)
                            + 0.4 * acAt (0.5 * lag)) * prior;
        total += score;
        ++count;

        if (score > bestScore)
        {
            bestScore = score;
            best = bpm;
        }
    }

    // No clear peak: no steady beat.
    if (bestScore <= 0.0 || bestScore < 1.25 * total / count)
        return {};

    // Refine: the tempo whose beat grid, at its best phase, collects the most
    // onset strength over the whole range (precise to a few hundredths over
    // a minute of music).
    auto onsetAt = [&] (double x)
    {
        const auto i = (int) x;

        if (i < 0 || i + 1 >= frames)
            return 0.0;

        const auto frac = x - i;
        return (double) onset[(size_t) i] * (1.0 - frac) + (double) onset[(size_t) i + 1] * frac;
    };

    double refined = best, refinedScore = -1.0;

    for (double bpm = best * 0.985; bpm <= best * 1.015; bpm += 0.01)
    {
        const auto period = 60.0 / bpm / hopSeconds;

        for (double phase = 0.0; phase < period; phase += 0.5)
        {
            double sum = 0.0;

            for (double x = phase; x < frames; x += period)
                sum += onsetAt (x) + 0.5 * onsetAt (x + period * 0.5);

            if (sum > refinedScore)
            {
                refinedScore = sum;
                refined = bpm;
            }
        }
    }

    // To 0.1 BPM; whole numbers when it's that close to one.
    const auto rounded = std::round (refined);
    return std::abs (refined - rounded) < 0.15 ? rounded : std::round (refined * 10.0) / 10.0;
}
std::optional<juce::int64> findFirstDownbeat (const juce::AudioBuffer<float>& audio, double rate, double bpm)
{
    if (bpm <= 0.0 || audio.getNumSamples() < (int) (rate * 4.0))
        return {};

    const auto hop = std::max (64, juce::roundToInt (rate * 256.0 / 44100.0));
    const auto hopSeconds = (double) hop / rate;
    const juce::Range<int> all (0, audio.getNumSamples());
    const auto onset = onsetEnvelope (audio, all, hop, hopSeconds);
    const auto frames = (int) onset.size();

    if (frames < 16)
        return {};

    auto at = [frames] (const std::vector<float>& v, double x)
    {
        const auto i = (int) x;

        if (i < 0 || i + 1 >= frames)
            return 0.0;

        const auto frac = x - i;
        return (double) v[(size_t) i] * (1.0 - frac) + (double) v[(size_t) i + 1] * frac;
    };

    // Beat phase: where a grid of this tempo collects the most onset strength.
    const auto period = 60.0 / bpm / hopSeconds;
    double phase = 0.0, bestSum = -1.0;

    for (double p = 0.0; p < period; p += 0.25)
    {
        double sum = 0.0;

        for (double x = p; x < frames; x += period)
            sum += at (onset, x);

        if (sum > bestSum)
        {
            bestSum = sum;
            phase = p;
        }
    }

    // Which of the four beats is the downbeat: the one with the most bass
    // energy (kick drum) just after it, summed over every bar. Low-passed
    // twice at ~120 Hz so snares and hi-hats don't count.
    std::vector<float> bass ((size_t) audio.getNumSamples(), 0.0f);
    {
        const auto a = (float) std::exp (-2.0 * juce::MathConstants<double>::pi * 120.0 / rate);
        float s1 = 0.0f, s2 = 0.0f;

        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            float x = 0.0f;

            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                x += audio.getSample (ch, i);

            s1 = x + a * (s1 - x);
            s2 = s1 + a * (s2 - s1);
            bass[(size_t) i] = s2 * s2;
        }
    }

    const auto window = (int) (0.08 * rate);
    auto bassAt = [&] (double sample)
    {
        const auto start = (int) sample;

        if (start < 0 || start + window >= (int) bass.size())
            return 0.0;

        double sum = 0.0;

        for (int i = 0; i < window; ++i)
            sum += bass[(size_t) (start + i)];

        return sum;
    };

    int downbeat = 0;
    double bestLow = -1.0;
    const auto beatSamples = period * hopSeconds * rate, barSamples = 4.0 * beatSamples;
    const auto phaseSamples = phase * hopSeconds * rate;

    for (int j = 0; j < 4; ++j)
    {
        double sum = 0.0;

        for (double x = phaseSamples + j * beatSamples; x < (double) audio.getNumSamples(); x += barSamples)
            sum += bassAt (x);

        if (sum > bestLow)
        {
            bestLow = sum;
            downbeat = j;
        }
    }

    // Frames → samples. A flux frame peaks when the onset sits inside the
    // analysis window; refine on the waveform itself: around each of the
    // first downbeats, find the sharpest rise in short-time energy and move
    // the grid by the median correction.
    auto estimate = (phase + downbeat * period) * hopSeconds * rate;
    const auto periodSamples = 60.0 / bpm * rate;
    const auto bar = 4.0 * periodSamples;
    constexpr int block = 32;
    const auto blocks = audio.getNumSamples() / block;
    std::vector<float> energy ((size_t) blocks, 0.0f);

    for (int b = 0; b < blocks; ++b)
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            energy[(size_t) b] += std::sqrt (audio.getRMSLevel (ch, b * block, block));

    std::vector<double> corrections;
    const auto search = periodSamples * 0.25;

    for (int n = 0; n < 32; ++n)
    {
        const auto centre = estimate + n * bar;

        if (centre + search >= audio.getNumSamples())
            break;

        if (centre - search < 0)
            continue;

        int bestBlock = -1;
        float bestRise = 0.0f;

        for (auto b = (int) ((centre - search) / block); b < (int) ((centre + search) / block) && b + 1 < blocks; ++b)
        {
            const auto rise = energy[(size_t) b + 1] - energy[(size_t) b];

            if (rise > bestRise)
            {
                bestRise = rise;
                bestBlock = b + 1;
            }
        }

        if (bestBlock >= 0)
            corrections.push_back (bestBlock * (double) block - centre);
    }

    if (! corrections.empty())
    {
        std::sort (corrections.begin(), corrections.end());
        estimate += corrections[corrections.size() / 2];
    }

    // The earliest downbeat in the file (a hair before 0 counts as 0).
    const auto slack = 0.003 * rate;

    while (estimate - bar >= -slack)
        estimate -= bar;

    while (estimate < -slack)
        estimate += bar;

    return (juce::int64) std::llround (juce::jmax (0.0, estimate));
}
} // namespace cdj
