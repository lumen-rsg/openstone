#include "PresetText.hpp"

#include "PluginHost.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace openstone {
namespace {

using namespace green;
constexpr std::string_view tableTitle = "Preset Table Format";
constexpr std::string_view orderedTableTitle = "Preset Table Format with Ordering";
constexpr std::string_view escapedTableTitle = "Preset Table Format (OpenStone escaped)";
constexpr std::size_t maxTextBytes = 16 * 1024 * 1024;

[[noreturn]] void invalid()
{
    throw std::invalid_argument("Invalid preset text table");
}

bool needsEscape(std::string_view cell)
{
    return cell.find_first_of("\t\r\n") != cell.npos;
}

class Writer {
public:
    Writer(std::string& text, bool escaped) : text_(text), escaped_(escaped) {}

    void cell(std::string_view cell)
    {
        if (column_++ > 0) text_.push_back('\t');
        if (!escaped_) {
            text_ += cell;
            return;
        }
        for (const char c : cell) {
            switch (c) {
            case '\\': text_ += "\\\\"; break;
            case '\t': text_ += "\\t"; break;
            case '\n': text_ += "\\n"; break;
            case '\r': text_ += "\\r"; break;
            default: text_.push_back(c); break;
            }
        }
    }

    void endRow()
    {
        text_.push_back('\n');
        column_ = 0;
        if (text_.size() > maxTextBytes) throw std::invalid_argument("Preset text exceeds the size limit");
    }

private:
    std::string& text_;
    bool escaped_;
    int column_ = 0;
};

class Reader {
public:
    explicit Reader(std::string_view text) : remaining_(text) {}

