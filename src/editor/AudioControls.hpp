#pragma once

#include "green/Value.hpp"

#include <functional>
#include <optional>

namespace openstone {

enum class AudioApi { Any, DirectSound, Asio };
inline constexpr int maxAsioChannels = 256;

struct AudioDeviceInfo {
    int id;
    std::string name;
    AudioApi api = AudioApi::Any;
    int inputs = 0;
    int outputs = 0;
    bool operator==(const AudioDeviceInfo&) const = default;
};

struct AudioDeviceSetup {
    std::optional<int> input;
    std::optional<int> output;
    std::optional<int> directSoundInput;
    bool directSoundInputDisabled = false;
    bool inputDisabled = false;
    bool outputDisabled = false;
    double rate = 0;
    AudioApi inputApi = AudioApi::Any;
    AudioApi outputApi = AudioApi::Any;
    bool operator==(const AudioDeviceSetup&) const = default;
};

struct AsioBufferSizes {
    long minimum = 0;
    long maximum = 0;
    long preferred = 0;
    long granularity = 0;
    bool operator==(const AsioBufferSizes&) const = default;
};

struct AudioControlState {
    std::vector<AudioDeviceInfo> devices;
    AudioDeviceSetup setup;
    std::optional<int> selectedDevice;
    green::StringArray inputs;
    green::StringArray outputs;
    AsioBufferSizes asioBufferSizes;
    bool applying = false;
    std::function<bool(const AudioDeviceSetup&)> apply;
    std::function<void()> asioSettings;
};

}
