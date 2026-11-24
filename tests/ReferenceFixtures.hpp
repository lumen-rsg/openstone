#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace openstone::tests {

inline const std::vector<std::string>& missingReferenceSources()
{
    static const auto missing = [] {
        const auto directory = std::filesystem::path(OPENSTONE_TEST_FIXTURES) / "flowstone";
        std::ifstream manifest(directory / "external-sources.txt");
        if (!manifest) throw std::runtime_error("Missing external reference source manifest");
        std::vector<std::string> result;
        std::string name;
        std::size_t count = 0;
        while (std::getline(manifest, name)) {
            if (name.empty() || name.front() == '#') continue;
            if (std::filesystem::path(name).filename() != name)
                throw std::runtime_error("Invalid external reference source name");
            ++count;
            if (!std::filesystem::is_regular_file(directory / name)) result.push_back(name);
        }
        if (manifest.bad() || count == 0)
            throw std::runtime_error("Invalid external reference source manifest");
        return result;
    }();
    return missing;
}

template<class Function>
void withReferenceSources(const char* name, Function&& function)
{
    const auto& missing = missingReferenceSources();
    if (missing.empty()) {
        function();
        return;
    }
    const auto* required = std::getenv("OPENSTONE_REQUIRE_FLOWSTONE_FIXTURES");
    if (required && std::string(required) == "1")
        throw std::runtime_error("Private FlowStone reference corpus is incomplete: " + missing.front());
    std::cout << "SKIP private FlowStone reference: " << name
              << " (" << missing.size() << " omitted sources; see docs/testing.md)\n";
}

} // namespace openstone::tests
