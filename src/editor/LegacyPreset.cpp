#include "LegacyPreset.hpp"

#include "PluginHost.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace openstone {
namespace {

constexpr std::string_view namedTag = "OSP1";
constexpr std::size_t programNameBytes = 28;

[[noreturn]] void invalid()
{
    throw std::invalid_argument("Invalid or unsupported FXP/FXB file");
}

class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    std::string_view take(std::size_t size)
    {
        if (size > data_.size()) invalid();
        const auto bytes = data_.substr(0, size);
        data_.remove_prefix(size);
        return bytes;
    }

    std::uint32_t number()
    {
        std::uint32_t result = 0;
        for (unsigned char byte : take(4)) result = (result << 8) | byte;
        return result;
    }

    int count(std::size_t maximum)
    {
        const auto size = number();
        if (size > maximum) invalid();
        return static_cast<int>(size);
    }

    std::uint32_t chunkSize()
    {
        if (take(4) != "CcnK") invalid();
        return number();
    }

    std::string name()
    {
        const auto field = take(programNameBytes);
        return std::string(field.substr(0, field.find('\0')));
    }

    [[nodiscard]] std::size_t size() const { return data_.size(); }

private:
    std::string_view data_;
};

void appendNumber(std::string& bytes, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<char>((value >> shift) & 0xff));
}

void finishChunk(std::string& bytes, const std::string& body)
{
    if (body.size() > maxLegacyPresetBytes - 8 || bytes.size() > maxLegacyPresetBytes - 8 - body.size()) invalid();
    bytes += "CcnK";
    appendNumber(bytes, static_cast<std::uint32_t>(body.size()));
    bytes += body;
}

void appendName(std::string& bytes, std::string_view name)
{
    std::size_t size = name.find('\0');
    if (size == name.npos) size = name.size();
    if (size >= programNameBytes) {
        size = programNameBytes - 1;
        while (size && (static_cast<unsigned char>(name[size]) & 0xc0) == 0x80) --size;
    }
    bytes += name.substr(0, size);
    bytes.append(programNameBytes - size, '\0');
}

void appendNormalized(std::string& body, float value)
{
    if (!std::isfinite(value) || value < 0 || value > 1) invalid();
    appendNumber(body, std::bit_cast<std::uint32_t>(value));
}

void readNamedPayload(Reader& reader, LegacyPreset& preset)
{
    auto data = reader.take(reader.number());
    if (!data.starts_with(namedTag)) invalid();
    data.remove_prefix(namedTag.size());
    preset.namedTable.emplace(data);
}

LegacyPreset::Program readProgram(Reader& reader, std::uint32_t parameters)
{
    auto name = reader.name();
    if (parameters > reader.size() / 4) invalid();
    LegacyPreset::Program program{std::move(name), {}};
    program.values.reserve(parameters);
    for (std::uint32_t i = 0; i < parameters; ++i) {
        const auto value = std::bit_cast<float>(reader.number());
        if (!std::isfinite(value) || value < 0 || value > 1) invalid();
        program.values.push_back(value);
    }
    return program;
}

std::string header(std::string_view magic, int format, const LegacyPreset& preset, std::size_t count)
{
    std::string body(magic);
    appendNumber(body, format);
    body += preset.id;
    appendNumber(body, preset.version);
    appendNumber(body, static_cast<std::uint32_t>(count));
    return body;
}

}

LegacyPreset readLegacyPreset(std::string_view bytes)
{
    if (bytes.size() > maxLegacyPresetBytes) invalid();
    Reader file(bytes);
    const auto size = file.chunkSize();
    Reader content(file.take(size == 0 ? file.size() : size));
    if (file.size()) invalid();
    const auto magic = content.take(4);
    const bool single = magic == "FxCk" || magic == "FPCh";
    const bool opaque = magic == "FPCh" || magic == "FBCh";
    if (!single && magic != "FxBk" && !opaque) invalid();
    const auto format = content.number();
    if (format > 2 || (single && format != 1)) invalid();

    LegacyPreset result;
    result.single = single;
    result.id = content.take(4);
    result.version = content.count(INT32_MAX);
    if (single) {
        const auto parameters = content.number();
        if (opaque) {
            result.programs.push_back({content.name(), {}});
            readNamedPayload(content, result);
        } else {
            result.programs.push_back(readProgram(content, parameters));
        }
    } else {
        const auto programs = content.count(maxPresetPrograms);
        if (!programs) invalid();
        const auto selected = content.number();
        if (format >= 2 && selected >= static_cast<std::uint32_t>(programs)) invalid();
        result.current = selected < static_cast<std::uint32_t>(programs) ? static_cast<int>(selected) : 0;
        content.take(124);
        if (opaque) {
            readNamedPayload(content, result);
            result.programs.resize(programs);
        } else {
            const auto appendProgram = [&](Reader& program) {
                if (program.take(4) != "FxCk" || program.number() != 1 || program.take(4) != result.id) invalid();
                static_cast<void>(program.number()); // Individual programs can have their own revision.
                result.programs.push_back(readProgram(program, program.number()));
            };
            for (int i = 0; i < programs; ++i) {
                const auto size = content.chunkSize();
                if (size == 0) {
                    // Some legacy files leave byteSize blank. The parameter count delimits the record.
                    appendProgram(content);
                } else {
                    Reader program(content.take(size));
                    appendProgram(program);
                    if (program.size()) invalid();
                }
            }
        }
    }
    if (content.size()) invalid();
    return result;
}

std::string writeLegacyPreset(const LegacyPreset& preset)
{
    if (preset.id.size() != 4 || preset.id.find('\0') != std::string::npos || preset.version < 0 ||
        preset.programs.empty() || preset.programs.size() > maxPresetPrograms ||
        preset.current < 0 || static_cast<std::size_t>(preset.current) >= preset.programs.size() ||
        (preset.single && preset.programs.size() != 1)) invalid();

    const auto appendProgram = [&](std::string& destination, const LegacyPreset::Program& program) {
        if (program.values.size() > maxLegacyPresetBytes / sizeof(float)) invalid();
        auto body = header("FxCk", 1, preset, program.values.size());
        appendName(body, program.name);
        for (const float value : program.values) appendNormalized(body, value);
        finishChunk(destination, body);
    };

    std::string bytes;
    if (preset.single && !preset.namedTable) {
        appendProgram(bytes, preset.programs.front());
        return bytes;
    }

    auto body = header(preset.single ? "FPCh" : preset.namedTable ? "FBCh" : "FxBk", preset.single ? 1 : 2,
                       preset, preset.single ? 0 : preset.programs.size());
    if (preset.single) {
        appendName(body, preset.programs.front().name);
    } else {
        appendNumber(body, preset.current);
        body.append(124, '\0');
    }
    if (preset.namedTable) {
        if (preset.namedTable->size() > maxLegacyPresetBytes - namedTag.size()) invalid();
        appendNumber(body, static_cast<std::uint32_t>(namedTag.size() + preset.namedTable->size()));
        body += namedTag;
        body += *preset.namedTable;
    } else {
        for (const auto& program : preset.programs) appendProgram(body, program);
    }
    finishChunk(bytes, body);
    return bytes;
}

}
