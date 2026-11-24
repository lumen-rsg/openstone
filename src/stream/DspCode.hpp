#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace openstone::stream {

enum class DspBackend { Interpreter, LlvmOrc };
enum class DspLanguage { Code, Assembler };
// The first arithmetic fault since reset; the faulting Assembler stage stops without committing its result.
enum class DspFaultKind { None, IntegerDivideByZero, IntegerDivideOverflow };
struct DspFault {
    DspFaultKind kind = DspFaultKind::None;
    std::uint64_t frame{};
    bool operator==(const DspFault&) const = default;
};
[[nodiscard]] DspBackend defaultDspBackend() noexcept;
[[nodiscard]] bool dspBackendAvailable(DspBackend backend) noexcept;

enum class DspWordKind { Port, Type, Section, Literal, Function, Register };

struct DspWord {
    std::string_view text;
    DspWordKind kind;
};

[[nodiscard]] std::span<const DspWord> dspLanguageWords(DspLanguage language = DspLanguage::Code);

enum class DspPortType {
    Stream, Mono, Poly, StreamBoolean, MonoBoolean, PolyBoolean,
    StreamDouble, MonoDouble, PolyDouble, PolyInt, Mem, FloatArray, MemArrayRef, MemRef,
};

struct DspPort {
    std::string name;
    DspPortType type;
    std::size_t arraySize{}; // Maximum number of samples for a Mem or FloatArray input.
};

class DspCompileError : public std::runtime_error {
public:
    DspCompileError(std::size_t line, std::size_t column, std::string_view message);

    const std::size_t line;
    const std::size_t column;
};

namespace detail {
struct DspProgramData;
}

// An immutable program, shared by all instances and voices.
class DspProgram {
public:
    [[nodiscard]] std::span<const DspPort> inputs() const noexcept;
    // Declared __const names are metadata, separate from signal connectors.
    [[nodiscard]] std::span<const std::string> enginePointers() const noexcept;
    [[nodiscard]] bool requiresEnginePointer(std::size_t binding) const noexcept;
    [[nodiscard]] std::span<const DspPort> outputs() const noexcept;
    // Whether a stage reads a declared scalar signal input.
    [[nodiscard]] bool readsSignalInput(std::size_t port) const noexcept;
    // Whether an outlet can depend on this input's current sample.
    [[nodiscard]] bool outputDependsOnInput(std::size_t output, std::size_t input) const noexcept;

private:
    explicit DspProgram(std::shared_ptr<const detail::DspProgramData> program);
    friend DspProgram compileDspCode(std::string_view source, DspLanguage language);
    friend DspProgram compileMathExpression(std::string_view source, std::span<const std::string_view> variables);
    friend class Graph;

    std::shared_ptr<const detail::DspProgramData> program_;
};

// Compiles a FlowStone-style DSP Code subset. Throws DspCompileError with
// one-based source coordinates. See the README for the accepted syntax.
[[nodiscard]] DspProgram compileDspCode(std::string_view source, DspLanguage language = DspLanguage::Code);

// Compile a single expression with immutable scalar inputs. Statements and declarations are disabled.
[[nodiscard]] DspProgram compileMathExpression(std::string_view source, std::span<const std::string_view> variables);

}
