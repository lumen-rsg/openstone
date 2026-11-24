#include "Schematic.hpp"

#include <stdexcept>

namespace openstone {
void Schematic::receiveSystemAction(SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    const auto& system = rootGraph().systemInfo_;
    const auto text = [&](int port) { return std::get<std::string>(readInput(entry.id, port)); };
    switch (entry.kind) {
    case WebUrl:
        if (input == 1 && system.openUrl && !system.openUrl(text(0))) {
            throw std::runtime_error("Could not open the URL");
        }
        break;
    case ShellExecute:
        if (input != 2 || !system.launchProgram) return;
        if (!system.launchProgram(text(0), text(1), std::get<bool>(readInput(entry.id, 3)))) {
            throw std::runtime_error("Could not launch the program");
        }
        fireOutput(entry, 0);
        break;
    case HardDiskSerial:
        entry.capturedSignals.assign(1, system.diskSerial ? system.diskSerial() : std::string{});
        fireOutput(entry, 0);
        break;
    case MacAddresses: {
        std::string addresses;
        if (system.macAddresses) {
            for (const auto& address : system.macAddresses()) {
                if (address.empty()) continue;
                if (!addresses.empty()) addresses += ',';
                addresses += address;
            }
        }
        entry.capturedSignals.assign(1, std::move(addresses));
        fireOutput(entry, 0);
        break;
    }
    default:
        break;
    }
}

}
