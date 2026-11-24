#include "SfzParser.hpp"
#include "Schematic.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <ranges>
#include <stdexcept>

namespace openstone {
namespace {

using green::Type;
using Opcodes = std::map<std::string_view, std::string_view, std::less<>>;
namespace fs = std::filesystem;
constexpr std::string_view whitespace = " \r\n\t";

constexpr auto columns = std::to_array<std::pair<std::string_view, float>>({
    {"sample", 0}, {"lokey", 0}, {"hikey", 127}, {"pitch_keycenter", 60},
    {"lovel", 0}, {"hivel", 127}, {"lochan", 1}, {"hichan", 16}, {"lorand", 0}, {"hirand", 1},
    {"loop_mode", 0}, {"loop_start", 0}, {"loop_end", 0},
    {"ampeg_attack", 0}, {"ampeg_decay", 0}, {"ampeg_delay", 0}, {"ampeg_hold", 0},
    {"ampeg_release", 0}, {"ampeg_sustain", 100},
    {"amplfo_delay", 0}, {"amplfo_depth", 0}, {"amplfo_freq", 0},
    {"cutoff", 0}, {"effect1", 0}, {"effect2", 0},
    {"fileg_attack", 0}, {"fileg_decay", 0}, {"fileg_delay", 0}, {"fileg_depth", 0},
    {"fileg_hold", 0}, {"fileg_release", 0}, {"fileg_sustain", 100},
    {"fillfo_delay", 0}, {"fillfo_depth", 0}, {"fillfo_freq", 0}, {"pan", 0},
    {"pitch_keytrack", 100}, {"resonance", 0}, {"transpose", 0}, {"volume", 0},
    {"offset", 0}, {"end", 0},
});

bool opcodeCharacter(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
}

std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(whitespace);
    return first == text.npos ? std::string_view{} : text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

fs::path filePath(std::string_view path)
{
    std::u8string normalized(path.begin(), path.end());
    std::ranges::replace(normalized, u8'\\', u8'/');
    return fs::path(normalized);
}

float number(std::string_view value, bool note)
{
    float result{};
    if (value.starts_with('+')) value.remove_prefix(1);
    const auto end = value.data() + value.size();
    if (const auto [next, error] = std::from_chars(value.data(), end, result);
        error == std::errc{} && next == end && std::isfinite(result)) return result;

    if (note && !value.empty()) {
        constexpr std::string_view names = "c d ef g a b";
        auto semitone = names.find(static_cast<char>(value.front() | 0x20));
        if (semitone != names.npos && value.front() != ' ') {
            value.remove_prefix(1);
            int pitch = static_cast<int>(semitone);
            if (value.starts_with('#') || value.starts_with('b')) {
                pitch += value.front() == '#' ? 1 : -1;
                value.remove_prefix(1);
            }
            int octave{};
            const auto [next, error] = std::from_chars(value.data(), end, octave);
            if (error == std::errc{} && next == end && octave >= -2 && octave <= 9) {
                return static_cast<float>(12 * (octave + 1) + pitch);
            }
        }
    }
    throw std::invalid_argument("Invalid SFZ opcode value");
}

float opcodeValue(const Opcodes& region, std::string_view name, float fallback)
{
    const auto field = region.find(name);
    if (field == region.end()) return fallback;
    if (name == "loop_mode") {
        constexpr std::array modes{"no_loop", "one_shot", "loop_continuous", "loop_sustain"};
        const auto mode = std::ranges::find(modes, field->second);
        if (mode == modes.end()) throw std::invalid_argument("Invalid SFZ loop mode");
        return static_cast<float>(mode - modes.begin());
    }
    return number(field->second, name == "lokey" || name == "hikey" || name == "pitch_keycenter");
}

}

std::span<const green::BusChannel> sfzChannels()
{
    static const auto channels = [] {
        std::vector<green::BusChannel> result;
        for (const auto& [key, fallback] : columns) {
            result.push_back({std::string(key), key == "sample" ? Type::StringArray : Type::FloatArray});
        }
        return result;
    }();
    return channels;
}

SfzState parseSfz(std::string_view source, std::string_view path)
{
    if (source.starts_with("\xef\xbb\xbf")) source.remove_prefix(3);
    SfzState result{.result = 0};
    result.columns.emplace_back(green::StringArray{});
    for (std::size_t i = 1; i < columns.size(); ++i) result.columns.emplace_back(green::FloatArray{});

    const auto folder = filePath(path).parent_path();
    Opcodes control, global, master, group, region, ignored;
    Opcodes* current = &ignored;
    const auto finishRegion = [&] {
        if (current != &region) return;
        const auto sample = region.find("sample");
        if (sample == region.end() || sample->second.empty()) throw std::invalid_argument("An SFZ region needs a sample");
        const auto prefix = control.find("default_path");
        const auto samplePath = sample->second.starts_with('*') ? filePath(sample->second)
            : folder / (prefix == control.end() ? fs::path{} : filePath(prefix->second)) / filePath(sample->second);
        const auto normalized = samplePath.lexically_normal().u8string();
        std::get<green::StringArray>(result.columns[0]).emplace_back(normalized.begin(), normalized.end());
        for (std::size_t column = 1; column < columns.size(); ++column) {
            const auto [name, fallback] = columns[column];
            std::get<green::FloatArray>(result.columns[column]).push_back(opcodeValue(region, name, fallback));
        }
    };

    while (!(source = trim(source)).empty()) {
        if (source.starts_with("//")) {
            const auto end = source.find('\n');
            if (end == source.npos) break;
            source.remove_prefix(end + 1);
            continue;
        }
        if (source.starts_with('<')) {
            finishRegion();
            const auto end = source.find('>');
            if (end == source.npos) throw std::invalid_argument("Unclosed SFZ header");
            const auto section = source.substr(1, end - 1);
            source.remove_prefix(end + 1);
            if (section == "control") {
                control.clear();
                current = &control;
            } else if (section == "global") {
                global.clear();
                master.clear();
                group.clear();
                current = &global;
            } else if (section == "master") {
                master = global;
                group.clear();
                current = &master;
            } else if (section == "group") {
                group = master.empty() ? global : master;
                current = &group;
            } else if (section == "region") {
                region = !group.empty() ? group : !master.empty() ? master : global;
                current = &region;
            } else {
                current = &ignored;
            }
            continue;
        }

        const auto keyEnd = std::ranges::find_if_not(source, opcodeCharacter);
        const auto keySize = static_cast<std::size_t>(keyEnd - source.begin());
        if (!keySize || keyEnd == source.end() || *keyEnd != '=') {
            throw std::invalid_argument("Expected an SFZ opcode");
        }
        const auto key = source.substr(0, keySize);
        source.remove_prefix(keySize + 1);
        auto valueEnd = std::min({source.find('\n'), source.find('<'), source.find("//"), source.size()});
        for (std::size_t i = 0; i < valueEnd; ++i) {
            if (!whitespace.contains(source[i])) continue;
            const auto start = source.find_first_not_of(whitespace, i);
            if (start >= valueEnd) break;
            const auto next = std::ranges::find_if_not(source.substr(start, valueEnd - start), opcodeCharacter);
            if (next != source.begin() + start && next != source.begin() + valueEnd && *next == '=') {
                valueEnd = i;
                break;
            }
            i = start - 1;
        }
        const auto value = trim(source.substr(0, valueEnd));
        source.remove_prefix(valueEnd);
        if (key == "key") {
            for (const auto related : {"lokey", "hikey", "pitch_keycenter"}) current->insert_or_assign(related, value);
        } else {
            current->insert_or_assign(key, value);
        }
    }
    finishRegion();
    if (std::get<green::StringArray>(result.columns.front()).empty()) {
        throw std::invalid_argument("The SFZ file contains no regions");
    }
    return result;
}

green::Value Schematic::readSfz(const SchematicNode& entry, int output) const
{
    if (output == 0) return entry.sfz ? entry.sfz->result : std::int32_t{1};
    return green::Bus{{sfzChannels().begin(), sfzChannels().end()}};
}

green::Value Schematic::readSfzColumn(const SchematicNode& entry, int column) const
{
    if (entry.sfz && !entry.sfz->columns.empty()) return entry.sfz->columns.at(column);
    if (column == 0) return green::StringArray{};
    return green::FloatArray{};
}

void Schematic::receiveSfz(SchematicNode& entry, int input)
{
    if (input != 1) return;
    entry.sfz.emplace();
    const auto path = std::get<std::string>(readInput(entry.id, 0));
    const auto& files = rootGraph().textFiles_;
    try {
        constexpr std::size_t limit = 8 * 1024 * 1024;
        const auto text = files.loadBounded ? files.loadBounded(path, limit) : files.load(path);
        entry.sfz->result = 2;
        if (text.size() <= limit) entry.sfz = parseSfz(text, path);
    } catch (const std::exception&) {
        // The result distinguishes opening errors from invalid SFZ content.
    }
    fireOutput(entry, 0);
    fireOutput(entry, 1);
}

}
