#include "AudioDevice.hpp"

#if defined(OPENSTONE_PORTAUDIO_ASIO)
#include <pa_asio.h>
#endif

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace openstone::desktop {
namespace {

void check(PaError result, const char* operation)
{
    if (result != paNoError) {
        throw std::runtime_error(std::string(operation) + " (" + Pa_GetErrorText(result) + ")");
    }
}

float deviceSample(float sample)
{
    return std::isfinite(sample) ? std::clamp(sample, -1.0F, 1.0F) : 0.0F;
}

AudioApi audioApi(const PaDeviceInfo& info)
{
    const auto* api = Pa_GetHostApiInfo(info.hostApi);
    if (api && api->type == paDirectSound) return AudioApi::DirectSound;
    if (api && api->type == paASIO) return AudioApi::Asio;
    return AudioApi::Any;
}

PaDeviceIndex selectDevice(std::optional<int> selected, AudioApi api, bool input)
{
    if (selected) {
        const auto* info = Pa_GetDeviceInfo(*selected);
        if (info && (api == AudioApi::Any || audioApi(*info) == api)) return *selected;
    }
    if (api == AudioApi::Any) return input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
    const auto id = Pa_HostApiTypeIdToHostApiIndex(api == AudioApi::DirectSound ? paDirectSound : paASIO);
    if (const auto* host = Pa_GetHostApiInfo(id)) return input ? host->defaultInputDevice : host->defaultOutputDevice;
    return paNoDevice;
}

struct AudioFormat {
    PaStreamParameters input{};
    PaStreamParameters output{};
    double rate = 0;
};

AudioFormat audioFormat(const AudioDeviceSetup& setup, bool needsInput)
{
    AudioFormat format;
    if (setup.outputDisabled) throw std::runtime_error("Select an audio output device");
    const auto output = selectDevice(setup.output, setup.outputApi, false);
    const auto* outputInfo = Pa_GetDeviceInfo(output);
    if (!outputInfo || outputInfo->maxOutputChannels == 0) {
        throw std::runtime_error("No output device is available for the selected audio driver");
    }
    constexpr auto flags = paFloat32 | paNonInterleaved;
    format.output = {output, std::min(audioApi(*outputInfo) == AudioApi::Asio ? maxAsioChannels : 2, outputInfo->maxOutputChannels), flags,
                     outputInfo->defaultLowOutputLatency, nullptr};
    double inputRate = outputInfo->defaultSampleRate;
    if (needsInput && !setup.inputDisabled) {
        const auto input = selectDevice(setup.input, setup.inputApi, true);
        const auto* inputInfo = Pa_GetDeviceInfo(input);
        if (!inputInfo || inputInfo->maxInputChannels == 0) {
            throw std::runtime_error("No input device is available for the selected audio driver");
        }
        format.input = {input, std::min(audioApi(*inputInfo) == AudioApi::Asio ? maxAsioChannels : 2, inputInfo->maxInputChannels), flags,
                        inputInfo->defaultLowInputLatency, nullptr};
        inputRate = inputInfo->defaultSampleRate;
    }
    const auto* input = format.input.channelCount ? &format.input : nullptr;
    const std::array defaultRates{outputInfo->defaultSampleRate, inputRate, 48000.0, 44100.0};
    const auto rates = setup.rate > 0 ? std::span{&setup.rate, 1} : std::span<const double>{defaultRates};
    const auto rate = std::ranges::find_if(rates, [&](double candidate) {
        return Pa_IsFormatSupported(input, &format.output, candidate) == paFormatIsSupported;
    });
    if (rate == rates.end()) throw std::runtime_error("The audio devices do not support the requested format");
    format.rate = *rate;
    return format;
}

struct PortAudioSession {
    PortAudioSession() { check(Pa_Initialize(), "Could not initialize the audio devices"); }
    ~PortAudioSession() { Pa_Terminate(); }
};

}

AudioDevice::AudioDevice(std::shared_ptr<stream::AudioProcessor> processor, const AudioDeviceSetup& setup)
    : inputDisabled_(setup.inputDisabled)
{
    check(Pa_Initialize(), "Could not initialize the audio devices");
    try {
        const auto format = audioFormat(setup, processor->needsInput());
        inputChannels_ = format.input.channelCount;
        outputChannels_ = format.output.channelCount;
        inputDevice_ = inputChannels_ ? format.input.device : paNoDevice;
        outputDevice_ = format.output.device;
        inputs_.resize(inputChannels_ == 1 && setup.inputApi != AudioApi::Asio ? 2 : inputChannels_);
        outputs_.resize(processor->outputCount());
        processors_.writable() = std::move(processor);
        processors_.publish();
        check(Pa_OpenStream(&stream_, inputChannels_ ? &format.input : nullptr, &format.output,
                            format.rate, blockSize, paNoFlag, callback, this), "Could not open the audio stream");
        sampleRate_ = Pa_GetStreamInfo(stream_)->sampleRate;
        if (setup.rate > 0 && sampleRate_ != setup.rate) {
            throw std::runtime_error("The audio driver did not set the requested sample rate");
        }
    } catch (...) {
        close();
        throw;
    }
}

std::vector<AudioDeviceInfo> AudioDevice::devices()
{
    const PortAudioSession session;
    const auto count = Pa_GetDeviceCount();
    check(std::min(count, 0), "Could not list the audio devices");
    std::vector<AudioDeviceInfo> devices;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const auto* info = Pa_GetDeviceInfo(i);
        if (!info || (info->maxInputChannels == 0 && info->maxOutputChannels == 0)) continue;
        devices.push_back({i, info->name, audioApi(*info), info->maxInputChannels, info->maxOutputChannels});
    }
    return devices;
}

