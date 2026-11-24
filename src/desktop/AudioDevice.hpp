#pragma once

#include "AudioControls.hpp"
#include "stream/AudioProcessor.hpp"

#include <portaudio.h>

namespace openstone::desktop {

class AudioDevice {
public:
    static constexpr std::size_t blockSize = 256;

    AudioDevice(std::shared_ptr<stream::AudioProcessor> processor, const AudioDeviceSetup& setup);
    [[nodiscard]] static std::vector<AudioDeviceInfo> devices();
    static void validate(const AudioDeviceSetup& setup);
    [[nodiscard]] green::StringArray channelNames(bool input) const;
    [[nodiscard]] AsioBufferSizes asioBufferSizes() const;
    void openAsioSettings(void* window) const;
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    [[nodiscard]] bool replaceProcessor(std::shared_ptr<stream::AudioProcessor> processor);
    void start();
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] bool active() const noexcept;

private:
    static int callback(const void* input, void* output, unsigned long frames,
                        const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* user) noexcept;
    void render(const float* const* input, float* const* output, std::size_t frames) noexcept;
    void close() noexcept;

    stream::RealtimeValue<std::shared_ptr<stream::AudioProcessor>> processors_;
    bool inputDisabled_ = false;
    PaStream* stream_ = nullptr;
    PaDeviceIndex inputDevice_ = paNoDevice;
    PaDeviceIndex outputDevice_ = paNoDevice;
    int inputChannels_ = 0;
    int outputChannels_ = 0;
    double sampleRate_ = 0;
    std::vector<std::span<const float>> inputs_;
    std::vector<std::span<float>> outputs_;
    std::array<float, blockSize> monoScratch_;
};

}
