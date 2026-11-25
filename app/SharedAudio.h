#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace cdj
{
// The audio device to play through. Empty in the standalone app (each player
// then opens the default output itself); Adormis Audio sets its own device
// here, so CDJ Export plays through the same output and never opens a second one.
inline juce::AudioDeviceManager*& sharedAudioDevices()
{
    static juce::AudioDeviceManager* shared = nullptr;
    return shared;
}
}
