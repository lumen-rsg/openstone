#pragma once

#include "stream/HostTransport.hpp"

#include "green/Value.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openstone {

inline constexpr int maxPresetPrograms = 4096;
inline constexpr int maxPresetArraySize = 4096;

struct PresetManagerState {
    int program = 0;
    std::map<int, std::string> names;
    std::optional<std::string> editedName;
    // A command lives only for the synchronous Green cascade.
    green::PresetAction action = green::PresetAction::Recall;
    bool dispatching = false;
};

struct PresetParameterState {
    std::optional<green::Value> current;
    std::map<int, green::Value> saved;
    // Native .fsm Preset Float keeps an unconnected lock and a floating-point
    // ordering key. Its values are not clamped to the host's normalized range.
    struct FlowStoneFloat {
        bool locked = false;
        float order = -1;
        bool inactive = false; // Native bank has no addressable program yet.
    };
    std::optional<FlowStoneFloat> flowStoneFloat;
    bool dispatching = false;
};

inline constexpr int maxPluginAudioChannels = 256;

struct PluginAudioLayout {
    int mainInputs = 0;
    int mainOutputs = 0;
    int auxInputs = 0;
    int auxOutputs = 0;
};

struct PluginExportInfo {
    std::string type;
    std::string name;
    std::string vendor;
    std::optional<int> version;
    std::string vst2Id;
    std::string vst3Guid;
    std::string email;
    std::string url;
    std::optional<bool> resizable;
    std::optional<bool> createTables;
    std::optional<bool> embedDlls;
    std::string folder;
    std::optional<bool> vst2;
    std::optional<bool> vst3;
};

struct PluginHostInfo {
    std::string vendor;
    std::string product;
    std::string version;
    std::string pluginFolder; // Directory containing the installed plug-in.
    bool operator==(const PluginHostInfo&) const = default;
};

struct SchematicLocation {
    std::string filePath;
    std::optional<PluginHostInfo> pluginHost;
};

struct PluginParameterAddress {
    // The path contains module node IDs followed by the parameter node ID.
    std::vector<int> path;
    int element = 0;
};

struct PluginParameter {
    PluginParameterAddress address;
    std::string name;
    std::string display;
    std::string units;
    double order;
    float value;
    float initial;
    bool discrete;
    bool bypass = false;
};

// These callbacks and the Schematic host API run on the control thread.
struct PluginHostCallbacks {
    std::function<void(int samples)> latencyChanged;
    std::function<void(const PluginParameterAddress&, float)> recordParameter;
};

}
