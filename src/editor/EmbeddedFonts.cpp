#include "Schematic.hpp"

#include <stdexcept>

namespace openstone {

void Schematic::receiveFontEmbed(SchematicNode& entry)
{
    const auto paths = std::get<green::StringArray>(readInput(entry.id, 0));
    auto& root = rootGraph();
    const auto& files = root.textFiles_;
    const auto& registerFonts = root.systemInfo_.registerFonts;
    bool loaded = false;
    try {
        if (paths.empty()) {
            entry.embeddedFonts = {};
        } else if (registerFonts && (files.loadBounded || files.load)) {
            auto data = std::make_shared<std::vector<std::string>>();
            std::size_t remaining = maxEmbeddedFontBytes;
            for (const auto& path : paths) {
                if (path.empty() || path.find('\0') != path.npos) throw std::runtime_error("Choose a font file");
                auto bytes = files.loadBounded ? files.loadBounded(path, remaining) : files.load(path);
                if (bytes.empty() || bytes.size() > remaining) throw std::runtime_error("Invalid embedded font size");
                remaining -= bytes.size();
                data->push_back(std::move(bytes));
            }
            if (auto registration = registerFonts(*data)) {
                entry.embeddedFonts = {std::move(data), std::move(registration)};
                loaded = true;
            }
        }
        root.systemFontNames_.reset();
    } catch (const std::exception&) {
        // Keep the fonts already embedded if the new list cannot be loaded as a whole.
    }
    entry.capturedSignals.assign(1, loaded);
    fireOutput(entry, 0);
}

void Schematic::primeEmbeddedFonts()
{
    auto& root = rootGraph();
    const auto& registerFonts = root.systemInfo_.registerFonts;
    if (!registerFonts) return;
    for (auto& entry : nodes_) {
        auto& fonts = entry.embeddedFonts;
        if (!fonts.data || fonts.registration) continue;
        try {
            fonts.registration = registerFonts(*fonts.data);
            root.systemFontNames_.reset();
        } catch (const std::exception&) {
            // The embedded data stays in the project if this host cannot register it.
        }
    }
    for (auto& [id, module] : modules_) module->primeEmbeddedFonts();
}

}