    std::optional<std::string_view> nextLine()
    {
        if (remaining_.empty()) return std::nullopt;
        const auto end = remaining_.find('\n');
        auto line = remaining_.substr(0, end);
        remaining_.remove_prefix(end == line.npos ? line.size() : end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        return line;
    }

    std::string_view requireLine()
    {
        const auto line = nextLine();
        if (!line) invalid();
        return *line;
    }

    std::vector<std::string_view> row(std::string_view line, std::size_t width) const
    {
        std::vector<std::string_view> cells;
        cells.reserve(width);
        for (std::size_t column = 0; column < width; ++column) {
            const auto end = line.find('\t');
            cells.push_back(line.substr(0, end));
            if (column == width - 1) {
                if (end != line.npos) invalid();
            } else {
                if (end == line.npos) invalid();
                line.remove_prefix(end + 1);
            }
        }
        return cells;
    }

private:
    std::string_view remaining_;
};

std::string unescape(std::string_view cell, bool escaped)
{
    if (!escaped) return std::string(cell);
    std::string value;
    for (std::size_t i = 0; i < cell.size(); ++i) {
        if (cell[i] != '\\') {
            value.push_back(cell[i]);
            continue;
        }
        if (++i == cell.size()) invalid();
        switch (cell[i]) {
        case 't': value.push_back('\t'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case '\\': value.push_back('\\'); break;
        default: invalid();
        }
    }
    return value;
}

int integer(std::string_view text)
{
    if (text.empty()) invalid();
    int value{};
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (status != std::errc{} || end != text.data() + text.size()) invalid();
    return value;
}

int readCount(Reader& reader, std::string_view prefix, int maximum)
{
    auto text = reader.requireLine();
    if (!text.starts_with(prefix)) invalid();
    text.remove_prefix(prefix.size());
    const int count = integer(text);
    if (count < 0 || count > maximum) invalid();
    return count;
}

float readFloat(std::string_view cell)
{
    if (cell.empty()) invalid();
    if (cell.starts_with('+')) cell.remove_prefix(1);
    float value{};
    const auto [end, status] = std::from_chars(cell.data(), cell.data() + cell.size(), value);
    if (status != std::errc{} || end != cell.data() + cell.size() || !std::isfinite(value) || value < 0 || value > 1) {
        invalid();
    }
    return value;
}

void writeFloat(Writer& writer, float value)
{
    if (!std::isfinite(value) || value < 0 || value > 1) invalid();
    char buffer[64];
    const auto [end, status] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (status != std::errc{}) invalid();
    writer.cell({buffer, end});
}

bool readBoolean(std::string_view cell)
{
    if (cell == "True" || cell == "true") return true;
    if (cell == "False" || cell == "false") return false;
    return readFloat(cell) >= .5f;
}

std::optional<int> arraySize(std::string_view label)
{
    if (!label.starts_with("Float Array (") && !label.starts_with("float array (")) return std::nullopt;
    if (!label.ends_with(')')) invalid();
    const int size = integer(label.substr(13, label.size() - 14));
    if (size < 0 || size > maxPresetArraySize) invalid();
    return size;
}

}

std::string writePresetText(const PresetTable& table)
{
    const auto columns = table.programs.size();
    if (!columns || columns > maxPresetPrograms || table.parameters.size() > maxPresetTableCells / columns) invalid();

    bool escaped = std::ranges::any_of(table.programs, needsEscape);
    std::set<std::string_view> names;
    std::size_t rows = table.parameters.size();
    for (const auto& parameter : table.parameters) {
        if (parameter.name.empty() || !names.insert(parameter.name).second || parameter.values.size() != columns) invalid();
        escaped |= needsEscape(parameter.name);
        const auto type = typeOf(parameter.values.front());
        std::size_t elements = 0;
        for (const auto& value : parameter.values) {
            if (typeOf(value) != type) invalid();
            if (const auto* text = std::get_if<std::string>(&value)) escaped |= needsEscape(*text);
            if (const auto* array = std::get_if<FloatArray>(&value)) elements = std::max(elements, array->size());
        }
        if (elements > maxPresetArraySize || (rows += elements) > maxPresetTableCells / columns) invalid();
    }

    std::string text(escaped ? escapedTableTitle : tableTitle);
    text += "\nNumber of programs = " + std::to_string(columns);
    text += "\nNumber of parameters = " + std::to_string(table.parameters.size()) + "\n\n";
    Writer writer(text, escaped);
    writer.cell("Parameter");
    writer.cell("Type");
    for (const auto& name : table.programs) writer.cell(name);
    writer.endRow();
    for (const auto& parameter : table.parameters) {
        writer.cell(parameter.name);
        const auto type = typeOf(parameter.values.front());
        if (type == Type::FloatArray) {
            std::size_t size = 0;
            for (const auto& value : parameter.values) size = std::max(size, std::get<FloatArray>(value).size());
            writer.cell("Float Array (" + std::to_string(size) + ")");
            for (std::size_t column = 0; column < columns; ++column) writer.cell("");
            writer.endRow();
            for (std::size_t element = 0; element < size; ++element) {
                writer.cell(std::to_string(element + 1));
                writer.cell("");
                for (const auto& value : parameter.values) {
                    const auto& array = std::get<FloatArray>(value);
                    if (element < array.size()) writeFloat(writer, array[element]);
                    else writer.cell("");
                }
                writer.endRow();
            }
        } else {
            // The original table represents numeric parameters as normalized floats.
            if (type == Type::Float || type == Type::Boolean) writer.cell("Float");
            else if (type == Type::String) writer.cell("String");
            else invalid();
            for (const auto& value : parameter.values) {
                if (type == Type::Float) writeFloat(writer, std::get<float>(value));
                else if (type == Type::Boolean) writer.cell(std::get<bool>(value) ? "1" : "0");
                else writer.cell(std::get<std::string>(value));
            }
            writer.endRow();
        }
    }
    return text;
}

PresetTable readPresetText(std::string_view text)
{
    if (text.size() > maxTextBytes) invalid();
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    Reader reader(text);
    const auto title = reader.requireLine();
    if (title != tableTitle && title != orderedTableTitle && title != escapedTableTitle) invalid();
    const bool ordered = title == orderedTableTitle;
    const bool escaped = title == escapedTableTitle;
    const int programs = readCount(reader, "Number of programs = ", maxPresetPrograms);
    if (programs == 0) invalid();
    const auto rowLimit = maxPresetTableCells / programs;
    const int count = readCount(reader, "Number of parameters = ", static_cast<int>(rowLimit));
    const int firstProgram = ordered ? 3 : 2;
    auto heading = reader.requireLine();
    if (heading.empty()) heading = reader.requireLine();
    const auto header = reader.row(heading, firstProgram + programs);
    if (header[0] != "Parameter" || header[1] != "Type" || (ordered && header[2] != "Order")) invalid();

    PresetTable table;
    for (int i = 0; i < programs; ++i) table.programs.push_back(unescape(header[firstProgram + i], escaped));
    std::set<std::string, std::less<>> names;
    std::size_t rows = count;
    for (int i = 0; i < count; ++i) {
        const auto cells = reader.row(reader.requireLine(), header.size());
        auto name = unescape(cells[0], escaped);
        if (name.empty() || !names.insert(name).second) invalid();
        auto& parameter = table.parameters.emplace_back(std::move(name), std::vector<Value>{});
        const auto type = cells[1];
        if (const auto size = arraySize(type)) {
            if ((rows += *size) > rowLimit) invalid();
            parameter.values.resize(programs, FloatArray{});
            int firstIndex = 0;
            for (int element = 0; element < *size; ++element) {
                const auto data = reader.row(reader.requireLine(), header.size());
                const auto index = integer(data[0]);
                if (element == 0) firstIndex = index;
                if (firstIndex < 0 || firstIndex > 1 || index != firstIndex + element) invalid();
                for (int j = 0; j < programs; ++j) {
                    const auto cell = data[firstProgram + j];
                    if (cell.empty()) continue;
                    auto& array = std::get<FloatArray>(parameter.values[j]);
                    if (!std::cmp_equal(array.size(), element)) invalid();
                    array.push_back(readFloat(cell));
                }
            }
        } else {
            for (int j = 0; j < programs; ++j) {
                const auto cell = cells[firstProgram + j];
                if (type == "Float" || type == "float") parameter.values.emplace_back(readFloat(cell));
                else if (type == "String" || type == "string") parameter.values.emplace_back(unescape(cell, escaped));
                else if (type == "Bool" || type == "Boolean") parameter.values.emplace_back(readBoolean(cell));
                else invalid();
            }
        }
    }
    while (const auto line = reader.nextLine()) {
        if (!line->empty()) invalid();
    }
    return table;
}

}
