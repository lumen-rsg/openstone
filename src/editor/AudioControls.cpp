#include "Schematic.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>

namespace openstone {
namespace {

bool directSoundInput(const AudioDeviceInfo& device)
{
    return device.api == AudioApi::DirectSound && device.inputs > 0;
}

}

void Schematic::setAudioControlCallbacks(std::function<bool(const AudioDeviceSetup&)> apply,
                                         std::function<void()> asioSettings)
{
    auto& state = rootGraph().audioControls_;
    state.apply = std::move(apply);
    state.asioSettings = std::move(asioSettings);
}

void Schematic::setAudioDevices(std::vector<AudioDeviceInfo> devices)
{
    auto& root = rootGraph();
    if (root.audioControls_.devices == devices) return;
    root.audioControls_.devices = std::move(devices);
    root.notifyAudioControls(PrimitiveKind::AudioDevices);
    root.notifyAudioControls(PrimitiveKind::DirectSoundInDevices);
    root.notifyAudioControls(PrimitiveKind::AudioSelect);
    root.notifyAudioControls(PrimitiveKind::DirectSoundInSelect);
}

void Schematic::setAudioChannels(green::StringArray inputs, green::StringArray outputs)
{
    auto& root = rootGraph();
    auto& state = root.audioControls_;
    if (state.inputs == inputs && state.outputs == outputs) return;
    state.inputs = std::move(inputs);
    state.outputs = std::move(outputs);
    root.notifyAudioControls(PrimitiveKind::AudioInputsOutputs);
}

AudioDeviceSetup Schematic::audioDeviceSetup() const
{
    return rootGraph().audioControls_.setup;
}

void Schematic::notifyAudioControls(PrimitiveKind kind)
{
    for (auto& entry : nodes_) {
        if (entry.kind == kind) fireOutputs(entry.id);
    }
    for (auto& [id, child] : modules_) child->notifyAudioControls(kind);
}

green::Value Schematic::readAudioControl(const SchematicNode& entry, int port) const
{
    using enum PrimitiveKind;
    if (entry.kind == MonoToFrame) {
        return entry.capturedSignals.empty() ? green::Value{green::FloatArray{}} : entry.capturedSignals.front();
    }
    const auto& state = rootGraph().audioControls_;
    if (entry.kind == AudioInputsOutputs) return port == 0 ? state.inputs : state.outputs;
    const bool direct = entry.kind == DirectSoundInDevices || entry.kind == DirectSoundInSelect;
    auto devices = state.devices | std::views::filter([direct](const AudioDeviceInfo& info) {
        return !direct || directSoundInput(info);
    });
    if (entry.kind == AudioDevices || entry.kind == DirectSoundInDevices) {
        if (port == 0) return static_cast<int>(std::ranges::distance(devices));
        green::StringArray names;
        for (const auto& device : devices) names.push_back(device.name);
        return names;
    }
    const auto selected = direct ? state.setup.directSoundInput : state.selectedDevice;
    int index = 0;
    for (const auto& device : devices) {
        if (device.id == selected) return index;
        ++index;
    }
    return -1;
}

void Schematic::receiveAudioControl(SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    auto& root = rootGraph();
    auto& state = root.audioControls_;
    if (state.applying) return;
    const auto apply = [&](const AudioDeviceSetup& setup) {
        struct Applying {
            bool& flag;
            ~Applying() { flag = false; }
        } applying{state.applying};
        state.applying = true;
        return state.apply && state.apply(setup);
    };
    auto setup = state.setup;
    const auto integer = [&](int port) { return std::get<int>(readInput(entry.id, port)); };
    if (entry.kind == OpenAsioSettings) {
        if (state.asioSettings) state.asioSettings();
        return;
    }
    if (entry.kind == SetSampleRate) {
        if (input != 1) return;
        const int rate = integer(0);
        setup.rate = rate;
        if (rate <= 0 || !apply(setup)) {
            fireOutput(entry, 0);
            return;
        }
        state.setup = setup;
        root.setSampleRate(rate);
        return;
    }
    if (entry.kind != AudioSelect && entry.kind != DirectSoundInSelect) return;
    const int index = integer(0);
    const bool direct = entry.kind == DirectSoundInSelect;
    if (index < -1) return;
    auto devices = state.devices | std::views::filter([direct](const AudioDeviceInfo& info) {
        return !direct || directSoundInput(info);
    });
    std::optional<int> selected;
    if (index >= 0) {
        const auto it = std::ranges::next(devices.begin(), index, devices.end());
        if (it == devices.end()) return;
        const auto previous = direct ? setup.directSoundInput : state.selectedDevice;
        if (previous != it->id) {
            selected = it->id;
            if (direct) {
                setup.directSoundInput = selected;
                setup.directSoundInputDisabled = false;
            } else {
                setup.input = it->inputs > 0 ? selected : std::nullopt;
                setup.output = it->outputs > 0 ? selected : std::nullopt;
                setup.inputDisabled = false;
                setup.outputDisabled = false;
            }
        }
    }
    if (!selected) {
        if (direct) {
            setup.directSoundInput.reset();
            setup.directSoundInputDisabled = true;
        } else {
            setup.input.reset();
            setup.output.reset();
            setup.inputDisabled = setup.outputDisabled = true;
        }
    }
    if (setup != state.setup && state.apply && !apply(setup)) return;
    if (setup == state.setup && (direct || selected == state.selectedDevice)) return;
    state.setup = setup;
    if (!direct) state.selectedDevice = selected;
    root.notifyAudioControls(AudioSelect);
    root.notifyAudioControls(DirectSoundInSelect);
}

}
