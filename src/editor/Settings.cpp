#include "Session.hpp"

#include <stdexcept>

namespace openstone {
namespace {

long asioSetting(SystemSetting setting, const AsioBufferSizes& sizes)
{
    switch (setting) {
    case SystemSetting::AsioPreferredFrames: return sizes.preferred;
    case SystemSetting::AsioMinimumFrames: return sizes.minimum;
    case SystemSetting::AsioMaximumFrames: return sizes.maximum;
    case SystemSetting::AsioGranularity: return sizes.granularity;
    default: return 0;
    }
}

SystemSetting selectedSetting(const SchematicNode& node)
{
    return static_cast<SystemSetting>(std::get<std::int32_t>(*node.localValue));
}

}

float Schematic::readSystemSetting(const SchematicNode& entry) const
{
    const auto setting = selectedSetting(entry);
    if (setting == SystemSetting::DefaultZoom) return windowPixelsPerUnit;
    if (setting == SystemSetting::SampleRate) return static_cast<float>(sampleRate());
    return static_cast<float>(asioSetting(setting, rootGraph().audioControls_.asioBufferSizes));
}

void Schematic::setSystemSetting(int id, int index)
{
    auto& entry = node(id);
    if (entry.kind != PrimitiveKind::Settings || !validSystemSetting(index)) {
        throw std::invalid_argument("Choose a valid system setting");
    }
    if (selectedSetting(entry) == static_cast<SystemSetting>(index)) return;
    entry.localValue = static_cast<std::int32_t>(index);
    fireOutput(entry, 0);
    refreshConnections();
}

void Session::setSystemSetting(int id, int index)
{
    edit({id}, [index](auto& graph, const auto& ids) { graph.setSystemSetting(ids.front(), index); });
}

bool Schematic::setAsioBufferSizes(AsioBufferSizes sizes)
{
    auto& root = rootGraph();
    const auto previous = root.audioControls_.asioBufferSizes;
    if (previous == sizes) return false;
    root.audioControls_.asioBufferSizes = sizes;
    const auto publish = [&](const auto& walk, Schematic& graph) -> void {
        for (auto& entry : graph.nodes_) {
            if (entry.kind != PrimitiveKind::Settings) continue;
            const auto setting = selectedSetting(entry);
            if (asioSetting(setting, previous) != asioSetting(setting, sizes)) graph.fireOutput(entry, 0);
        }
        for (auto& [id, child] : graph.modules_) walk(walk, *child);
    };
    publish(publish, root);
    root.refreshConnections();
    return true;
}

void Schematic::setSettingsStoreBackend(SettingsStoreBackend backend)
{
    rootGraph().settingsStore_ = std::move(backend);
}

void Schematic::receiveSettingsStore(SchematicNode& entry, int input)
{
    if (input != 3 && input != 4) return;
    const auto group = std::get<std::string>(readInput(entry.id, 0));
    const auto key = std::get<std::string>(readInput(entry.id, 1));
    const auto& backend = rootGraph().settingsStore_;
    if (key.empty() || (input == 3 ? !backend.read : !backend.write)) {
        fireOutput(entry, 2);
        return;
    }

    std::optional<std::string> value;
    try {
        if (input == 3) {
            value = backend.read(group, key);
        } else {
            value = std::get<std::string>(readInput(entry.id, 2));
            backend.write(group, key, *value);
        }
    } catch (const std::exception&) {
        fireOutput(entry, 2);
        return;
    }

    entry.localValue = value.value_or(std::string{});
    fireOutput(entry, 0);
    fireOutput(entry, value ? 1 : 2);
}

}