void AudioDevice::validate(const AudioDeviceSetup& setup)
{
    const PortAudioSession session;
    static_cast<void>(audioFormat(setup, setup.input.has_value()));
}

green::StringArray AudioDevice::channelNames(bool input) const
{
    const auto device = input ? inputDevice_ : outputDevice_;
    const auto channels = input ? inputChannels_ : outputChannels_;
    const auto* info = Pa_GetDeviceInfo(device);
    green::StringArray names;
    for (int i = 0; i < channels; ++i) {
#if defined(OPENSTONE_PORTAUDIO_ASIO)
        if (info && audioApi(*info) == AudioApi::Asio) {
            const char* name = nullptr;
            const auto getName = input ? PaAsio_GetInputChannelName : PaAsio_GetOutputChannelName;
            if (getName(device, i, &name) == paNoError && name) {
                names.emplace_back(name);
                continue;
            }
        }
#endif
        names.emplace_back(std::string(info ? info->name : "") + (input ? " Input " : " Output ") + std::to_string(i + 1));
    }
    return names;
}

AsioBufferSizes AudioDevice::asioBufferSizes() const
{
#if defined(OPENSTONE_PORTAUDIO_ASIO)
    for (const auto device : {outputDevice_, inputDevice_}) {
        const auto* info = Pa_GetDeviceInfo(device);
        if (!info || audioApi(*info) != AudioApi::Asio) continue;
        AsioBufferSizes sizes;
        if (PaAsio_GetAvailableBufferSizes(device, &sizes.minimum, &sizes.maximum,
                                          &sizes.preferred, &sizes.granularity) == paNoError) {
            return sizes;
        }
    }
#endif
    return {};
}

void AudioDevice::openAsioSettings([[maybe_unused]] void* window) const
{
#if defined(OPENSTONE_PORTAUDIO_ASIO)
    for (const auto device : {outputDevice_, inputDevice_}) {
        if (const auto* info = Pa_GetDeviceInfo(device); info && audioApi(*info) == AudioApi::Asio) {
            check(PaAsio_ShowControlPanel(device, window), "Could not open the ASIO control panel");
            return;
        }
    }
    throw std::runtime_error("Start an ASIO device before opening its control panel");
#else
    throw std::runtime_error("This OpenStone build does not include the ASIO control panel");
#endif
}

void AudioDevice::start()
{
    check(Pa_StartStream(stream_), "Could not start the audio stream");
}

AudioDevice::~AudioDevice()
{
    close();
}

void AudioDevice::close() noexcept
{
    if (stream_) {
        Pa_AbortStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    Pa_Terminate();
}

bool AudioDevice::active() const noexcept
{
    return Pa_IsStreamActive(stream_) == 1;
}

int AudioDevice::callback(const void* input, void* output, unsigned long frames,
                          const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* user) noexcept
{
    static_cast<AudioDevice*>(user)->render(static_cast<const float* const*>(input),
                                           static_cast<float* const*>(output), frames);
    return paContinue;
}

bool AudioDevice::replaceProcessor(std::shared_ptr<stream::AudioProcessor> processor)
{
    if (processor->outputCount() != outputs_.size() ||
        (processor->needsInput() && inputChannels_ == 0 && !inputDisabled_)) {
        return false;
    }
    // The callback holds a reference to its slot for the entire buffer.
    processors_.writable() = std::move(processor);
    processors_.publish();
    return true;
}

void AudioDevice::render(const float* const* input, float* const* output, std::size_t frames) noexcept
{
    const auto& processor = processors_.read();
    for (std::size_t offset = 0; offset < frames;) {
        const auto count = std::min(blockSize, frames - offset);
        for (std::size_t channel = 0; channel < inputs_.size(); ++channel) {
            inputs_[channel] = input ? std::span{input[std::min<int>(channel, inputChannels_ - 1)] + offset, count}
                                     : std::span<const float>{};
        }
        const bool downmix = outputChannels_ == 1 && outputs_.size() == 2;
        for (std::size_t channel = 0; channel < outputs_.size(); ++channel) {
            if (std::cmp_less(channel, outputChannels_)) outputs_[channel] = {output[channel] + offset, count};
            else if (downmix) outputs_[channel] = std::span{monoScratch_}.first(count);
            else outputs_[channel] = {};
        }
        if (!processor->processHost(inputs_, outputs_, count)) {
            for (auto channel : outputs_) std::ranges::fill(channel, 0.0F);
        }
        if (downmix) {
            for (std::size_t i = 0; i < count; ++i) {
                outputs_[0][i] = 0.5F * deviceSample(outputs_[0][i]) + 0.5F * deviceSample(outputs_[1][i]);
            }
        } else {
            for (auto channel : outputs_) std::ranges::transform(channel, channel.begin(), deviceSample);
        }
        for (std::size_t channel = outputs_.size(); std::cmp_less(channel, outputChannels_); ++channel) {
            std::ranges::fill(std::span{output[channel] + offset, count}, 0.0F);
        }
        offset += count;
    }
}

}
