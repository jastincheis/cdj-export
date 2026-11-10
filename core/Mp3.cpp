#include "Mp3.h"

#if CDJ_HAS_MP3
 #include <lame/lame.h>
#endif

namespace cdj
{
bool mp3EncodingAvailable()
{
    return CDJ_HAS_MP3 != 0;
}

#if CDJ_HAS_MP3
bool writeMp3 (const juce::File& destination, const juce::AudioBuffer<float>& audio, double sampleRate,
               int kbps, juce::String& error)
{
    std::unique_ptr<lame_global_flags, decltype (&lame_close)> lame (lame_init(), &lame_close);

    if (lame == nullptr)
    {
        error = "The MP3 encoder could not start.";
        return false;
    }

    const auto channels = juce::jmin (2, audio.getNumChannels());
    lame_set_in_samplerate (lame.get(), juce::roundToInt (sampleRate));
    lame_set_out_samplerate (lame.get(), juce::roundToInt (sampleRate));
    lame_set_num_channels (lame.get(), channels);
    lame_set_mode (lame.get(), channels == 1 ? MONO : JOINT_STEREO);
    lame_set_brate (lame.get(), kbps);
    lame_set_VBR (lame.get(), vbr_off);     // CBR: every CDJ seeks it exactly
    lame_set_quality (lame.get(), 2);
    lame_set_bWriteVbrTag (lame.get(), 1);  // the Info/LAME header with the encoder delay

    if (lame_init_params (lame.get()) < 0)
    {
        error = "The MP3 encoder does not support " + juce::String (juce::roundToInt (sampleRate)) + " Hz at "
              + juce::String (kbps) + " kbps.";
        return false;
    }

    std::unique_ptr<juce::FileOutputStream> out (destination.createOutputStream());

    if (out == nullptr || out->failedToOpen() || ! out->setPosition (0) || ! out->truncate().wasOk())
    {
        error = "Can't write to \"" + destination.getFullPathName() + "\".";
        return false;
    }

    constexpr int blockSize = 8192;
    constexpr int mp3Size = blockSize * 5 / 4 + 7200;   // LAME's recommended worst case
    juce::HeapBlock<unsigned char> mp3 ((size_t) mp3Size);

    const auto* left = audio.getReadPointer (0);
    const auto* right = audio.getReadPointer (channels > 1 ? 1 : 0);

    for (int start = 0; start < audio.getNumSamples(); start += blockSize)
    {
        const auto count = juce::jmin (blockSize, audio.getNumSamples() - start);
        const auto bytes = lame_encode_buffer_ieee_float (lame.get(), left + start, right + start, count, mp3, mp3Size);

        if (bytes < 0 || ! out->write (mp3, (size_t) bytes))
        {
            error = "MP3 encoding failed.";
            return false;
        }
    }

    const auto last = lame_encode_flush (lame.get(), mp3, mp3Size);

    if (last < 0 || ! out->write (mp3, (size_t) last))
    {
        error = "MP3 encoding failed.";
        return false;
    }

    out->flush();

    // The Info/LAME frame is written last, over the placeholder at the start.
    const auto tagSize = lame_get_lametag_frame (lame.get(), mp3, mp3Size);

    if (tagSize > 0 && tagSize <= mp3Size && out->setPosition (0))
        out->write (mp3, tagSize);

    out->flush();

    if (out->getStatus().failed())
    {
        error = "Can't write to \"" + destination.getFullPathName() + "\" (stick full?).";
        return false;
    }

    return true;
}
#else
bool writeMp3 (const juce::File&, const juce::AudioBuffer<float>&, double, int, juce::String& error)
{
    error = "This build has no MP3 encoder.";
    return false;
}
#endif

namespace
{
struct FrameInfo
{
    juce::int64 offset = -1;
    int version = 0;        // 1 = MPEG-1, 2 = MPEG-2 / 2.5
    bool mono = false;
    int kbps = 0;
    int sampleRate = 0;
};

FrameInfo firstFrame (const juce::MemoryBlock& data)
{
    const auto* b = static_cast<const juce::uint8*> (data.getData());
    const auto size = (juce::int64) data.getSize();
    juce::int64 pos = 0;

    // Skip an ID3v2 tag: "ID3", version, flags, 4 syncsafe size bytes.
    if (size > 10 && b[0] == 'I' && b[1] == 'D' && b[2] == '3')
        pos = 10 + (((juce::int64) b[6] & 0x7f) << 21 | ((juce::int64) b[7] & 0x7f) << 14
                    | ((juce::int64) b[8] & 0x7f) << 7 | ((juce::int64) b[9] & 0x7f));

    static constexpr int rates1[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static constexpr int rates2[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };

    for (; pos + 4 < size; ++pos)
    {
        if (b[pos] != 0xff || (b[pos + 1] & 0xe0) != 0xe0)
            continue;

        const auto versionBits = (b[pos + 1] >> 3) & 3;     // 3 = MPEG-1, 2 = MPEG-2, 0 = 2.5
        const auto layer = (b[pos + 1] >> 1) & 3;           // 1 = layer III
        const auto rateIndex = (b[pos + 2] >> 4) & 15;

        if (versionBits == 1 || layer != 1 || rateIndex == 0 || rateIndex == 15 || ((b[pos + 2] >> 2) & 3) == 3)
            continue;

        FrameInfo f;
        f.offset = pos;
        f.version = versionBits == 3 ? 1 : 2;
        f.mono = ((b[pos + 3] >> 6) & 3) == 3;
        f.kbps = f.version == 1 ? rates1[rateIndex] : rates2[rateIndex];
        static constexpr int sampleRates[] = { 44100, 48000, 32000 };
        const auto base = sampleRates[(b[pos + 2] >> 2) & 3];
        f.sampleRate = versionBits == 3 ? base : versionBits == 2 ? base / 2 : base / 4;
        return f;
    }

    return {};
}

juce::MemoryBlock readStart (const juce::File& file)
{
    juce::MemoryBlock data;
    juce::FileInputStream in (file);

    if (in.openedOk())
        in.readIntoMemoryBlock (data, 1 << 20);     // tags + first frame are well inside 1 MB… usually

    return data;
}
} // namespace

int mp3EncoderDelay (const juce::File& file)
{
    const auto data = readStart (file);
    const auto frame = firstFrame (data);

    if (frame.offset < 0)
        return -1;

    const auto* b = static_cast<const juce::uint8*> (data.getData());
    const auto size = (juce::int64) data.getSize();

    // Side information length decides where the Xing/Info header sits.
    const auto sideInfo = frame.version == 1 ? (frame.mono ? 17 : 32) : (frame.mono ? 9 : 17);
    auto p = frame.offset + 4 + sideInfo;

    if (p + 8 > size)
        return -1;

    const auto id = juce::String::fromUTF8 (reinterpret_cast<const char*> (b + p), 4);

    if (id != "Xing" && id != "Info")
        return -1;

    const auto flags = (juce::uint32) b[p + 4] << 24 | (juce::uint32) b[p + 5] << 16
                     | (juce::uint32) b[p + 6] << 8 | (juce::uint32) b[p + 7];
    p += 8;

    if (flags & 1) p += 4;      // frame count
    if (flags & 2) p += 4;      // byte count
    if (flags & 4) p += 100;    // seek table
    if (flags & 8) p += 4;      // quality

    // LAME extension: 9-byte encoder name, then 12 bytes of other fields,
    // then encoder delay (12 bits) and padding (12 bits).
    if (p + 24 > size)
        return -1;

    const auto encoder = juce::String::fromUTF8 (reinterpret_cast<const char*> (b + p), 4);

    if (encoder != "LAME" && encoder != "Lavc")
        return -1;

    const auto q = p + 21;
    return (int) ((juce::uint32) b[q] << 4 | (juce::uint32) b[q + 1] >> 4);
}

int mp3SampleRate (const juce::File& file)
{
    return firstFrame (readStart (file)).sampleRate;
}

int mp3Bitrate (const juce::File& file)
{
    return firstFrame (readStart (file)).kbps;
}
} // namespace cdj
