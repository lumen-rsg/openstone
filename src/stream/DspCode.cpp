#include "stream/DspProgramData.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <compare>
#include <cctype>
#include <cstdint>
#include <limits>
#include <locale>
#include <map>
#include <numbers>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace openstone::stream {

DspCompileError::DspCompileError(std::size_t line, std::size_t column, std::string_view message)
    : std::runtime_error(std::to_string(line) + ":" + std::to_string(column) + ": " + std::string(message)),
      line(line), column(column)
{
}

DspProgram::DspProgram(std::shared_ptr<const detail::DspProgramData> program)
    : program_(std::move(program))
{
}

bool DspProgram::requiresEnginePointer(std::size_t binding) const noexcept
{
    return program_ && binding < program_->enginePointerUsed.size() && program_->enginePointerUsed[binding];
}

std::span<const std::string> DspProgram::enginePointers() const noexcept
{
    return program_ ? std::span<const std::string>{program_->enginePointers} : std::span<const std::string>{};
}

std::span<const DspPort> DspProgram::inputs() const noexcept
{
    return program_ ? std::span<const DspPort>{program_->inputs} : std::span<const DspPort>{};
}

std::span<const DspPort> DspProgram::outputs() const noexcept
{
    return program_ ? std::span<const DspPort>{program_->outputs} : std::span<const DspPort>{};
}

bool DspProgram::readsSignalInput(std::size_t port) const noexcept
{
    return program_ && std::ranges::any_of(program_->stageInputs, [port](const auto& inputs) {
        return std::ranges::binary_search(inputs, port);
    });
}

bool DspProgram::outputDependsOnInput(std::size_t output, std::size_t input) const noexcept
{
    const auto stage = program_->outputStages[output];
    if (stage == 3) return false;
    const auto& stages = program_->stageInputs;
    return std::ranges::binary_search(stages[1], input) ||
           (stage == 2 && std::ranges::binary_search(stages[2], input));
}

namespace {

using namespace detail;

enum class TokenKind { End, Identifier, Number, Hex, Symbol };

struct Token {
    TokenKind kind;
    std::string_view text;
    std::size_t line;
    std::size_t column;
};

[[noreturn]] void fail(Token token, std::string_view message)
{
    throw DspCompileError(token.line, token.column, message);
}

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr bool isLetter(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

constexpr bool isHexDigit(char c) noexcept
{
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr std::string_view compoundOperators[] = {
    "+=", "-=", "*=", "/=", "%=", "^=", "&=", "|=", "~=",
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next()
    {
        skipTrivia();
        const auto start = position_;
        Token token{TokenKind::End, source_.substr(start, 0), line_, column_};
        if (position_ == source_.size()) {
            return token;
        }
        const auto c = peek();
        if (isLetter(c)) {
            token.kind = TokenKind::Identifier;
            do {
                advance();
            } while (isLetter(peek()) || isDigit(peek()));
        } else if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            token.kind = TokenKind::Hex;
            advance();
            advance();
            if (!isHexDigit(peek())) {
                fail(token, "expected hexadecimal digits");
            }
            while (isHexDigit(peek())) {
                advance();
            }
        } else if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
            token.kind = TokenKind::Number;
            decimal();
        } else {
            token.kind = TokenKind::Symbol;
            const auto rest = source_.substr(position_);
            auto length = std::size_t{1};
            if (rest.starts_with(">>>")) {
                length = 3;
            } else if (rest.starts_with("<<") || rest.starts_with(">>") || rest.starts_with("<=")
                       || rest.starts_with(">=") || rest.starts_with("==") || rest.starts_with("!=")
                       || std::ranges::find(compoundOperators, rest.substr(0, 2)) != std::end(compoundOperators)) {
                length = 2;
            }
            for (std::size_t i = 0; i < length; ++i) {
                advance();
            }
        }
        token.text = source_.substr(start, position_ - start);
        return token;
    }

private:
    char peek(std::size_t offset = 0) const noexcept
    {
        return offset < source_.size() - position_ ? source_[position_ + offset] : '\0';
    }

    void advance() noexcept
    {
        if (source_[position_++] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
    }

    void decimal()
    {
        while (isDigit(peek())) {
            advance();
        }
        if (peek() == '.') {
            advance();
            while (isDigit(peek())) {
                advance();
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            if (!isDigit(peek())) {
                throw DspCompileError(line_, column_, "expected an exponent");
            }
            while (isDigit(peek())) {
                advance();
            }
        }
    }

    void skipTrivia()
    {
        for (;;) {
            switch (peek()) {
            case ' ': case '\t': case '\r': case '\n':
                advance();
                continue;
            case '/':
                if (peek(1) == '/') {
                    while (position_ < source_.size() && peek() != '\n') {
                        advance();
                    }
                    continue;
                }
                break;
            }
            return;
        }
    }

    std::string_view source_;
    std::size_t position_{};
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

struct PortDeclaration {
    std::string_view keyword;
    DspPortType type;
    bool input;
};

constexpr PortDeclaration ports[] = {
    {"streamin", DspPortType::Stream, true}, {"streamout", DspPortType::Stream, false},
    {"monoin", DspPortType::Mono, true}, {"monoout", DspPortType::Mono, false},
    {"polyin", DspPortType::Poly, true}, {"polyout", DspPortType::Poly, false},
    {"streamboolin", DspPortType::StreamBoolean, true}, {"streamboolout", DspPortType::StreamBoolean, false},
    {"monoboolin", DspPortType::MonoBoolean, true}, {"monoboolout", DspPortType::MonoBoolean, false},
    {"polyboolin", DspPortType::PolyBoolean, true}, {"polyboolout", DspPortType::PolyBoolean, false},
    {"streamdoublein", DspPortType::StreamDouble, true}, {"streamdoubleout", DspPortType::StreamDouble, false},
    {"monodoublein", DspPortType::MonoDouble, true}, {"monodoubleout", DspPortType::MonoDouble, false},
    {"polydoublein", DspPortType::PolyDouble, true}, {"polydoubleout", DspPortType::PolyDouble, false},
    {"polyintin", DspPortType::PolyInt, true}, {"polyintout", DspPortType::PolyInt, false},
    {"memin", DspPortType::Mem, true}, {"floatarrayin", DspPortType::FloatArray, true},
    {"memarrayrefin", DspPortType::MemArrayRef, true}, {"memrefin", DspPortType::MemRef, true},
};

bool supportsPort(DspPortType type, bool assembler)
{
    return assembler || (type != DspPortType::MemArrayRef && type != DspPortType::MemRef);
}

struct Operator {
    std::string_view spelling;
    int precedence;
    DspOp op;
};

// Masks bind more tightly than sums, as in a & (x > 0) + b & (x <= 0).
constexpr Operator operators[] = {
    {"+", 1, DspOp::Add}, {"-", 1, DspOp::Subtract},
    {"*", 2, DspOp::Multiply}, {"/", 2, DspOp::Divide}, {"%", 2, DspOp::Remainder},
    {"|", 3, DspOp::Or}, {"~", 4, DspOp::Xor}, {"&", 5, DspOp::And},
    {"<", 6, DspOp::Less}, {"<=", 6, DspOp::LessEqual}, {">", 6, DspOp::Greater},
    {">=", 6, DspOp::GreaterEqual}, {"==", 6, DspOp::Equal}, {"!=", 6, DspOp::NotEqual},
    {"<<", 7, DspOp::ShiftLeft}, {">>", 7, DspOp::ShiftRight}, {">>>", 7, DspOp::ShiftArithmetic},
    {"^", 8, DspOp::Power},
};

struct Function {
    std::string_view name;
    DspOp op;
    bool binary = false;
};

constexpr Function functions[] = {
    {"min", DspOp::Min, true}, {"max", DspOp::Max, true}, {"pow", DspOp::Power, true},
    {"sin1", DspOp::Sin1}, {"cos1", DspOp::Cos1}, {"tan1", DspOp::Tan1},
    {"log10", DspOp::Log10}, {"log2", DspOp::Log2}, {"ln", DspOp::Ln},
    {"sign", DspOp::Sign}, {"abs", DspOp::Abs}, {"exp", DspOp::Exp}, {"sqrt", DspOp::Sqrt},
    {"rndint", DspOp::RoundNearest}, {"round", DspOp::Round}, {"trunc", DspOp::Trunc},
    {"floor", DspOp::Floor}, {"ceil", DspOp::Ceil}, {"denorm", DspOp::Denorm},
};

std::optional<DspType> valueType(std::string_view name) noexcept
{
    if (name == "float") return DspType::Float;
    if (name == "double") return DspType::Double;
    if (name == "int") return DspType::Int;
    if (name == "bool") return DspType::Bool;
    return std::nullopt;
}

enum class Storage { Local, Input, Output, ArrayInput, MemArrayRef, MemRef, Constant, EnginePointer };

struct Symbol {
    Storage storage;
    DspType type;
    std::size_t slot;
    std::size_t port;
    std::size_t arraySize;
    bool operator==(const Symbol&) const = default;
};

struct AssemblyConstant { std::string_view name; DspType type; std::uint64_t bits; };
// Verified in Alpha 5483's x86/x64 constructors. Keep each binding's physical lane width:
// a Float mask is four repeated DWORDs, not a converted or zero-extended Double mask.
constexpr AssemblyConstant assemblyConstants[]{
    {"_DONE", DspType::Double, 0x3ff0000000000000ULL}, {"_DMONE", DspType::Double, 0xbff0000000000000ULL},
    {"_DABS", DspType::Double, 0x7fffffffffffffffULL}, {"_DSIGN", DspType::Double, 0x8000000000000000ULL},
    {"_PI", DspType::Float, 0x40490fdbU}, {"_E", DspType::Float, 0x402df854U},
    {"_SQRT2", DspType::Float, 0x3fb504f3U}, {"_ABS", DspType::Float, 0x7fffffffU},
    {"_SIGN", DspType::Float, 0x80000000U}, {"_DENORM", DspType::Float, 0x219392efU},
    {"_ZERO", DspType::Float, 0x00000000U}, {"_ONE", DspType::Float, 0x3f800000U},
    {"_MONE", DspType::Float, 0xbf800000U},

};

struct AssemblyFrameSymbol { std::string_view name; std::uint32_t offset; };
// These names resolve to storage at RBP + offset, not literals containing the offset.
constexpr AssemblyFrameSymbol assemblyFrameSymbols[]{
    {"_CHANNEL_GATE_OFFSET", 0x1010}, {"_CHANNEL_TRIGGER_OFFSET", 0x1020},
    {"_SAMPLE_POS_OFFSET", 0x1030}, {"_FRAME_POS_OFFSET", 0x1040},
    {"_CHANNEL_MASK_OFFSET", 0x1050}, {"_IS_POLY_PROCESS", 0x1080},
    {"_CHANNEL_CHANNEL_ID_OFFSET", 0x10c0}, {"_CHANNEL_NOTE_ID_OFFSET", 0x10e0},
};

std::uint64_t unsignedNumber(Token token)
{
    if (token.kind != TokenKind::Number && token.kind != TokenKind::Hex) {
        fail(token, "expected a nonnegative integer");
    }
    const auto hex = token.kind == TokenKind::Hex;
    const auto digits = token.text.substr(hex ? 2 : 0);
    std::uint64_t number{};
    const auto end = digits.data() + digits.size();
    const auto [last, error] = std::from_chars(digits.data(), end, number, hex ? 16 : 10);
    if (error != std::errc{} || last != end) {
        fail(token, "expected a nonnegative integer");
    }
    return number;
}

double decimalNumber(Token token)
{
    std::istringstream input{std::string(token.text)};
    input.imbue(std::locale::classic());
    double value{};
    if (token.kind != TokenKind::Number || !(input >> value) || input.peek() != std::char_traits<char>::eof()) {
        fail(token, "invalid or out-of-range decimal number");
    }
    return value;
}

enum class AssemblyForm {
    Move, Binary, Unary, AndNot, Compare, CompareAlias, Shuffle, RoundToInt, TruncToInt, ToFloat, ScalarCompare, FloatCompare, Branch,
    ScalarMove, ScalarBinary, ScalarTest, ScalarStep, Exchange, IntegerDivide, Push, Pop, Timestamp, Call, Return,
    MoveScalar, MoveDword, MoveHalf, ConvertPair, ScalarRoundToInt, PackedShift, Reciprocal,
    X87Load, X87Store, X87StoreKeep, X87One, X87Exchange, X87Unary, X87BinaryPop, X87BinaryKeep, X87TrigPair,
};

enum class AssemblyBranch { Always, Equal, NotEqual, Greater, Less, NotGreater, NotLess,
                            Above, Below, NotAbove, NotBelow, Parity, NotParity };

struct AssemblyMnemonic {
    std::string_view name;
    AssemblyForm form;
    DspOp op = DspOp::Load;
    DspType type = DspType::Float;
    unsigned predicate = 0;
};

constexpr AssemblyMnemonic mnemonics[] = {
    {"cmp", AssemblyForm::ScalarCompare},
    {"comiss", AssemblyForm::FloatCompare},
    {"mov", AssemblyForm::ScalarMove}, {"xchg", AssemblyForm::Exchange},
    {"idiv", AssemblyForm::IntegerDivide},
    {"add", AssemblyForm::ScalarBinary, DspOp::Add}, {"sub", AssemblyForm::ScalarBinary, DspOp::Subtract},
    {"and", AssemblyForm::ScalarBinary, DspOp::And}, {"or", AssemblyForm::ScalarBinary, DspOp::Or},
    {"xor", AssemblyForm::ScalarBinary, DspOp::Xor}, {"test", AssemblyForm::ScalarTest, DspOp::And},
    {"shl", AssemblyForm::ScalarBinary, DspOp::ShiftLeft}, {"shr", AssemblyForm::ScalarBinary, DspOp::ShiftRight},
    {"inc", AssemblyForm::ScalarStep, DspOp::Add},
    {"dec", AssemblyForm::ScalarStep, DspOp::Subtract},
    {"push", AssemblyForm::Push}, {"pop", AssemblyForm::Pop}, {"rdtsc", AssemblyForm::Timestamp},
    {"call", AssemblyForm::Call}, {"ret", AssemblyForm::Return},
    {"jmp", AssemblyForm::Branch, DspOp::Jump, DspType::Int, unsigned(AssemblyBranch::Always)},
    {"je", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::Equal)},
    {"jz", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::Equal)},
    {"jnz", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::NotEqual)},
    {"jne", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::NotEqual)},
    {"ja", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::Above)},
    {"jb", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::Below)},
    {"jbe", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::NotAbove)},
    {"jae", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::NotBelow)},
    {"jp", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::Parity)},
    {"jnp", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::NotParity)},
    {"jg", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::Greater)},
    {"jl", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::Less)},
    {"jng", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::NotGreater)},
    {"jnl", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::NotLess)},
    {"jge", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::NotLess)},
    {"jle", AssemblyForm::Branch, DspOp::JumpIfTrue, DspType::Int, unsigned(AssemblyBranch::NotGreater)},
    {"fld", AssemblyForm::X87Load}, {"fstp", AssemblyForm::X87Store},
    {"dfld", AssemblyForm::X87Load, DspOp::Load, DspType::Double},
    {"dfst", AssemblyForm::X87StoreKeep, DspOp::Store, DspType::Double},
    {"dfstp", AssemblyForm::X87Store, DspOp::Store, DspType::Double},
    {"fild", AssemblyForm::X87Load, DspOp::Load, DspType::Int},
    {"fst", AssemblyForm::X87StoreKeep},
    {"fist", AssemblyForm::X87StoreKeep, DspOp::RoundNearest, DspType::Int},
    {"fistp", AssemblyForm::X87Store, DspOp::RoundNearest, DspType::Int},
    {"fld1", AssemblyForm::X87One}, {"fxch", AssemblyForm::X87Exchange},
    {"frndint", AssemblyForm::X87Unary, DspOp::RoundNearest},
    {"f2xm1", AssemblyForm::X87Unary, DspOp::Exp2MinusOne},
    {"fsin", AssemblyForm::X87Unary, DspOp::SinRadians},
    {"fcos", AssemblyForm::X87Unary, DspOp::CosRadians},
    {"fsincos", AssemblyForm::X87TrigPair, DspOp::SinRadians},
    {"fptan", AssemblyForm::X87TrigPair, DspOp::TanRadians},
    {"fprem", AssemblyForm::X87BinaryKeep, DspOp::Remainder},
    {"fadd", AssemblyForm::X87BinaryPop, DspOp::Add},
    {"fsub", AssemblyForm::X87BinaryPop, DspOp::Subtract},
    {"fmul", AssemblyForm::X87BinaryPop, DspOp::Multiply},
    {"fdivp", AssemblyForm::X87BinaryPop, DspOp::Divide},
    {"dfdivp", AssemblyForm::X87BinaryPop, DspOp::Divide},
    {"fyl2x", AssemblyForm::X87BinaryPop, DspOp::Log2},
    {"fscale", AssemblyForm::X87BinaryKeep, DspOp::ScalePowerOfTwo},
    {"movaps", AssemblyForm::Move}, {"movups", AssemblyForm::Move},
    {"movapd", AssemblyForm::Move, DspOp::Load, DspType::Double},
    {"movupd", AssemblyForm::Move, DspOp::Load, DspType::Double},
    {"movdqa", AssemblyForm::Move}, {"movdqu", AssemblyForm::Move},
    {"movss", AssemblyForm::MoveScalar}, {"movd", AssemblyForm::MoveDword},
    {"movhlps", AssemblyForm::MoveHalf}, {"movlhps", AssemblyForm::MoveHalf, DspOp::ShufflePs, DspType::Float, 1},
    {"addps", AssemblyForm::Binary, DspOp::SseAdd},
    {"subps", AssemblyForm::Binary, DspOp::SseSubtract},
    {"mulps", AssemblyForm::Binary, DspOp::SseMultiply},
    {"addpd", AssemblyForm::Binary, DspOp::SseAdd, DspType::Double},
    {"subpd", AssemblyForm::Binary, DspOp::SseSubtract, DspType::Double},
    {"mulpd", AssemblyForm::Binary, DspOp::SseMultiply, DspType::Double},
    {"divpd", AssemblyForm::Binary, DspOp::SseDivide, DspType::Double},
    {"minpd", AssemblyForm::Binary, DspOp::SseMin, DspType::Double},
    {"maxpd", AssemblyForm::Binary, DspOp::SseMax, DspType::Double},
    {"sqrtpd", AssemblyForm::Unary, DspOp::SseSqrt, DspType::Double},
    {"divps", AssemblyForm::Binary, DspOp::SseDivide},
    {"minps", AssemblyForm::Binary, DspOp::SseMin},
    {"maxps", AssemblyForm::Binary, DspOp::SseMax},
    {"sqrtps", AssemblyForm::Unary, DspOp::SseSqrt},
    {"andps", AssemblyForm::Binary, DspOp::And},
    {"orps", AssemblyForm::Binary, DspOp::Or},
    {"xorps", AssemblyForm::Binary, DspOp::Xor},
    {"andnps", AssemblyForm::AndNot},
    {"andpd", AssemblyForm::Binary, DspOp::And, DspType::Double},
    {"andnpd", AssemblyForm::AndNot, DspOp::And, DspType::Double},
    {"orpd", AssemblyForm::Binary, DspOp::Or, DspType::Double},
    {"xorpd", AssemblyForm::Binary, DspOp::Xor, DspType::Double},
    {"pand", AssemblyForm::Binary, DspOp::And},
    {"por", AssemblyForm::Binary, DspOp::Or},
    {"pxor", AssemblyForm::Binary, DspOp::Xor},
    {"pandn", AssemblyForm::AndNot},
    {"paddd", AssemblyForm::Binary, DspOp::Add, DspType::Int},
    {"psubd", AssemblyForm::Binary, DspOp::Subtract, DspType::Int},
    {"pcmpeqd", AssemblyForm::Binary, DspOp::Equal, DspType::Int},
    {"pcmpgtd", AssemblyForm::Binary, DspOp::Greater, DspType::Int},
    {"pmuludq", AssemblyForm::Binary, DspOp::MultiplyUnsignedDoublewords, DspType::Int},
    {"pslld", AssemblyForm::PackedShift, DspOp::ShiftLeft, DspType::Int},
    {"psrld", AssemblyForm::PackedShift, DspOp::ShiftRight, DspType::Int},
    {"rcpps", AssemblyForm::Reciprocal},
    {"cmpps", AssemblyForm::Compare},
    {"cmpeqps", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Float, 0},
    {"cmpltps", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Float, 1},
    {"cmpleps", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Float, 2},
    {"cmpunordps", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Float, 3},
    {"cmpneqps", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Float, 4},
    {"cmpnltps", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Float, 5},
    {"cmpnleps", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Float, 6},
    // FlowStone's greater-than aliases use NLE (unordered is true), not ordered GT.
    {"cmpgtps", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Float, 6},
    {"cmpordps", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Float, 7},
    {"cmppd", AssemblyForm::Compare, DspOp::SseCompare, DspType::Double},
    {"cmpeqpd", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Double, 0},
    {"cmpltpd", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Double, 1},
    {"cmplepd", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Double, 2},
    {"cmpunordpd", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Double, 3},
    {"cmpneqpd", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Double, 4},
    {"cmpnltpd", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Double, 5},
    {"cmpnlepd", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Double, 6},
    {"cmpgtpd", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Double, 6},
    {"cmpordpd", AssemblyForm::CompareAlias, DspOp::SseCompare, DspType::Double, 7},
    {"shufps", AssemblyForm::Shuffle},
    {"shufpd", AssemblyForm::Shuffle, DspOp::ShufflePd, DspType::Double},
    {"cvtps2dq", AssemblyForm::RoundToInt},
    {"cvtss2si", AssemblyForm::ScalarRoundToInt},
    {"cvttps2dq", AssemblyForm::TruncToInt},
    {"cvtpd2dq", AssemblyForm::RoundToInt, DspOp::Convert, DspType::Double},
    {"cvttpd2dq", AssemblyForm::TruncToInt, DspOp::Convert, DspType::Double},
    {"cvtdq2ps", AssemblyForm::ToFloat, DspOp::Convert, DspType::Int},
    {"cvtdq2pd", AssemblyForm::ConvertPair, DspOp::Convert, DspType::Int},
    {"cvtps2pd", AssemblyForm::ConvertPair, DspOp::Convert, DspType::Float},
    {"cvtpd2ps", AssemblyForm::ConvertPair, DspOp::Convert, DspType::Double},
};

constexpr std::string_view xmmNames[] = {
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
};

constexpr std::string_view gprNames[] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"};
constexpr std::string_view byteRegisterNames[] = {"al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"};
// Only the first four legacy GPRs have the existing low/high byte aliases.
constexpr auto byteRegisterParentCount = std::size(byteRegisterNames) / 2;
constexpr std::string_view addressRegisterNames[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp"};
static_assert(std::size(gprNames) == std::size(addressRegisterNames));
using GprAssignments = std::array<bool, std::size(gprNames)>;

struct AssemblyTarget {
    std::string_view label;
    std::uint32_t id;

    auto operator<=>(const AssemblyTarget&) const = default;
};

struct AssemblyTargets {
    std::vector<AssemblyTarget> labels;

    bool operator==(const AssemblyTargets&) const = default;
};

enum class AssemblyPointerKind { Mem, Channels, Addresses, Sizes, Limits, Samples, Frame, Engine };

struct AssemblyPointer {
    Symbol symbol;
    std::optional<std::int32_t> offset = 0;
    AssemblyPointerKind kind = AssemblyPointerKind::Mem;

    bool operator==(const AssemblyPointer&) const = default;
};

struct AssemblyMemoryCell {
    Storage storage;
    std::size_t slot;
    std::size_t offset;

    auto operator<=>(const AssemblyMemoryCell&) const = default;
};

using AssemblyReference = std::variant<std::monostate, AssemblyPointer, AssemblyTargets>;
using GprReferences = std::array<AssemblyReference, std::size(gprNames)>;

bool isReference(const AssemblyReference& value) noexcept
{
    return !std::holds_alternative<std::monostate>(value);
}

AssemblyTargets mergeAssemblyTargets(const AssemblyTargets& lhs, const AssemblyTargets& rhs)
{
    if (lhs == rhs) return lhs;
    auto labels = lhs.labels;
    labels.insert(labels.end(), rhs.labels.begin(), rhs.labels.end());
    std::ranges::sort(labels);
    const auto duplicates = std::ranges::unique(labels);
    labels.erase(duplicates.begin(), duplicates.end());
    return {std::move(labels)};
}

std::optional<AssemblyReference> mergeAssemblyReferences(const AssemblyReference& lhs, const AssemblyReference& rhs)
{
    if (lhs == rhs) return lhs;
    const auto* leftPointer = std::get_if<AssemblyPointer>(&lhs);
    const auto* rightPointer = std::get_if<AssemblyPointer>(&rhs);
    if (leftPointer && rightPointer && leftPointer->symbol == rightPointer->symbol && leftPointer->kind == rightPointer->kind) {
        return AssemblyPointer{leftPointer->symbol, std::nullopt, leftPointer->kind};
    }
    const auto* left = std::get_if<AssemblyTargets>(&lhs);
    const auto* right = std::get_if<AssemblyTargets>(&rhs);
    if (!left || !right) return std::nullopt;
    return mergeAssemblyTargets(*left, *right);
}

bool assemblyReferenceIncludes(const AssemblyReference& expected, const AssemblyReference& actual)
{
    const auto* expectedPointer = std::get_if<AssemblyPointer>(&expected);
    const auto* actualPointer = std::get_if<AssemblyPointer>(&actual);
    if (expectedPointer && actualPointer) return expectedPointer->symbol == actualPointer->symbol && expectedPointer->kind == actualPointer->kind;
    const auto merged = mergeAssemblyReferences(expected, actual);
    return merged && *merged == expected;
}

std::string lowercase(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::optional<std::size_t> assemblyGpr(std::string_view text)
{
    const auto name = lowercase(text);
    const auto reg = std::ranges::find(gprNames, name);
    if (reg != std::end(gprNames)) return static_cast<std::size_t>(reg - std::begin(gprNames));
    const auto addressReg = std::ranges::find(addressRegisterNames, name);
    if (addressReg != std::end(addressRegisterNames)) {
        return static_cast<std::size_t>(addressReg - std::begin(addressRegisterNames));
    }
    return std::nullopt;
}

enum class XmmWidth { Bits32, Bits64 };
using XmmAssignments = std::array<bool, std::size(xmmNames)>;

constexpr XmmWidth xmmWidth(DspType type) noexcept
{
    return type == DspType::Double ? XmmWidth::Bits64 : XmmWidth::Bits32;
}

struct AssemblyPosition {
    Lexer lexer;
    Token token;
};

struct AssemblyLabelSource {
    AssemblyPosition entry;
    std::uint32_t id;
    bool possibleSubroutine = false;
    bool jumpTarget = false;
    bool reachedWithoutCall = false;
    bool reachedInStage = false;
    bool ambiguous = false;
};

struct AssemblyState {
    XmmAssignments xmmWritten{};
    GprAssignments written{};
    bool flagsAvailable = false;
    bool carryAvailable = false;
    bool parityAvailable = false;
    std::size_t x87Depth = 0;
    // nullopt preserves an opaque incoming value without making its bits observable.
    std::vector<std::optional<AssemblyReference>> stackReferences;
    GprReferences references{};
    std::map<AssemblyMemoryCell, AssemblyTargets> memoryTargets;
    std::map<std::size_t, AssemblyTargets> indexedMemoryTargets;

    bool operator==(const AssemblyState&) const = default;
};

struct AssemblyJump {
    Token target;
    std::size_t instruction;
    AssemblyState state;
};

struct AssemblyInvocation {
    std::string_view label;
    std::size_t depth;
    AssemblyJump entry;
};

struct AssemblySubroutine {
    std::size_t stackDepth;
    std::vector<AssemblyJump> earlyReturns;
    bool endedWithTailJump = false;
};

struct AssemblyCallBody {
    const char* source;
    std::size_t entry;
    AssemblyState input;
    std::optional<AssemblyState> output;
    std::unordered_map<std::string_view, AssemblyPosition> subroutineEnds;
};

struct AssemblyBoundary {
    AssemblyPosition source;
    std::optional<AssemblyJump> entry;
};

// Inlining a call chain can expand a short source exponentially.
constexpr std::size_t assemblyNestedCallLimit = 4096;

struct AssemblyBranches {
    bool reachable = true;
    bool skippingSubroutine = false;
    bool stoppedByCall = false;
    std::unordered_map<std::string_view, AssemblyPosition> subroutineEnds;
    std::unordered_map<std::string_view, std::vector<AssemblyJump>> jumps;
    std::unordered_map<std::string_view, std::optional<AssemblyJump>> labels;
    std::vector<std::string_view> pendingLabels;
    std::unordered_map<const char*, std::vector<AssemblyJump>> displacementJumps;
    std::vector<AssemblyBoundary> boundaries;
    const char* skipUntil{};
};

struct AssemblyStage : AssemblyState {
    std::array<std::optional<std::size_t>, std::size(xmmNames)> registers{};
    std::array<std::optional<std::size_t>, std::size(gprNames)> gprs{};
    std::optional<std::size_t> zeroFlag;
    std::optional<std::size_t> lessFlag;
    std::optional<std::size_t> carryFlag;
    std::optional<std::size_t> parityFlag;
    std::vector<std::size_t> integerStack;
    std::array<std::vector<std::size_t>, dspNestingLimit> recursiveIntegerStacks;
    std::vector<std::size_t> returns;
    std::array<std::optional<std::size_t>, 8> x87Slots{};
    AssemblyBranches branches;
};

class Compiler {
public:
    explicit Compiler(std::string_view source, DspLanguage language = DspLanguage::Code)
        : assembler_(language == DspLanguage::Assembler), lexer_(source), token_(lexer_.next())
    {
        if (assembler_ && source.contains('-')) {
            Lexer scan(source);
            for (auto word = scan.next(); word.kind != TokenKind::End;) {
                const auto next = scan.next();
                if (next.text == "-" && word.text.size() == 3) {
                    const auto name = lowercase(word.text);
                    if (name == "jmp" || name == "jnz") {
                        trackAssemblyBoundaries_ = true;
                        break;
                    }
                }
                word = next;
            }
        }
    }

    std::shared_ptr<const DspProgramData> compileExpression(std::span<const std::string_view> variables)
    {
        expressionOnly_ = true;
        const auto location = token_;
        for (const auto name : variables) {
            const auto input = declare({TokenKind::Identifier, name, location.line, location.column}, Storage::Input,
                                       DspType::Double, 0, data_->inputs.size());
            data_->inputs.push_back({std::string(name), DspPortType::MonoDouble});
            data_->inputSlots.push_back(input.slot);
        }
        const auto output = declare({TokenKind::Identifier, "__result", location.line, location.column},
                                    Storage::Output, DspType::Double, 0);
        data_->outputs.push_back({"__result", DspPortType::MonoDouble});
        data_->outputSlots.push_back(output.slot);
        data_->outputStages.push_back(2);
        expression(DspType::Double);
        if (token_.kind != TokenKind::End) fail(token_, "expected the end of the expression");
        emit({DspOp::Store, DspType::Double, output.slot}, -1);
        auto& inputs = data_->stageInputs[2];
        std::ranges::sort(inputs);
        const auto duplicates = std::ranges::unique(inputs);
        inputs.erase(duplicates.begin(), duplicates.end());
        return data_;
    }

    std::shared_ptr<const DspProgramData> compile()
    {
        if (token_.text == "#") {
            const auto header = take();
            if (token_.line != header.line || !accept("COMPATIBILITY_3_0_8")) {
                fail(header, "expected # COMPATIBILITY_3_0_8");
            }
            if (token_.kind != TokenKind::End && token_.line == header.line) {
                fail(token_, "expected the end of the compatibility directive");
            }
        }
        if (assembler_) {
            for (std::size_t i = 0; i < assemblyStages_.size(); ++i) {
                auto& stage = assemblyStages_[i];
                stage.xmmWritten.fill(true); // An explicit portable entry bank supplies all XMM registers.
                stage.gprs[2] = data_->slotCount++;
                stage.written[2] = true;
                data_->stages[i].push_back({DspOp::ReadSamplePosition, DspType::Int, *stage.gprs[2]});
                for (const auto reg : {3U, 6U}) {
                    stage.gprs[reg] = data_->slotCount++;
                    stage.written[reg] = true;
                    stage.references[reg] = AssemblyPointer{{Storage::MemRef, DspType::Float, 0, nativeFramePort, 0}, 0, AssemblyPointerKind::Frame};
                }
                data_->stages[i].push_back({DspOp::BindNativeFrame, DspType::Int, *stage.gprs[3], *stage.gprs[6]});
            }
            indexAssemblySection();
        }
        while (token_.kind != TokenKind::End) {
            if (assembler_) resolveDisplacementJumps();
            if (startsStageHeader(token_, lexer_) &&
                (assembler_ || !symbols_.contains("stage") || stageBlockAhead(lexer_))) {
                const bool compact = compactStageHeader(token_, lexer_);
                const auto header = take();
                // The engine's Assembler kernels also use persistent `stage N;`
                // and `stageN;` directives. Parenthesized blocks keep their existing scope.
                const bool directive = compact || (assembler_ && token_.text != "(");
                const auto [location, number] = compact
                    ? std::pair{header, static_cast<std::size_t>(header.text.back() - '0')}
                    : directive ? stageDirectiveNumber() : sectionNumber();
                if (number >= data_->stages.size()) {
                    fail(location, "expected stage number 0, 1, 2, or 3");
                }
                if (compact) expect(";");
                activeStage_ = static_cast<unsigned>(number);
                assemblyStages_[activeStage_].branches.boundaries.clear();
                if (!directive) {
                    sectionBody(1);
                    activeStage_ = 2;
                    assemblyStages_[activeStage_].branches.boundaries.clear();
                }
                if (assembler_) indexAssemblySection();
            } else if (assembler_ && accept("__const")) {
                do {
                    const auto name = identifier();
                    if (data_->enginePointers.size() == 65536) fail(name, "too many engine pointer declarations");
                    declare(name, Storage::EnginePointer, DspType::Int, 0, enginePointerPortBase + data_->enginePointers.size());
                    data_->enginePointers.emplace_back(name.text);
                    data_->enginePointerUsed.push_back(false);
                } while (accept(","));
                expect(";");
            } else if (const auto type = valueType(token_.text)) {
                localDeclaration(*type);
            } else if (const auto port = std::ranges::find(ports, token_.text, &PortDeclaration::keyword);
                       port != std::end(ports)) {
                if (!supportsPort(port->type, assembler_)) {
                    fail(token_, assembler_ ? "this connector type is not supported in Assembler"
                                            : "this connector type requires Assembler");
                }
                portDeclaration(*port);
            } else {
                statement(0);
            }
        }
        if (assembler_) resolveDisplacementJumps();
        const auto savedStage = activeStage_;
        for (std::size_t i = 0; i < assemblyStages_.size(); ++i) {
            activeStage_ = static_cast<unsigned>(i);
            const auto& stage = assemblyStages_[i];
            for (const auto& [label, jumps] : stage.branches.jumps) {
                fail(jumps.front().target, "unknown Assembler label '" + std::string(label) + "' in this stage");
            }
            if (!stage.branches.displacementJumps.empty()) {
                fail(stage.branches.displacementJumps.begin()->second.front().target,
                     "numeric branch target must be executable in the same stage");
            }
            if (stage.branches.reachable) checkAssemblyStacks(token_, stage);
            auto& code = data_->stages[i];
            for (auto pc : stage.returns) code[pc].slot = code.size();
            for (const auto& check : assemblyDeferredLiveness_[i])
                if (!assemblyRegisterDeadAt(check.slot, check.start)) fail(check.target, check.flags
                    ? "flags are unavailable at backward branch target" : "integer register state differs at backward branch target");
            // An entry binding alone must not activate an otherwise empty stage.
            if (code.size() == 2 && code.front().op == DspOp::ReadSamplePosition &&
                code.back().op == DspOp::BindNativeFrame) code.clear();
        }
        activeStage_ = savedStage;
        if (data_->outputs.empty()) {
            fail(token_, assembler_ ? "declare at least one stream output" : "declare at least one DSP output");
        }
        for (auto& inputs : data_->stageInputs) {
            std::ranges::sort(inputs);
            const auto duplicates = std::ranges::unique(inputs);
            inputs.erase(duplicates.begin(), duplicates.end());
        }
        for (auto& outputStage : data_->outputStages) {
            if (outputStage == 0) {
                outputStage = 2;
            }
        }
        return data_;
    }

private:
    bool compactStageHeader(Token token, Lexer scan) const
    {
        return assembler_ && token.text.size() == 6 && token.text.starts_with("stage") &&
            isDigit(token.text.back()) && !symbols_.contains(token.text) && scan.next().text != ":";
    }

    bool startsStageHeader(Token token, Lexer scan) const
    {
        if (compactStageHeader(token, scan)) return true;
        if (token.text != "stage") return false;
        const auto next = scan.next();
        return next.text == "(" || (assembler_ &&
            (next.kind == TokenKind::Number || next.kind == TokenKind::Hex || next.text == "-" || next.text == "+"));
    }

    static bool stageBlockAhead(Lexer scan)
    {
        // A declared variable can also use a parenthesized lane selector.
        // The following opening brace distinguishes a real stage block.
        if (scan.next().text != "(") return false;
        scan.next();
        return scan.next().text == ")" && scan.next().text == "{";
    }

    Token take() { return std::exchange(token_, lexer_.next()); }

    bool accept(std::string_view spelling)
    {
        if (token_.text == spelling && token_.kind != TokenKind::End) {
            take();
            return true;
        }
        return false;
    }

    void expect(std::string_view spelling)
    {
        if (!accept(spelling)) {
            fail(token_, "expected '" + std::string(spelling) + "'");
        }
    }

    Token identifier()
    {
        if (token_.kind != TokenKind::Identifier) {
            fail(token_, "expected a name");
        }
        return take();
    }

    void emit(DspInstruction instruction, int change = 0)
    {
        data_->stages[activeStage_].push_back(instruction);
        if (change > 0) {
            data_->stackSize = std::max(data_->stackSize, ++stackDepth_);
        } else if (change < 0) {
            stackDepth_ -= static_cast<std::size_t>(-change);
        }
    }

    static void checkNesting(Token token, std::size_t depth)
    {
        if (depth >= dspNestingLimit) {
            fail(token, "DSP code is nested too deeply");
        }
    }

    std::size_t arraySize()
    {
        const auto size = take();
        const auto count = unsignedNumber(size);
        if (count == 0 || count > std::vector<DspLanes>{}.max_size()) {
            fail(size, "array size must be positive and fit in memory");
        }
        expect("]");
        return static_cast<std::size_t>(count);
    }

    Symbol declare(Token name, Storage storage, DspType type, std::size_t count, std::size_t port = 0)
    {
        if (assembler_ && (std::ranges::contains(xmmNames, lowercase(name.text)) ||
                           assemblyGpr(name.text))) {
            fail(name, "an Assembler variable cannot have a register name");
        }
        if ((assembler_ && name.text == "__const") || name.text == "loop" || name.text == "hop" || name.text == "rand"
            || name.text == "true" || name.text == "false" || valueType(name.text)
            || std::ranges::find(ports, name.text, &PortDeclaration::keyword) != std::end(ports)) {
            fail(name, "reserved DSP name");
        }
        const bool arrayStorage = storage == Storage::ArrayInput || storage == Storage::MemArrayRef || storage == Storage::MemRef;
        auto& slots = arrayStorage ? data_->arraySlotCount : data_->slotCount;
        const auto needed = std::max<std::size_t>(count, 1);
        if (needed > std::vector<DspLanes>{}.max_size() - slots) {
            fail(name, "total DSP storage is too large");
        }
        const Symbol symbol{storage, type, slots, port, count};
        if (!symbols_.try_emplace(name.text, symbol).second) {
            fail(name, "duplicate DSP name");
        }
        slots += needed;
        return symbol;
    }

    Symbol lookup(Token name) const
    {
        const auto found = symbols_.find(name.text);
        if (found == symbols_.end()) {
            fail(name, "unknown DSP name '" + std::string(name.text) + "'");
        }
        return found->second;
    }

    Symbol assemblySymbol(Token name)
    {
        // Existing explicit declarations retain their meaning; engine constants are fallback bindings.
        if (symbols_.contains(name.text)) return lookup(name);
        const auto constant = std::ranges::find(assemblyConstants, name.text, &AssemblyConstant::name);
        if (constant == std::end(assemblyConstants)) return lookup(name);
        auto& cached = assemblyConstantSymbols_[static_cast<std::size_t>(constant - std::begin(assemblyConstants))];
        if (!cached) {
            if (data_->slotCount == std::vector<DspLanes>{}.max_size()) fail(name, "total DSP storage is too large");
            cached = Symbol{Storage::Constant, constant->type, data_->slotCount++, 0, 0};
            DspLanes lanes{};
            lanes.fill(constant->bits);
            data_->initializers.push_back({cached->slot, 1, constant->type, {lanes}});
        }
        return *cached;
    }

    void portDeclaration(PortDeclaration port)
    {
        take();
        do {
            portDeclarator(port);
        } while (accept(","));
        expect(";");
    }

    void portDeclarator(PortDeclaration port)
    {
        const auto name = identifier();
        const bool external = port.type == DspPortType::Mem || port.type == DspPortType::FloatArray;
        std::size_t count = 0;
        if (external) {
            expect("[");
            count = arraySize();
        }
        auto& list = port.input ? data_->inputs : data_->outputs;
        auto& slots = port.input ? data_->inputSlots : data_->outputSlots;
        const auto storage = port.type == DspPortType::MemRef ? Storage::MemRef
            : port.type == DspPortType::MemArrayRef ? Storage::MemArrayRef
            : external ? Storage::ArrayInput : port.input ? Storage::Input : Storage::Output;
        const auto storageCount = storage == Storage::MemArrayRef ? static_cast<std::size_t>(MemArrayField::Size)
            : storage == Storage::MemRef ? static_cast<std::size_t>(MemField::Size) : count;
        if (storage == Storage::MemRef || storage == Storage::MemArrayRef) data_->hasMemRef = true;
        slots.push_back(declare(name, storage, dspPortType(port.type), storageCount, list.size()).slot);
        list.push_back({std::string(name.text), port.type, count});
        if (!port.input) {
            data_->outputStages.push_back(0);
        }
    }

    double signedDecimal()
    {
        const auto neg = accept("-");
        if (!neg) {
            accept("+");
        }
        const auto value = decimalNumber(take());
        return neg ? -value : value;
    }

    std::uint64_t initializerValue(DspType type)
    {
        const auto location = token_;
        if (accept("true") || accept("false")) {
            return dspEncode(location.text == "true", type);
        }
        if (token_.kind == TokenKind::Hex) {
            const auto bits = unsignedNumber(take());
            if (type != DspType::Double && bits > std::numeric_limits<std::uint32_t>::max()) {
                fail(location, "hexadecimal initializer exceeds the variable's width");
            }
            return dspConvert(bits, DspType::Hex, type);
        }
        const auto number = signedDecimal();
        if (type == DspType::Float && std::abs(number) > std::numeric_limits<float>::max()) {
            fail(location, "initializer exceeds the float range");
        }
        if (type == DspType::Int && (std::trunc(number) < -2147483648.0 || std::trunc(number) >= 2147483648.0)) {
            fail(location, "initializer exceeds the int range");
        }
        return dspEncode(number, type);
    }

    void randomInitializer(DspInitializer& init, Token location)
    {
        if (init.type == DspType::Bool) {
            fail(location, "rand requires a numeric array");
        }
        init.random = true;
        init.high = init.type == DspType::Int ? 2147483648.0 : 1.0;
        expect("(");
        if (!accept(")")) {
            init.high = signedDecimal();
            if (accept(",")) {
                init.low = init.high;
                init.high = signedDecimal();
            } else if (init.high < 0) {
                std::swap(init.low, init.high);
            }
            expect(")");
        }
        if (init.low > init.high
            || (init.type == DspType::Int && (init.low < -2147483648.0 || init.high > 2147483648.0))
            || (init.type == DspType::Float && std::max(std::abs(init.low), std::abs(init.high)) > std::numeric_limits<float>::max())) {
            fail(location, "invalid rand range");
        }
    }

    void localDeclaration(DspType type)
    {
        take();
        do {
            const auto name = identifier();
            const auto count = accept("[") ? arraySize() : 0;
            const auto slot = declare(name, Storage::Local, type, count).slot;
            if (!accept("=")) {
                continue;
            }
            DspInitializer init{slot, std::max<std::size_t>(count, 1), type, {}};
            if (count != 0 && accept("rand")) {
                randomInitializer(init, name);
            } else if (accept("{")) {
                init.fill = false;
                DspLanes lanes{};
                std::size_t index = 0;
                do {
                    if (index == (count == 0 ? lanes.size() : count)) {
                        fail(token_, "too many initializer values");
                    }
                    const auto bits = initializerValue(type);
                    if (count == 0) {
                        lanes[index] = bits;
                    } else {
                        lanes.fill(bits);
                        init.values.push_back(lanes);
                    }
                    ++index;
                } while (accept(","));
                expect("}");
                if (count == 0) {
                    init.values.push_back(lanes);
                }
            } else {
                DspLanes lanes;
                lanes.fill(initializerValue(type));
                init.values.push_back(lanes);
            }
            data_->initializers.push_back(std::move(init));
        } while (accept(","));
        expect(";");
    }

    std::pair<Token, std::size_t> sectionNumber()
    {
        expect("(");
        const auto location = take();
        const auto number = unsignedNumber(location);
        if (number > std::numeric_limits<std::size_t>::max()) {
            fail(location, "section count is too large");
        }
        expect(")");
        return {location, static_cast<std::size_t>(number)};
    }

    std::pair<Token, std::size_t> stageDirectiveNumber()
    {
        const auto location = take();
        const auto number = unsignedNumber(location);
        if (number >= data_->stages.size()) fail(location, "expected stage number 0, 1, 2, or 3");
        expect(";");
        return {location, static_cast<std::size_t>(number)};
    }

    void sectionBody(std::size_t depth)
    {
        expect("{");
        if (assembler_) indexAssemblySection();
        for (;;) {
            if (assembler_) resolveDisplacementJumps();
            if (accept("}")) break;
            statement(depth);
        }
    }

    void statement(std::size_t depth)
    {
        checkNesting(token_, depth);
        const auto section = token_.text;
        if (assembler_) {
            assemblyStatement();
        } else if (accept("loop") || accept("hop")) {
            const auto [location, count] = sectionNumber();
            const bool loop = section == "loop";
            if (!loop && (count < 2 || count > 4096 || !std::has_single_bit(count))) {
                fail(location, "hop requires a power of two from 2 to 4096");
            }
            auto& code = data_->stages[activeStage_];
            const auto start = code.size();
            emit({.op = loop ? DspOp::Loop : DspOp::Hop, .count = count});
            sectionBody(depth + 1);
            if (loop) {
                emit({.op = DspOp::Repeat, .slot = start + 1});
            }
            code[start].slot = code.size();
        } else {
            assignment();
        }
    }

    int laneNumber()
    {
        const auto location = take();
        const auto lane = unsignedNumber(location);
        if (lane >= DspLanes{}.size()) {
            fail(location, "lane must be in the range 0 to 3");
        }
        return static_cast<int>(lane);
    }

    DspSelection laneSelection()
    {
        DspSelection selection;
        if (accept("{")) {
            selection.mask = 0;
            do {
                selection.mask |= 1U << laneNumber();
            } while (accept(","));
            expect("}");
        }
        if (accept("(")) {
            selection.broadcast = laneNumber();
            expect(")");
        }
        return selection;
    }

    void address(Symbol source, std::size_t depth)
    {
        if (source.arraySize != 0) {
            expect("[");
            expression(DspType::Int, 1, depth + 1);
            expect("]");
        }
    }

    void load(Symbol source, DspSelection selection = {})
    {
        const auto op = source.storage == Storage::ArrayInput ? DspOp::LoadArrayInput
            : source.arraySize == 0 ? DspOp::Load : DspOp::LoadIndexed;
        emit({op, source.type, source.slot, source.arraySize, 0, selection}, source.arraySize == 0 ? 1 : 0);
    }

    struct AssemblyIndex {
        Token token;
        std::size_t reg{};
        unsigned shift{};
    };

    struct AssemblyAddress {
        std::array<AssemblyIndex, 2> indices{};
        std::size_t indexCount{};
        std::int64_t displacement{};
        std::optional<Symbol> engineBase;
    };

    struct AssemblyOperand {
        Token token;
        std::optional<std::size_t> reg;
        Symbol symbol{};
        std::optional<std::size_t> gpr;
        std::optional<std::uint32_t> immediate;
        std::optional<AssemblyAddress> address;
        std::optional<unsigned> memRefField;
        std::optional<unsigned> memArrayField;
        AssemblyPointerKind pointerKind = AssemblyPointerKind::Mem;
        bool byte = false;
        unsigned byteShift = 0;
    };

    static unsigned assemblyAddressShift(Token token)
    {
        const auto scale = unsignedNumber(token);
        if (scale > 8 || !std::has_single_bit(scale)) fail(token, "address scale must be 1, 2, 4, or 8");
        return std::countr_zero(scale);
    }

    AssemblyAddress assemblyAddress(std::optional<Symbol> symbol)
    {
        AssemblyAddress address;
        bool hasDisplacement = false;
        bool negative = accept("-");
        if (!negative) accept("+");
        do {
            const auto term = take();
            const bool leadingScale = term.kind != TokenKind::Identifier && accept("*");
            if (term.kind == TokenKind::Identifier || leadingScale) {
                const auto name = leadingScale ? identifier() : term;
                const auto reg = assemblyGpr(name.text);
                const auto external = symbols_.find(name.text);
                if (!leadingScale && external != symbols_.end() && external->second.storage == Storage::EnginePointer) {
                    if (symbol || negative || address.engineBase || token_.text == "*") fail(name, "an address accepts one unscaled engine pointer base");
                    address.engineBase = external->second;
                    data_->enginePointerUsed[external->second.port - enginePointerPortBase] = true;
                    if (accept("]")) return address;
                    negative = accept("-");
                    if (!negative) expect("+");
                    continue;
                }
                if (negative || !reg || address.indexCount == address.indices.size()) {
                    fail(name, "an Assembler address accepts up to two added integer registers");
                }
                const auto shift = leadingScale ? assemblyAddressShift(term) : accept("*") ? assemblyAddressShift(take()) : 0U;
                if (symbol && isReference(assemblyStages_[activeStage_].references[*reg])) {
                    fail(name, "use a numeric offset with named storage");
                }
                address.indices[address.indexCount++] = {name, *reg, shift};
            } else {
                const auto magnitude = unsignedNumber(term);
                if (hasDisplacement || magnitude > (negative ? 0x80000000ULL : 0x7fffffffULL)) {
                    fail(term, "an Assembler address accepts one signed 32-bit displacement");
                }
                address.displacement = static_cast<std::int64_t>(magnitude) * (negative ? -1 : 1);
                hasDisplacement = true;
                if (symbol && token_.text == "]" && address.indexCount == 0) {
                    if (symbol->arraySize == 0 && (address.displacement < 0 || address.displacement >= 4)) {
                        fail(term, "lane must be in the range 0 to 3");
                    }
                    address.displacement *= assemblyElementBytes(symbol->type);
                    if (address.displacement < std::numeric_limits<std::int32_t>::min() ||
                        address.displacement > std::numeric_limits<std::int32_t>::max()) {
                        fail(term, "an Assembler address must fit in a signed 32-bit byte offset");
                    }
                }
            }
            if (accept("]")) return address;
            negative = accept("-");
            if (!negative) expect("+");
        } while (true);
    }

    AssemblyOperand assemblyOperand(bool allowByte = false)
    {
        const auto at = token_;
        if (!accept("[")) return assemblyNamedOperand(identifier(), allowByte);
        const auto address = assemblyAddress(std::nullopt);
        std::optional<Symbol> reference = address.engineBase;
        auto kind = reference ? AssemblyPointerKind::Engine : AssemblyPointerKind::Mem;
        const auto& stage = assemblyStages_[activeStage_];
        for (std::size_t i = 0; i < address.indexCount; ++i) {
            const auto [token, reg, shift] = address.indices[i];
            if (!stage.written[reg]) fail(token, "write the integer register before reading it on every path");
            if (std::holds_alternative<AssemblyTargets>(stage.references[reg])) {
                fail(token, "a subroutine label cannot be used as a memory address");
            }
            const auto* memory = std::get_if<AssemblyPointer>(&stage.references[reg]);
            if (!memory) continue;
            if (reference || shift != 0) fail(token, "an indirect operand requires one unscaled Mem reference register");
            reference = memory->symbol;
            kind = memory->kind;
        }
        if (!reference) fail(at, "an indirect operand requires a register loaded from memrefin[0]");
        reference->storage = Storage::MemRef;
        if (kind == AssemblyPointerKind::Frame) data_->hasNativeFrame = true;
        return {.token = at, .symbol = *reference, .address = address, .pointerKind = kind};
    }

    AssemblyOperand assemblyNamedOperand(Token name, bool allowByte = false)
    {
        const auto byte = std::ranges::find(byteRegisterNames, lowercase(name.text));
        if (byte != std::end(byteRegisterNames)) {
            if (!allowByte) fail(name, "this instruction does not support byte-register operands");
            const auto index = static_cast<std::size_t>(byte - std::begin(byteRegisterNames));
            return {.token = name, .gpr = index % byteRegisterParentCount, .byte = true,
                    .byteShift = index >= byteRegisterParentCount ? 8U : 0U};
        }
        const auto reg = std::ranges::find(xmmNames, lowercase(name.text));
        if (reg != std::end(xmmNames)) return {name, static_cast<std::size_t>(reg - std::begin(xmmNames))};
        if (const auto gpr = assemblyGpr(name.text)) return {.token = name, .gpr = gpr};
        const auto frame = std::ranges::find(assemblyFrameSymbols, name.text, &AssemblyFrameSymbol::name);
        if (!symbols_.contains(name.text) && frame != std::end(assemblyFrameSymbols)) {
            const auto& stage = assemblyStages_[activeStage_];
            const auto* base = std::get_if<AssemblyPointer>(&stage.references[6]);
            if (!stage.written[6] || !base) fail(name, "a native frame alias needs an RBP memory reference");
            auto symbol = base->symbol;
            symbol.storage = Storage::MemRef;
            symbol.type = DspType::Float;
            AssemblyAddress address;
            if (accept("[")) address = assemblyAddress(symbol);
            if (address.indexCount == address.indices.size()) fail(name, "a frame alias accepts one numeric index");
            address.indices[address.indexCount++] = {name, 6, 0};
            address.displacement += frame->offset;
            if (address.displacement < std::numeric_limits<std::int32_t>::min() || address.displacement > std::numeric_limits<std::int32_t>::max())
                fail(name, "an Assembler address must fit in a signed 32-bit byte offset");
            if (base->kind == AssemblyPointerKind::Frame) data_->hasNativeFrame = true;
            return {.token = name, .symbol = symbol, .address = address, .pointerKind = base->kind};
        }
        auto symbol = assemblySymbol(name);
        if (symbol.storage == Storage::EnginePointer) {
            if (token_.text == "[") fail(name, "use [engine_pointer+byte_offset] to dereference a __const binding");
            data_->enginePointerUsed[symbol.port - enginePointerPortBase] = true;
        }
        if (symbol.storage == Storage::MemRef) {
            expect("[");
            const auto location = take();
            const auto field = unsignedNumber(location);
            expect("]");
            if (field > 4) fail(location, "memrefin fields must be in the range 0 to 4");
            return {.token = name, .symbol = symbol, .memRefField = static_cast<unsigned>(field)};
        } else if (symbol.storage == Storage::MemArrayRef) {
            expect("[");
            const auto offset = take();
            const auto index = unsignedNumber(offset);
            expect("]");
            if (index > 7) fail(offset, "memarrayrefin fields must be in the range 0 to 7");
            return {.token = name, .symbol = symbol, .memArrayField = static_cast<unsigned>(index)};
        } else if (accept("[")) {
            return {.token = name, .symbol = symbol, .address = assemblyAddress(symbol)};
        } else if (symbol.arraySize != 0) {
            return {.token = name, .symbol = symbol, .address = AssemblyAddress{}};
        }
        return {name, std::nullopt, symbol};
    }

    static std::optional<std::size_t> assemblyMemoryStart(const AssemblyOperand& operand, DspType type)
    {
        if (!operand.address) return 0;
        if (operand.address->indexCount != 0) return std::nullopt;
        const auto offset = operand.address->displacement;
        const auto bytes = assemblyElementBytes(type);
        if (offset < 0 || offset % bytes != 0) return std::numeric_limits<std::size_t>::max();
        return static_cast<std::size_t>(offset / bytes);
    }

    static AssemblyMemoryCell assemblyMemoryCell(Symbol symbol, std::size_t element)
    {
        return symbol.arraySize == 0 ? AssemblyMemoryCell{Storage::Local, symbol.slot, element}
                                    : AssemblyMemoryCell{Storage::Local, symbol.slot + element, 0};
    }

    std::optional<std::int32_t> assemblyMemRefOffset(const AssemblyOperand& operand) const
    {
        std::uint32_t offset = static_cast<std::uint32_t>(operand.address->displacement);
        if (operand.address->engineBase) offset += engineRootAddress;
        for (std::size_t i = 0; i < operand.address->indexCount; ++i) {
            const auto& index = operand.address->indices[i];
            const auto* pointer = std::get_if<AssemblyPointer>(&assemblyStages_[activeStage_].references[index.reg]);
            if (!pointer || !pointer->offset || index.shift != 0) return std::nullopt;
            offset += static_cast<std::uint32_t>(*pointer->offset);
        }
        return std::bit_cast<std::int32_t>(offset);
    }

    void noteAssemblyMemRefWrite(const AssemblyOperand& operand, DspType type, unsigned mask,
                                 const AssemblyTargets* targets, unsigned byteWidth = 0)
    {
        auto& stage = assemblyStages_[activeStage_];
        auto& memory = stage.memoryTargets;
        const auto offset = assemblyMemRefOffset(operand);
        if (targets) {
            if (std::ranges::find(data_->targetMemoryPorts, operand.symbol.port) == data_->targetMemoryPorts.end()) {
                data_->targetMemoryPorts.push_back(operand.symbol.port);
            }
            if (!offset || stage.indexedMemoryTargets.contains(operand.symbol.port)) {
                auto& possible = stage.indexedMemoryTargets[operand.symbol.port];
                possible = mergeAssemblyTargets(possible, *targets);
                for (const auto& [cell, saved] : memory) {
                    if (cell.storage == Storage::MemRef && cell.slot == operand.symbol.port) {
                        possible = mergeAssemblyTargets(possible, saved);
                    }
                }
            }
        }
        const auto bytes = byteWidth ? byteWidth : assemblyElementBytes(type);
        for (auto cell = memory.begin(); cell != memory.end();) {
            if (cell->first.storage != Storage::MemRef) {
                ++cell;
                continue;
            }
            // Connected Mem inputs may share backing bytes. Writes invalidate facts for other ports.
            bool affected = cell->first.slot != operand.symbol.port || !offset;
            for (unsigned lane = 0; !affected && lane < assemblyPacketLanes(type); ++lane) {
                if ((mask & (1U << lane)) == 0) continue;
                const auto first = static_cast<std::int64_t>(*offset) + lane * bytes;
                const auto saved = static_cast<std::int64_t>(cell->first.offset);
                affected = first < saved + 4 && saved < first + bytes;
            }
            if (affected) cell = memory.erase(cell);
            else ++cell;
        }
        if (targets && offset && *offset >= 0 && (mask & 1U) != 0) {
            memory.insert_or_assign({Storage::MemRef, operand.symbol.port, static_cast<std::size_t>(*offset)}, *targets);
        }
    }

    void noteAssemblyMemoryWrite(const AssemblyOperand& operand, DspType type, unsigned mask,
                                 const AssemblyTargets* targets = nullptr)
    {
        const auto symbol = operand.symbol;
        if (symbol.storage == Storage::MemRef) {
            noteAssemblyMemRefWrite(operand, type, mask, targets);
            return;
        }
        if (symbol.storage != Storage::Local) {
            if (targets) fail(operand.token, "save a local routine target in writable memory");
            return;
        }
        auto& memory = assemblyStages_[activeStage_].memoryTargets;
        const auto first = assemblyMemoryStart(operand, type);
        if (!first) {
            // An indexed write can affect any cell of this object. Untouched cells retain their possible labels.
            for (auto cell = memory.begin(); cell != memory.end();) {
                const bool affected = cell->first.storage == Storage::Local &&
                    (symbol.arraySize == 0 ? cell->first.slot == symbol.slot
                        : cell->first.slot >= symbol.slot && cell->first.slot - symbol.slot < symbol.arraySize);
                if (!affected) {
                    ++cell;
                } else if (targets) {
                    cell->second = mergeAssemblyTargets(cell->second, *targets);
                    ++cell;
                } else {
                    cell = memory.erase(cell);
                }
            }
            return;
        }
        const auto count = symbol.arraySize == 0 ? DspLanes{}.size() : symbol.arraySize;
        if (*first >= count) return;
        for (unsigned lane = 0; lane < assemblyPacketLanes(type) && lane < count - *first; ++lane) {
            if ((mask & (1U << lane)) == 0) continue;
            const auto cell = assemblyMemoryCell(symbol, *first + lane);
            if (targets) memory.insert_or_assign(cell, *targets);
            else memory.erase(cell);
        }
    }

    void noteAssemblyPacketWrite(const AssemblyOperand& operand, DspType type, unsigned mask)
    {
        const auto symbol = operand.symbol;
        if (symbol.storage != Storage::Local || assemblyElementBytes(type) == assemblyElementBytes(symbol.type) ||
            (operand.address && operand.address->indexCount != 0)) {
            noteAssemblyMemoryWrite(operand, type, mask);
            return;
        }
        const auto offset = operand.address ? operand.address->displacement : 0;
        if (offset < 0 || offset % 4 != 0) return;
        const auto storageBytes = assemblyElementBytes(symbol.type);
        const auto bytes = assemblyElementBytes(type);
        const auto count = symbol.arraySize == 0 ? DspLanes{}.size() : symbol.arraySize;
        for (unsigned lane = 0; lane < assemblyPacketLanes(type); ++lane) {
            if ((mask & (1U << lane)) == 0) continue;
            const auto first = static_cast<std::uint64_t>(offset) + lane * bytes;
            if (first + bytes > count * storageBytes) continue;
            for (unsigned word = 0; word < bytes / 4; ++word) {
                assemblyStages_[activeStage_].memoryTargets.erase(assemblyMemoryCell(symbol, (first + word * 4) / storageBytes));
            }
        }
    }

    void noteAssemblyByteWrite(const AssemblyOperand& operand)
    {
        const auto symbol = operand.symbol;
        if (symbol.storage == Storage::MemRef) {
            noteAssemblyMemRefWrite(operand, DspType::Int, 1, nullptr, 1);
            return;
        }
        if (symbol.storage != Storage::Local) return;
        const auto bytes = assemblyElementBytes(symbol.type);
        const auto count = symbol.arraySize ? symbol.arraySize : DspLanes{}.size();
        if (operand.address->indexCount == 0) {
            const auto offset = operand.address->displacement;
            if (offset >= 0 && static_cast<std::uint64_t>(offset) < count * bytes) {
                assemblyStages_[activeStage_].memoryTargets.erase(assemblyMemoryCell(symbol, offset / bytes));
            }
        } else {
            // A dynamic byte write can overlap any saved target in this object.
            auto& memory = assemblyStages_[activeStage_].memoryTargets;
            std::erase_if(memory, [&](const auto& entry) {
                const auto& cell = entry.first;
                return cell.storage == Storage::Local && (symbol.arraySize == 0 ? cell.slot == symbol.slot
                    : cell.slot >= symbol.slot && cell.slot - symbol.slot < symbol.arraySize);
            });
        }
    }

    std::optional<AssemblyTargets> assemblySavedTargets(const AssemblyOperand& operand) const
    {
        if (operand.reg || operand.gpr || operand.immediate || operand.memRefField) return std::nullopt;
        const auto& memory = assemblyStages_[activeStage_].memoryTargets;
        if (operand.symbol.storage == Storage::MemRef) {
            const auto offset = assemblyMemRefOffset(operand);
            if (offset) {
                if (*offset < 0) return std::nullopt;
                const auto cell = memory.find({Storage::MemRef, operand.symbol.port, static_cast<std::size_t>(*offset)});
                if (cell != memory.end()) return cell->second;
                const auto dynamic = assemblyStages_[activeStage_].indexedMemoryTargets.find(operand.symbol.port);
                return dynamic == assemblyStages_[activeStage_].indexedMemoryTargets.end()
                    ? std::nullopt : std::optional<AssemblyTargets>{dynamic->second};
            }
            AssemblyTargets targets;
            if (const auto found = assemblyStages_[activeStage_].indexedMemoryTargets.find(operand.symbol.port);
                found != assemblyStages_[activeStage_].indexedMemoryTargets.end()) targets = found->second;
            for (const auto& [cell, saved] : memory) {
                if (cell.storage == Storage::MemRef && cell.slot == operand.symbol.port) {
                    targets = mergeAssemblyTargets(targets, saved);
                }
            }
            return targets.labels.empty() ? std::nullopt : std::optional<AssemblyTargets>{std::move(targets)};
        }
        if (operand.symbol.storage != Storage::Local) return std::nullopt;
        const auto first = assemblyMemoryStart(operand, DspType::Int);
        const auto count = operand.symbol.arraySize == 0 ? DspLanes{}.size() : operand.symbol.arraySize;
        if (first && *first >= count) return std::nullopt;
        AssemblyTargets result;
        const auto begin = first.value_or(0);
        const auto end = first ? *first + 1 : count;
        for (auto element = begin; element < end; ++element) {
            const auto cell = memory.find(assemblyMemoryCell(operand.symbol, element));
            if (cell == memory.end()) return std::nullopt;
            result = mergeAssemblyTargets(result, cell->second);
        }
        return result;
    }

    void noteInput(Token name, Symbol source)
    {
        if (source.storage != Storage::Input) return;
        if (activeStage_ == 0) fail(name, "read stream inputs after stage(0)");
        data_->stageInputs[activeStage_].push_back(source.port);
    }

    void noteDestination(Token name, Symbol destination)
    {
        if (destination.storage == Storage::Constant) fail(name, "cannot assign to an Assembler engine constant");
        if (destination.storage == Storage::EnginePointer) fail(name, "cannot assign to a __const engine address; write through the pointer instead");
        if (assembler_ && destination.storage == Storage::Input) {
            if (activeStage_ == 0) fail(name, "write signal inputs after stage(0)");
            // Signal operands are this block's stage-local working slots, never caller buffers.
            data_->writableInputs.resize(data_->inputs.size());
            data_->writableInputs[destination.port] = true;
            data_->stageInputs[activeStage_].push_back(destination.port);
            return;
        }
        if (destination.storage == Storage::Input || destination.storage == Storage::ArrayInput ||
            destination.storage == Storage::MemArrayRef) {
            fail(name, "cannot assign to a DSP input");
        }
        if (destination.storage != Storage::Output) return;
        if (assembler_ && activeStage_ == 0) {
            data_->initializedOutputs.resize(data_->outputs.size());
            data_->initializedOutputs[destination.port] = true;
            return;
        }
        if (activeStage_ == 0) fail(name, "assign DSP outputs in stage(1), stage(2), or stage(3)");
        auto& stage = data_->outputStages[destination.port];
        if (stage != 0 && stage != activeStage_) fail(name, "assign a DSP output in only one stage");
        stage = activeStage_;
    }

    void loadEngineRoot(std::size_t port)
    {
        emit({DspOp::MemRefPresent, DspType::Int, port}, 1);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = engineRootAddress}, 1);
        emit({DspOp::Multiply, DspType::Int}, -1);
    }

    void loadAssemblyAddress(const AssemblyAddress& address)
    {
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = static_cast<std::uint32_t>(address.displacement)}, 1);
        if (address.engineBase) {
            loadEngineRoot(address.engineBase->port);
            emit({DspOp::Add, DspType::Int}, -1);
        }
        for (std::size_t i = 0; i < address.indexCount; ++i) {
            const auto [token, reg, shift] = address.indices[i];
            loadGpr(token, reg);
            if (shift != 0) {
                emit({.op = DspOp::Constant, .type = DspType::Int, .bits = shift}, 1);
                emit({DspOp::ShiftLeft, DspType::Int}, -1);
            }
            emit({DspOp::Add, DspType::Int}, -1);
        }
    }

    std::size_t engineAddressBaseSlot(const AssemblyOperand& operand) const
    {
        if (operand.symbol.port < enginePointerPortBase || operand.symbol.port == nativeFramePort || operand.address->engineBase) return 0;
        const auto& stage = assemblyStages_[activeStage_];
        for (std::size_t i = 0; i < operand.address->indexCount; ++i) {
            const auto reg = operand.address->indices[i].reg;
            if (std::holds_alternative<AssemblyPointer>(stage.references[reg])) return *stage.gprs[reg] + 1;
        }
        return 0;
    }

    void assemblyMemory(DspOp op, const AssemblyOperand& operand, DspType type, DspSelection selection,
                        const AssemblyTargets* targets = nullptr)
    {
        const bool read = op == DspOp::LoadAssemblyMemory || op == DspOp::LoadAssemblyPacket;
        if (!read) {
            if (op == DspOp::StoreAssemblyPacket) noteAssemblyPacketWrite(operand, type, selection.mask);
            else noteAssemblyMemoryWrite(operand, type, selection.mask, targets);
        }
        const auto symbol = operand.symbol;
        const auto storage = symbol.storage == Storage::ArrayInput ? AssemblyStorage::ArrayInput
            : symbol.arraySize != 0 ? AssemblyStorage::LocalArray : AssemblyStorage::Packet;
        const auto count = symbol.arraySize == 0 ? DspLanes{}.size() : symbol.arraySize;
        loadAssemblyAddress(*operand.address);
        if (symbol.storage == Storage::MemRef) {
            emit({.op = read ? DspOp::LoadMemRef : DspOp::StoreMemRef, .type = type, .slot = symbol.port, .count = engineAddressBaseSlot(operand),
                  .bits = targets ? 1U : 0U, .lanes = selection}, read ? 0 : -2);
            return;
        }
        emit({.op = op, .type = type, .slot = symbol.slot, .count = count,
              .bits = static_cast<std::uint64_t>(storage), .lanes = selection, .storageType = symbol.type},
             read ? 0 : -2);
    }

    std::size_t assemblyXmmSlot(unsigned reg)
    {
        auto& assigned = assemblyStages_[activeStage_].registers[reg];
        if (!assigned) {
            assigned = data_->slotCount++;
            data_->incomingXmmSlots[activeStage_].emplace_back(reg, *assigned);
        }
        return *assigned;
    }

    void loadAssembly(AssemblyOperand operand, DspType type, DspSelection selection = {})
    {
        if (operand.gpr) fail(operand.token, "expected a named operand or XMM register");
        if (operand.symbol.storage == Storage::EnginePointer) fail(operand.token, "dereference a __const engine pointer to read its data");
        if (operand.memRefField == 0) fail(operand.token, "load the Mem reference into an integer register with mov");
        if (operand.reg && !assemblyStages_[activeStage_].xmmWritten[*operand.reg]) {
            fail(operand.token, "write the XMM register before reading it on every path");
        }
        if (operand.memRefField) {
            const auto field = *operand.memRefField;
            const auto limitSlot = operand.symbol.slot + static_cast<std::size_t>(MemField::Limit);
            if (field == 4) {
                emit({.op = DspOp::LoadArrayPacket, .type = DspType::Float, .slot = limitSlot}, 1);
            } else {
                // Native numeric fields occupy eight bytes each, in both builds. A packed read
                // spans two adjacent fields; field 3 overlaps the first two Float limit words.
                emit({.op = DspOp::LoadArrayPacket, .type = DspType::Double, .slot = operand.symbol.slot,
                      .lanes = {.broadcast = static_cast<int>(field)}}, 1);
                if (field < 3) {
                    emit({.op = DspOp::LoadArrayPacket, .type = DspType::Double, .slot = operand.symbol.slot,
                          .lanes = {.broadcast = static_cast<int>(field + 1)}}, 1);
                } else {
                    emit({.op = DspOp::LoadArrayPacket, .type = DspType::Float, .slot = limitSlot}, 1);
                    emit({.op = DspOp::Reinterpret128, .type = DspType::Double});
                }
                emit({.op = DspOp::ShufflePd, .type = DspType::Double}, -1);
            }
            emit({.op = DspOp::Reinterpret128, .type = type, .lanes = selection});
        } else if (operand.reg) {
            // Registers have one canonical representation: four raw 32-bit words.
            emit({.op = DspOp::Load, .type = DspType::Float, .slot = assemblyXmmSlot(*operand.reg)}, 1);
            emit({.op = DspOp::Reinterpret128, .type = type, .lanes = selection});
        } else if (operand.memArrayField) {
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = *operand.memArrayField * 8U}, 1);
            emit({.op = DspOp::LoadMemArrayField, .type = type, .slot = operand.symbol.port, .lanes = selection}, 0);
        } else {
            noteInput(operand.token, operand.symbol);
            if (!operand.address) operand.address = AssemblyAddress{};
            assemblyMemory(DspOp::LoadAssemblyPacket, operand, type, selection);
        }
    }

    void storeAssembly(const AssemblyOperand& operand, XmmWidth width, unsigned mask = 0xf)
    {
        if (operand.gpr) fail(operand.token, "expected a named operand or XMM register");
        if (operand.symbol.storage == Storage::EnginePointer) fail(operand.token, "dereference a __const engine pointer to read its data");
        if (operand.memRefField) fail(operand.token, "cannot write a Mem descriptor");
        auto slot = operand.symbol.slot;
        if (operand.reg) {
            auto& stage = assemblyStages_[activeStage_];
            slot = assemblyXmmSlot(*operand.reg);
            stage.xmmWritten[*operand.reg] = true;
        } else {
            noteDestination(operand.token, operand.symbol);
        }
        const auto type = width == XmmWidth::Bits64 ? DspType::Double : DspType::Float;
        if (operand.reg) {
            emit({DspOp::Reinterpret128, DspType::Float});
            const auto words = type == DspType::Double ? ((mask & 1U) ? 3U : 0U) | ((mask & 2U) ? 12U : 0U) : mask;
            emit({.op = DspOp::Store, .type = DspType::Float, .slot = slot, .lanes = {.mask = words}}, -1);
        } else {
            auto memory = operand;
            if (!memory.address) memory.address = AssemblyAddress{};
            emit({DspOp::Reinterpret128, type});
            assemblyMemory(DspOp::StoreAssemblyPacket, memory, type, {.mask = mask});
        }
    }

    static void checkAssemblyStacks(Token at, const AssemblyStage& stage)
    {
        if (stage.x87Depth != 0) fail(at, "the x87 stack must be empty at the end of each Assembler stage");
        if (!stage.stackReferences.empty()) fail(at, "balance push and pop before leaving the Assembler stage");
    }

    void mergeAssemblyJumps(Token at, const std::vector<AssemblyJump>& jumps)
    {
        auto& stage = assemblyStages_[activeStage_];
        auto& code = data_->stages[activeStage_];
        for (const auto& jump : jumps) {
            code[jump.instruction].slot = code.size();
            if (!stage.branches.reachable) {
                static_cast<AssemblyState&>(stage) = jump.state;
                stage.branches.reachable = true;
                stage.branches.skippingSubroutine = false;
                stage.branches.stoppedByCall = false;
                continue;
            }
            if (jump.state.x87Depth != stage.x87Depth) fail(at, "x87 stack depth differs across control-flow paths");
            if (jump.state.stackReferences.size() != stage.stackReferences.size()) {
                fail(at, "integer stack depth or references differ across paths");
            }
            for (std::size_t i = 0; i < stage.stackReferences.size(); ++i) {
                auto& saved = stage.stackReferences[i];
                const auto& incoming = jump.state.stackReferences[i];
                if (!saved || !incoming) { saved.reset(); continue; }
                const auto merged = mergeAssemblyReferences(*saved, *incoming);
                if (!merged) fail(at, "integer stack depth or references differ across paths");
                saved = *merged;
            }
            for (std::size_t i = 0; i < stage.xmmWritten.size(); ++i) {
                stage.xmmWritten[i] = stage.xmmWritten[i] && jump.state.xmmWritten[i];
            }
            for (std::size_t i = 0; i < stage.written.size(); ++i) {
                const auto merged = mergeAssemblyReferences(stage.references[i], jump.state.references[i]);
                stage.written[i] = stage.written[i] && jump.state.written[i] && merged.has_value();
                stage.references[i] = stage.written[i] ? *merged : AssemblyReference{};
            }
            // Runtime tags let indexed target possibilities survive a branch with no write.
            for (const auto& [port, targets] : jump.state.indexedMemoryTargets) {
                stage.indexedMemoryTargets[port] = mergeAssemblyTargets(stage.indexedMemoryTargets[port], targets);
            }
            for (const auto* cells : std::array<const std::map<AssemblyMemoryCell, AssemblyTargets>*, 2>{
                     &stage.memoryTargets, &jump.state.memoryTargets}) {
                for (const auto& [cell, targets] : *cells) {
                    if (cell.storage == Storage::MemRef && stage.indexedMemoryTargets.contains(cell.slot)) {
                        auto& possible = stage.indexedMemoryTargets[cell.slot];
                        possible = mergeAssemblyTargets(possible, targets);
                    }
                }
            }
            for (auto cell = stage.memoryTargets.begin(); cell != stage.memoryTargets.end();) {
                const auto incoming = jump.state.memoryTargets.find(cell->first);
                if (incoming == jump.state.memoryTargets.end()) {
                    cell = stage.memoryTargets.erase(cell);
                } else {
                    cell->second = mergeAssemblyTargets(cell->second, incoming->second);
                    ++cell;
                }
            }
            stage.flagsAvailable &= jump.state.flagsAvailable;
            stage.carryAvailable &= jump.state.carryAvailable;
            stage.parityAvailable &= jump.state.parityAvailable;
        }
    }

    void resolveDisplacementJumps()
    {
        auto& branches = assemblyStages_[activeStage_].branches;
        const auto position = token_.text.data();
        if (branches.skipUntil && position == branches.skipUntil) branches.skipUntil = nullptr;
        if (const auto found = branches.displacementJumps.find(position); found != branches.displacementJumps.end()) {
            mergeAssemblyJumps(token_, found->second);
            branches.displacementJumps.erase(found);
        }
    }

    void assemblyLabel(Token label)
    {
        auto& branches = assemblyStages_[activeStage_].branches;
        if (!branches.labels.try_emplace(label.text).second) fail(label, "duplicate Assembler label");
        const auto incoming = branches.jumps.find(label.text);
        if (const auto subroutine = branches.subroutineEnds.find(label.text); subroutine != branches.subroutineEnds.end()) {
            if (branches.reachable || incoming != branches.jumps.end()) {
                fail(label, "use call to enter this subroutine");
            }
            lexer_ = subroutine->second.lexer;
            token_ = subroutine->second.token;
            branches.boundaries.clear();
            branches.pendingLabels.clear();
            return;
        }
        if (incoming != branches.jumps.end()) {
            mergeAssemblyJumps(label, incoming->second);
            branches.jumps.erase(incoming);
        } else if (!branches.reachable) {
            const auto source = assemblyLabels_.find(label.text);
            if (source != assemblyLabels_.end() && (source->second.possibleSubroutine || source->second.jumpTarget)) {
                // A call or a tail jump can compile this body with the caller's register and stack state.
                branches.skippingSubroutine = true;
            }
        }
        branches.pendingLabels.push_back(label.text);
    }

    AssemblyOperand scalarOperand(bool allowByte = false)
    {
        if (token_.kind == TokenKind::Identifier || token_.text == "[") return assemblyOperand(allowByte);
        const bool negative = accept("-");
        if (!negative) accept("+");
        const auto at = take();
        const auto value = unsignedNumber(at);
        if (value > (negative ? 0x80000000ULL : 0xffffffffULL)) fail(at, "Assembler immediate must fit in 32 bits");
        return {.token = at, .immediate = static_cast<std::uint32_t>(negative ? 0 - value : value)};
    }

    void loadGpr(Token at, std::size_t reg, unsigned mask = 0xf)
    {
        const auto& stage = assemblyStages_[activeStage_];
        if (!stage.written[reg]) fail(at, "write the integer register before reading it on every path");
        emit({.op = DspOp::Load, .type = DspType::Int, .slot = *stage.gprs[reg], .lanes = {.mask = mask}}, 1);
    }

    void loadScalar(const AssemblyOperand& operand, unsigned mask = 0xf)
    {
        if (operand.reg) fail(operand.token, "expected a 32-bit named memory operand, integer register, or immediate");
        if (operand.immediate) {
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = *operand.immediate}, 1);
        } else if (operand.gpr) {
            if (isReference(assemblyStages_[activeStage_].references[*operand.gpr])) {
                fail(operand.token, "expected integer data, found a reference");
            }
            if (lowercase(operand.token.text).starts_with('r')) fail(operand.token, "use a 32-bit register for integer data");
            loadGpr(operand.token, *operand.gpr, mask);
        } else if (operand.memRefField) {
            if (*operand.memRefField == 0) fail(operand.token, "load the Mem reference into an integer register with mov");
            if (*operand.memRefField == 4) {
                loadAssembly(operand, DspType::Int, {.mask = mask, .broadcast = 0});
                return;
            }
            emit({.op = DspOp::LoadArrayPacket, .type = DspType::Int, .slot = operand.symbol.slot,
                  .lanes = {.mask = mask, .broadcast = static_cast<int>(*operand.memRefField)}}, 1);
        } else {
            loadAssembly(operand, DspType::Int, {.mask = mask, .broadcast = 0});
        }
    }

    std::size_t writeGpr(std::size_t reg, AssemblyReference reference = {})
    {
        auto& stage = assemblyStages_[activeStage_];
        auto& slot = stage.gprs[reg];
        if (!slot) slot = data_->slotCount++;
        stage.written[reg] = true;
        stage.references[reg] = reference;
        return *slot;
    }

    void checkScalarMemoryDestination(const AssemblyOperand& operand)
    {
        if (operand.memRefField) fail(operand.token, "cannot write a Mem descriptor");
        noteDestination(operand.token, operand.symbol);
        if (operand.symbol.storage != Storage::MemRef && xmmWidth(operand.symbol.type) != XmmWidth::Bits32)
            fail(operand.token, "expected a named 32-bit destination");
    }

    void storeScalar(const AssemblyOperand& operand, AssemblyReference reference = {})
    {
        if (operand.reg || operand.immediate) fail(operand.token, "expected an integer register or named destination");
        if (operand.gpr) {
            if (!isReference(reference) && lowercase(operand.token.text).starts_with('r')) {
                fail(operand.token, "use a 32-bit register for integer data");
            }
            emit({DspOp::Store, DspType::Int, writeGpr(*operand.gpr, reference)}, -1);
        } else {
            if (std::holds_alternative<AssemblyPointer>(reference)) fail(operand.token, "store a Mem reference in an integer register");
            const auto* targets = std::get_if<AssemblyTargets>(&reference);
            checkScalarMemoryDestination(operand);
            if (operand.address) {
                assemblyMemory(DspOp::StoreAssemblyMemory, operand, DspType::Int, {.mask = 1}, targets);
            } else {
                noteAssemblyMemoryWrite(operand, DspType::Int, 1, targets);
                emit({.op = DspOp::Store, .type = DspType::Int, .slot = operand.symbol.slot, .lanes = {.mask = 1}}, -1);
            }
        }
    }

    void storeAssemblyFlag(std::optional<std::size_t>& slot)
    {
        if (!slot) slot = data_->slotCount++;
        emit({DspOp::Store, DspType::Bool, *slot}, -1);
    }

    void scalarFlags(DspOp test, DspType type, std::optional<std::size_t>& slot)
    {
        emit({test, type}, -1);
        storeAssemblyFlag(slot);
        assemblyStages_[activeStage_].flagsAvailable = true;
    }

    void integerCarryFlag(const AssemblyOperand& lhs, const AssemblyOperand& rhs, bool addition)
    {
        loadScalar(lhs);
        loadScalar(rhs);
        emit({.op = DspOp::IntegerCarry, .type = DspType::Int, .bits = addition}, -1);
        auto& stage = assemblyStages_[activeStage_];
        storeAssemblyFlag(stage.carryFlag);
        stage.carryAvailable = true;
    }

    // Consume a wrapped 32-bit result. Parity examines only its low byte.
    void integerParityFlag()
    {
        emit({DspOp::IntegerParity, DspType::Int});
        auto& stage = assemblyStages_[activeStage_];
        storeAssemblyFlag(stage.parityFlag);
        stage.parityAvailable = true;
    }

    void assemblyCompare()
    {
        const auto lhs = assemblyOperand(true);
        expect(",");
        const auto rhs = scalarOperand(true);
        expect(";");
        if (lhs.byte || rhs.byte) {
            assemblyByteArithmetic(lhs, rhs, DspOp::Subtract, true);
            return;
        }
        if (!lhs.gpr && !rhs.immediate) fail(rhs.token, "a named memory comparison requires an immediate");
        auto& stage = assemblyStages_[activeStage_];
        // Opaque Mem references retain their null-check semantics, not native pointer arithmetic flags.
        stage.carryAvailable = stage.parityAvailable = false;
        const auto* memory = lhs.gpr ? std::get_if<AssemblyPointer>(&stage.references[*lhs.gpr]) : nullptr;
        if (memory) {
            if (rhs.immediate != 0) fail(rhs.token, "compare a Mem reference with zero");
            if (memory->kind == AssemblyPointerKind::Engine) {
                loadGpr(lhs.token, *lhs.gpr);
            } else if (memory->kind == AssemblyPointerKind::Frame) {
                // Owned frame references are non-null; their stored integer is only a byte cursor.
                emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 1}, 1);
            } else if (memory->kind == AssemblyPointerKind::Mem) {
                emit({DspOp::LoadArrayPacket, DspType::Int, memory->symbol.slot}, 1);
            } else {
                loadGpr(lhs.token, *lhs.gpr);
            }
            emit({DspOp::Constant, DspType::Int}, 1);
            scalarFlags(DspOp::Equal, DspType::Int, stage.zeroFlag);
            if (!stage.lessFlag) stage.lessFlag = data_->slotCount++;
            emit({DspOp::Constant, DspType::Bool}, 1);
            emit({DspOp::Store, DspType::Bool, *stage.lessFlag}, -1);
            return;
        }
        integerCarryFlag(lhs, rhs, false);
        loadScalar(lhs);
        loadScalar(rhs);
        emit({DspOp::Subtract, DspType::Int}, -1);
        integerParityFlag();
        loadScalar(lhs);
        loadScalar(rhs);
        scalarFlags(DspOp::Equal, DspType::Int, stage.zeroFlag);
        loadScalar(lhs);
        loadScalar(rhs);
        scalarFlags(DspOp::Less, DspType::Int, stage.lessFlag);
    }

    void assemblyFloatCompare()
    {
        const auto lhs = assemblyOperand();
        expect(",");
        const auto rhs = assemblyOperand();
        expect(";");
        if (!lhs.reg) fail(lhs.token, "comiss requires an XMM first operand");
        if (rhs.gpr || rhs.immediate || (rhs.memRefField && *rhs.memRefField != 4)) fail(rhs.token, "comiss requires an XMM or memory second operand");
        auto& stage = assemblyStages_[activeStage_];
        const auto compare = [&](AssemblyOperand a, AssemblyOperand b, unsigned predicate) {
            // COMISS reads only DWORD zero, even in a packed or Double-backed register.
            loadAssembly(a, DspType::Float, {.broadcast = 0});
            emit({DspOp::Denorm, DspType::Float});
            loadAssembly(b, DspType::Float, {.broadcast = 0});
            emit({DspOp::Denorm, DspType::Float});
            emit({.op = DspOp::SseCompare, .type = DspType::Float, .bits = predicate}, -1);
            emit({DspOp::Convert, DspType::Bool});
        };
        compare(lhs, rhs, 3); // PF = unordered.
        storeAssemblyFlag(stage.parityFlag);
        compare(lhs, rhs, 0);
        emit({DspOp::Load, DspType::Bool, *stage.parityFlag}, 1);
        emit({DspOp::Or, DspType::Bool}, -1); // ZF = equal or unordered.
        storeAssemblyFlag(stage.zeroFlag);
        compare(rhs, lhs, 6); // CF = less or unordered (reverse NLE).
        storeAssemblyFlag(stage.carryFlag);
        emit({DspOp::Constant, DspType::Bool}, 1); // SF and OF clear, hence SF xor OF = 0.
        storeAssemblyFlag(stage.lessFlag);
        stage.flagsAvailable = stage.carryAvailable = stage.parityAvailable = true;
    }

    void loadAssemblyByteRegister(AssemblyOperand operand)
    {
        operand.byte = false;
        loadScalar(operand);
        if (operand.byteShift) {
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = operand.byteShift}, 1);
            emit({DspOp::ShiftRight, DspType::Int}, -1);
        }
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 0xff}, 1);
        emit({DspOp::And, DspType::Int}, -1);
    }

    void loadAssemblyByteValue(AssemblyOperand source)
    {
        if (source.reg || (source.gpr && !source.byte) || source.memRefField || source.memArrayField) {
            fail(source.token, "expected a byte register, memory or byte immediate");
        }
        if (source.immediate) {
            if (*source.immediate > 255 && *source.immediate < 0xffffff80U) fail(source.token, "byte immediate must fit in eight bits");
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = *source.immediate & 0xffU}, 1);
        } else if (source.byte) {
            loadAssemblyByteRegister(source);
        } else {
            noteInput(source.token, source.symbol);
            if (!source.address) source.address = AssemblyAddress{};
            loadAssemblyAddress(*source.address);
            if (source.symbol.storage == Storage::MemRef) {
                emit({.op = DspOp::LoadMemByte, .type = DspType::Int, .slot = source.symbol.port, .count = engineAddressBaseSlot(source), .lanes = {.broadcast = 0}});
            } else {
                const auto storage = source.symbol.storage == Storage::ArrayInput ? AssemblyStorage::ArrayInput
                    : source.symbol.arraySize ? AssemblyStorage::LocalArray : AssemblyStorage::Packet;
                emit({.op = DspOp::LoadAssemblyByte, .type = DspType::Int, .slot = source.symbol.slot,
                      .count = source.symbol.arraySize ? source.symbol.arraySize : DspLanes{}.size(),
                      .bits = static_cast<std::uint64_t>(storage), .storageType = source.symbol.type});
            }
        }
    }

    // Consume one captured, zero-extended byte. No assembly flags are changed by this merge/store.
    void storeAssemblyByteValue(AssemblyOperand destination)
    {
        if (!destination.byte) {
            if (destination.gpr || destination.reg || destination.memRefField || destination.memArrayField) {
                fail(destination.token, "a byte destination requires a byte register or writable memory");
            }
            noteDestination(destination.token, destination.symbol);
            if (!destination.address) destination.address = AssemblyAddress{};
            noteAssemblyByteWrite(destination);
            loadAssemblyAddress(*destination.address);
            const auto symbol = destination.symbol;
            if (symbol.storage == Storage::MemRef) {
                emit({.op = DspOp::StoreMemByte, .type = DspType::Int, .slot = symbol.port, .count = engineAddressBaseSlot(destination), .lanes = {.mask = 1}}, -2);
            } else {
                const auto storage = symbol.arraySize ? AssemblyStorage::LocalArray : AssemblyStorage::Packet;
                emit({.op = DspOp::StoreAssemblyByte, .type = DspType::Int, .slot = symbol.slot,
                      .count = symbol.arraySize ? symbol.arraySize : DspLanes{}.size(),
                      .bits = static_cast<std::uint64_t>(storage), .storageType = symbol.type}, -2);
            }
            return;
        }
        if (isReference(assemblyStages_[activeStage_].references[*destination.gpr])) {
            fail(destination.token, "cannot partially overwrite an opaque register reference");
        }
        if (destination.byteShift) {
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = destination.byteShift}, 1);
            emit({DspOp::ShiftLeft, DspType::Int}, -1);
        }
        destination.byte = false;
        loadScalar(destination);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = UINT32_MAX ^ (0xffU << destination.byteShift)}, 1);
        emit({DspOp::And, DspType::Int}, -1);
        emit({DspOp::Or, DspType::Int}, -1);
        storeScalar(destination);
    }

    void assemblyByteMove(AssemblyOperand destination, AssemblyOperand source)
    {
        loadAssemblyByteValue(source);
        storeAssemblyByteValue(destination);
    }

    void assemblyByteArithmetic(const AssemblyOperand& destination, const AssemblyOperand& source,
                                DspOp op, bool compare = false, bool preserveCarry = false)
    {
        // Capture once: high/low aliases and a destination used as a memory index must use the old operands.
        const auto left = data_->slotCount++, right = data_->slotCount++, result = data_->slotCount++;
        loadAssemblyByteValue(destination);
        emit({DspOp::Store, DspType::Int, left}, -1);
        loadAssemblyByteValue(source);
        emit({DspOp::Store, DspType::Int, right}, -1);
        const auto load = [&](std::size_t slot) { emit({DspOp::Load, DspType::Int, slot}, 1); };
        load(left);
        load(right);
        emit({op, DspType::Int}, -1);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 0xff}, 1);
        emit({DspOp::And, DspType::Int}, -1);
        emit({DspOp::Store, DspType::Int, result}, -1);

        auto& stage = assemblyStages_[activeStage_];
        // INC/DEC preserve both the incoming carry value and whether it is defined on every path.
        if (!preserveCarry) {
            load(left);
            load(right);
            if (op == DspOp::Add) {
                emit({DspOp::Add, DspType::Int}, -1); // At most 510: unsigned carry is bit 8.
                emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 0xff}, 1);
                emit({DspOp::Greater, DspType::Int}, -1);
            } else {
                emit({DspOp::Less, DspType::Int}, -1); // Unsigned byte borrow.
            }
            storeAssemblyFlag(stage.carryFlag);
            stage.carryAvailable = true;
        }
        load(result);
        integerParityFlag();
        load(result);
        emit({DspOp::Constant, DspType::Int}, 1);
        scalarFlags(DspOp::Equal, DspType::Int, stage.zeroFlag);

        const auto signedByte = [&](std::size_t slot) {
            load(slot);
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 0x80}, 1);
            emit({DspOp::Xor, DspType::Int}, -1);
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 0x80}, 1);
            emit({DspOp::Subtract, DspType::Int}, -1);
        };
        signedByte(left);
        signedByte(right);
        emit({op, DspType::Int}, -1); // Exact signed byte sum/difference: -256..255.
        emit({DspOp::Constant, DspType::Int}, 1);
        scalarFlags(DspOp::Less, DspType::Int, stage.lessFlag); // SF xor OF, including 8-bit overflow.
        if (!compare) {
            load(result);
            storeAssemblyByteValue(destination);
        }
    }

    void assemblyByteLogical(const AssemblyOperand& destination, const AssemblyOperand& source,
                             DspOp op, bool readOnly)
    {
        // Capture both old bytes before changing either alias or its effective-address parent.
        loadAssemblyByteValue(destination);
        loadAssemblyByteValue(source);
        emit({op, DspType::Int}, -1);
        const auto result = data_->slotCount++;
        emit({DspOp::Store, DspType::Int, result}, -1);
        const auto load = [&] { emit({DspOp::Load, DspType::Int, result}, 1); };
        auto& stage = assemblyStages_[activeStage_];
        emit({DspOp::Constant, DspType::Bool}, 1); // Logical instructions always clear CF and OF.
        storeAssemblyFlag(stage.carryFlag);
        stage.carryAvailable = true;
        load();
        integerParityFlag();
        load();
        emit({DspOp::Constant, DspType::Int}, 1);
        scalarFlags(DspOp::Equal, DspType::Int, stage.zeroFlag);
        load();
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 0x80}, 1);
        scalarFlags(DspOp::GreaterEqual, DspType::Int, stage.lessFlag); // SF xor OF = byte bit 7, since OF = 0.
        if (!readOnly) {
            load();
            storeAssemblyByteValue(destination);
        }
    }

    void assemblyByteShift(const AssemblyOperand& destination, const AssemblyOperand& source, DspOp op)
    {
        if (!destination.byte) fail(destination.token, "byte shift requires a byte register destination");
        const bool variable = source.byte && lowercase(source.token.text) == "cl";
        if (!variable && (!source.immediate || *source.immediate > 255)) {
            fail(source.token, "byte shift count must be CL or an immediate from 0 to 255");
        }
        // Even a zero shift must validate the complete numeric parent; no upper bits may be invented.
        if (source.immediate && (*source.immediate & 31U) == 0) {
            loadAssemblyByteValue(destination);
            storeAssemblyByteValue(destination);
            return;
        }
        auto& stage = assemblyStages_[activeStage_];
        const bool flagsAvailable = stage.flagsAvailable, carryAvailable = stage.carryAvailable,
                   parityAvailable = stage.parityAvailable;
        const auto left = data_->slotCount++, count = data_->slotCount++, result = data_->slotCount++;
        const auto load = [&](std::size_t slot) { emit({DspOp::Load, DspType::Int, slot}, 1); };
        // CL may be the destination, or share ECX with CH. Capture both old values before writing.
        loadAssemblyByteValue(destination);
        emit({DspOp::Store, DspType::Int, left}, -1);
        loadAssemblyByteValue(source);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 31}, 1);
        emit({DspOp::And, DspType::Int}, -1);
        emit({DspOp::Store, DspType::Int, count}, -1);
        auto& code = data_->stages[activeStage_];
        std::optional<std::size_t> zeroShift;
        if (variable) {
            load(count);
            emit({DspOp::Constant, DspType::Int}, 1);
            emit({DspOp::Equal, DspType::Int}, -1);
            zeroShift = code.size();
            emit({DspOp::JumpIfTrue}, -1); // A masked zero leaves every flag slot and parent bit untouched.
        }
        load(left);
        load(count);
        emit({op, DspType::Int}, -1);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 255}, 1);
        emit({DspOp::And, DspType::Int}, -1);
        emit({DspOp::Store, DspType::Int, result}, -1);

        load(left);
        if (op == DspOp::ShiftLeft) {
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 8}, 1);
            load(count);
        } else {
            load(count);
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 1}, 1);
        }
        emit({DspOp::Subtract, DspType::Int}, -1);
        emit({DspOp::ShiftRight, DspType::Int}, -1);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 1}, 1);
        emit({DspOp::And, DspType::Int}, -1);
        emit({DspOp::Convert, DspType::Bool});
        storeAssemblyFlag(stage.carryFlag);
        stage.carryAvailable = true;
        load(result);
        integerParityFlag();
        load(result);
        emit({DspOp::Constant, DspType::Int}, 1);
        scalarFlags(DspOp::Equal, DspType::Int, stage.zeroFlag);

        // For either one-bit shift SF xor OF is the original sign bit. For larger counts OF is
        // architecturally undefined; retain the existing scalar-shift convention of OF = 0.
        load(count);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 1}, 1);
        emit({DspOp::Equal, DspType::Int}, -1);
        load(left);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 128}, 1);
        emit({DspOp::GreaterEqual, DspType::Int}, -1);
        emit({DspOp::And, DspType::Bool}, -1);
        load(count);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 1}, 1);
        emit({DspOp::NotEqual, DspType::Int}, -1);
        load(result);
        emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 128}, 1);
        emit({DspOp::GreaterEqual, DspType::Int}, -1);
        emit({DspOp::And, DspType::Bool}, -1);
        emit({DspOp::Or, DspType::Bool}, -1);
        storeAssemblyFlag(stage.lessFlag);
        load(result);
        storeAssemblyByteValue(destination);
        if (zeroShift) {
            code[*zeroShift].slot = code.size();
            // CL can be zero at runtime, so it cannot define a previously unavailable flag.
            stage.flagsAvailable = flagsAvailable;
            stage.carryAvailable = carryAvailable;
            stage.parityAvailable = parityAvailable;
        }
    }

    void assemblyMove(const AssemblyOperand& destination)
    {
        if (!destination.byte && destination.gpr && token_.kind == TokenKind::Identifier &&
            !symbols_.contains(token_.text) && !assemblyGpr(token_.text) &&
            !std::ranges::contains(xmmNames, lowercase(token_.text)) && assemblyLabels_.contains(token_.text)) {
            const auto label = take();
            expect(";");
            // Portable local identifiers travel through registers and push/pop.
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = assemblyLabels_.at(label.text).id}, 1);
            storeScalar(destination, AssemblyTargets{{{label.text, assemblyLabels_.at(label.text).id}}});
            return;
        }
        const auto source = scalarOperand(true);
        expect(";");
        if (destination.byte || source.byte) {
            assemblyByteMove(destination, source);
            return;
        }
        if (!destination.gpr && !source.gpr && !source.immediate) fail(source.token, "mov requires an integer register or immediate");
        const auto& stage = assemblyStages_[activeStage_];
        if (source.symbol.storage == Storage::EnginePointer) {
            if (!destination.gpr) fail(source.token, "load a __const engine address into an integer register");
            auto symbol = source.symbol;
            symbol.storage = Storage::MemRef;
            loadEngineRoot(symbol.port);
            storeScalar(destination, AssemblyPointer{symbol, static_cast<std::int32_t>(engineRootAddress), AssemblyPointerKind::Engine});
        } else if (destination.gpr && lowercase(destination.token.text).starts_with('r') && source.address &&
                   source.pointerKind == AssemblyPointerKind::Engine) {
            loadAssemblyAddress(*source.address);
            emit({DspOp::LoadEnginePointer, DspType::Int, source.symbol.port, engineAddressBaseSlot(source)});
            storeScalar(destination, AssemblyPointer{source.symbol, std::nullopt, AssemblyPointerKind::Engine});
        } else if (source.memArrayField && *source.memArrayField >= 4) {
            if (!destination.gpr) fail(source.token, "load the MemArray table reference into an integer register");
            loadAssembly(source, DspType::Int, {.mask = 1, .broadcast = 0});
            const auto kind = static_cast<AssemblyPointerKind>(*source.memArrayField - 3);
            storeScalar(destination, AssemblyPointer{source.symbol, std::nullopt, kind});
        } else if (source.address && source.pointerKind == AssemblyPointerKind::Addresses) {
            if (!destination.gpr) fail(source.token, "load the wave reference into an integer register");
            loadScalar(source);
            storeScalar(destination, AssemblyPointer{source.symbol, std::nullopt, AssemblyPointerKind::Samples});
        } else if (source.memRefField == 0) {
            emit({DspOp::Constant, DspType::Int}, 1);
            storeScalar(destination, AssemblyPointer{source.symbol});
        } else if (destination.gpr && source.memRefField && *source.memRefField < 4 && lowercase(destination.token.text).starts_with('r')) {
            // FlowStone's engine uses pointer-sized MOV spellings for DWORD descriptor data too.
            // Define the numeric parent, clearing any former pointer/target provenance; consumers
            // still use the existing EAX/etc integer view rather than native address bits.
            loadScalar(source);
            emit({DspOp::Store, DspType::Int, writeGpr(*destination.gpr)}, -1);
        } else if (source.gpr && isReference(stage.references[*source.gpr])) {
            loadGpr(source.token, *source.gpr);
            storeScalar(destination, stage.references[*source.gpr]);
        } else {
            const auto saved = destination.gpr ? assemblySavedTargets(source) : std::nullopt;
            if (saved) guardAssemblyIndexedAddress(source);
            loadScalar(source);
            storeScalar(destination, saved ? AssemblyReference{*saved} : AssemblyReference{});
            if (saved && source.address && (source.symbol.storage == Storage::MemRef || source.address->indexCount != 0)) {
                guardAssemblySavedTarget(destination.token, *saved);
            }
        }
    }

    void assemblyIntegerDivide()
    {
        const auto at = token_;
        // An unsized memory divisor keeps the existing 32-bit form. A declared symbol named
        // byte is still a symbol; otherwise BYTE [PTR] makes the memory width explicit here.
        const bool byteMemory = lowercase(token_.text) == "byte" && !symbols_.contains(token_.text);
        if (byteMemory) {
            take();
            if (lowercase(token_.text) == "ptr") take();
        }
        const auto divisor = assemblyOperand(true);
        expect(";");
        if (byteMemory && (divisor.gpr || divisor.reg)) fail(at, "byte qualifier requires a memory divisor");
        if (divisor.reg || (divisor.memArrayField && *divisor.memArrayField >= 4)) {
            fail(at, "idiv requires a 32-bit integer register or numeric memory divisor");
        }
        const AssemblyOperand low{.token = Token{TokenKind::Identifier, "eax", at.line, at.column}, .gpr = 0};
        const AssemblyOperand high{.token = Token{TokenKind::Identifier, "edx", at.line, at.column}, .gpr = 3};
        if (divisor.byte || byteMemory) {
            // Capture AX and its divisor before AL/AH or an address index changes. The byte form
            // ignores the high input and checks its own signed quotient range in both backends.
            loadScalar(low);
            emit({DspOp::Constant, DspType::Int}, 1);
            loadAssemblyByteValue(divisor);
            emit({.op = DspOp::IntegerDivide, .type = DspType::Int, .bits = 8}, -1);
            auto destination = low;
            destination.byte = true;
            destination.byteShift = 8;
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 255}, 1);
            emit({DspOp::And, DspType::Int}, -1);
            storeAssemblyByteValue(destination);
            destination.byteShift = 0;
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = 255}, 1);
            emit({DspOp::And, DspType::Int}, -1);
            storeAssemblyByteValue(destination);
        } else {
            // Read EDX:EAX and the divisor before either implicit register changes, including aliased divisors/indices.
            loadScalar(low);
            loadScalar(high);
            loadScalar(divisor);
            emit({DspOp::IntegerDivide, DspType::Int}, -1); // Three inputs become quotient, remainder.
            storeScalar(high);
            storeScalar(low);
        }
        // IDIV leaves all arithmetic flags undefined, even if there was an earlier producer.
        auto& stage = assemblyStages_[activeStage_];
        stage.flagsAvailable = stage.carryAvailable = stage.parityAvailable = false;
    }

    void assemblyExchange()
    {
        const auto left = assemblyOperand(true);
        expect(",");
        const auto right = assemblyOperand(true);
        expect(";");
        if (left.reg || right.reg || left.memRefField || right.memRefField || left.memArrayField || right.memArrayField ||
            (!left.gpr && !right.gpr)) fail(left.token, "xchg requires integer registers or one writable memory operand");
        if (left.byte || right.byte) {
            if (left.gpr && right.gpr) {
                // Capture both old bytes, including the low/high aliases of one parent. Merging the
                // second byte must retain the first merge, without changing the other parent bits.
                loadAssemblyByteValue(left);
                loadAssemblyByteValue(right);
                storeAssemblyByteValue(left);
                storeAssemblyByteValue(right);
            } else {
                const auto& reg = left.gpr ? left : right;
                const auto& memory = left.gpr ? right : left;
                loadAssemblyByteValue(memory);
                loadAssemblyByteValue(reg);
                // An address index may alias the byte register. Store through its original value
                // before merging the old memory byte into the register; no flags are modified.
                storeAssemblyByteValue(memory);
                storeAssemblyByteValue(reg);
            }
            return;
        }
        auto& stage = assemblyStages_[activeStage_];
        const auto reference = [&](const AssemblyOperand& operand) { return stage.references[*operand.gpr]; };
        const auto read = [&](const AssemblyOperand& operand, const AssemblyReference& saved) {
            if (isReference(saved)) loadGpr(operand.token, *operand.gpr);
            else loadScalar(operand);
        };
        if (left.gpr && right.gpr) {
            if (lowercase(left.token.text).starts_with('r') != lowercase(right.token.text).starts_with('r')) {
                fail(right.token, "xchg register operands must have matching widths");
            }
            const auto leftReference = reference(left), rightReference = reference(right);
            // Capture both old values and their metadata before writing either register, including self-aliases.
            read(left, leftReference);
            read(right, rightReference);
            storeScalar(left, rightReference);
            storeScalar(right, leftReference);
            return;
        }
        const auto& reg = left.gpr ? left : right;
        const auto& memory = left.gpr ? right : left;
        if (lowercase(reg.token.text).starts_with('r')) fail(reg.token, "xchg memory operands require a 32-bit integer register");
        const auto oldRegister = reference(reg);
        if (std::holds_alternative<AssemblyPointer>(oldRegister)) fail(reg.token, "cannot store an opaque Mem reference in xchg memory");
        const auto saved = assemblySavedTargets(memory);
        if (saved) guardAssemblyIndexedAddress(memory);
        loadScalar(memory);
        read(reg, oldRegister);
        // Store memory while all address registers still hold their original values. Then replace the register.
        storeScalar(memory, oldRegister);
        storeScalar(reg, saved ? AssemblyReference{*saved} : AssemblyReference{});
        if (saved && memory.address && (memory.symbol.storage == Storage::MemRef || memory.address->indexCount != 0)) {
            guardAssemblySavedTarget(reg.token, *saved);
        }
    }

    void assemblyScalar(const AssemblyMnemonic& instruction)
    {
        const bool unitStep = instruction.form == AssemblyForm::ScalarStep;
        const bool byteArithmetic = unitStep || (instruction.form == AssemblyForm::ScalarBinary &&
            (instruction.op == DspOp::Add || instruction.op == DspOp::Subtract));
        const bool readOnly = instruction.form == AssemblyForm::ScalarTest;
        const bool byteLogical = readOnly || (instruction.form == AssemblyForm::ScalarBinary &&
            (instruction.op == DspOp::And || instruction.op == DspOp::Or || instruction.op == DspOp::Xor));
        const bool byteShift = instruction.form == AssemblyForm::ScalarBinary && isShift(instruction.op);
        const auto destination = assemblyOperand(instruction.form == AssemblyForm::ScalarMove || byteArithmetic || byteLogical || byteShift);
        if (!unitStep) expect(",");
        if (instruction.form == AssemblyForm::ScalarMove) {
            assemblyMove(destination);
            return;
        }
        auto source = unitStep ? AssemblyOperand{.token = destination.token, .immediate = 1} : scalarOperand(byteArithmetic || byteLogical || byteShift);
        expect(";");
        if (byteShift && (destination.byte || source.byte)) {
            assemblyByteShift(destination, source, instruction.op);
            return;
        }
        if (byteLogical && (destination.byte || source.byte)) {
            if (readOnly && !source.byte && !source.immediate) fail(source.token, "byte TEST requires a byte register or immediate second operand");
            assemblyByteLogical(destination, source, instruction.op, readOnly);
            return;
        }
        if (byteArithmetic && (destination.byte || source.byte)) {
            assemblyByteArithmetic(destination, source, instruction.op, false, unitStep);
            return;
        }
        auto& stage = assemblyStages_[activeStage_];
        const auto op = instruction.op;
        const bool selfXor = op == DspOp::Xor && destination.gpr && source.gpr == destination.gpr &&
            !lowercase(destination.token.text).starts_with('r') && !lowercase(source.token.text).starts_with('r');
        if (readOnly && !source.gpr && !source.immediate) fail(source.token, "test requires a register or immediate second operand");
        if (!selfXor && destination.gpr && std::holds_alternative<AssemblyPointer>(stage.references[*destination.gpr])) {
            if (op != DspOp::Add && op != DspOp::Subtract) fail(destination.token, "use add or sub to offset a Mem reference");
            loadGpr(destination.token, *destination.gpr);
            loadScalar(source);
            emit({op, DspType::Int}, -1);
            auto pointer = std::get<AssemblyPointer>(stage.references[*destination.gpr]);
            if (pointer.offset && source.immediate) {
                const auto base = static_cast<std::uint32_t>(*pointer.offset);
                pointer.offset = std::bit_cast<std::int32_t>(op == DspOp::Add ? base + *source.immediate : base - *source.immediate);
            } else {
                pointer.offset.reset();
            }
            storeScalar(destination, pointer);
            stage.flagsAvailable = stage.carryAvailable = stage.parityAvailable = false;
            return;
        }
        const bool shift = isShift(op);
        if (!unitStep && !readOnly && op != DspOp::Xor && !destination.gpr && !source.gpr && !source.immediate)
            fail(source.token, "integer operations cannot have two memory operands");
        if (op == DspOp::Xor && !destination.gpr && !source.gpr && !source.immediate) {
            fail(source.token, "xor cannot have two memory operands");
        }
        if (shift) {
            if (!source.immediate || *source.immediate > 255) fail(source.token, "integer shift requires a byte immediate");
            *source.immediate &= 31U;
            if (*source.immediate == 0) {
                loadScalar(destination);
                if (destination.gpr) {
                    storeScalar(destination);
                } else {
                    checkScalarMemoryDestination(destination);
                    // Count zero leaves memory and its pointer/target provenance untouched.
                    // Validate the read/write operand, then discard the captured value privately.
                    emit({DspOp::Store, DspType::Int, data_->slotCount++}, -1);
                }
                return;
            }
        }

        const bool sum = op == DspOp::Add || op == DspOp::Subtract;
        if (sum && !unitStep) {
            // Capture carry/borrow before an aliased destination or memory cell changes.
            integerCarryFlag(destination, source, op == DspOp::Add);
        } else if (shift) {
            loadScalar(destination);
            const auto bit = op == DspOp::ShiftLeft ? 32U - *source.immediate : *source.immediate - 1U;
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = std::uint32_t{1} << bit}, 1);
            emit({DspOp::And, DspType::Int}, -1);
            emit({DspOp::Convert, DspType::Bool});
            storeAssemblyFlag(stage.carryFlag);
            stage.carryAvailable = true;
        } else if (!unitStep) {
            // Logical instructions clear carry. INC/DEC preserve both its value and availability.
            emit({DspOp::Constant, DspType::Bool}, 1);
            storeAssemblyFlag(stage.carryFlag);
            stage.carryAvailable = true;
        }
        const bool singleShift = shift && *source.immediate == 1;
        if (sum) {
            // A binary64 sum or difference of signed 32-bit integers is exact, including overflow of the 32-bit result.
            loadScalar(destination);
            emit({DspOp::Convert, DspType::Double});
            loadScalar(source);
            emit({DspOp::Convert, DspType::Double});
            emit({op, DspType::Double}, -1);
            emit({DspOp::Constant, DspType::Double}, 1);
            scalarFlags(DspOp::Less, DspType::Double, stage.lessFlag);
        } else if (singleShift) {
            loadScalar(destination);
            emit({DspOp::Constant, DspType::Int}, 1);
            scalarFlags(DspOp::Less, DspType::Int, stage.lessFlag);
        }

        if (selfXor) {
            // XOR reg,reg defines zero without reading an incoming value or retaining its pointer/target tag.
            emit({DspOp::Constant, DspType::Int}, 1);
        } else {
            loadScalar(destination);
            loadScalar(source);
            emit({op, DspType::Int}, -1);
        }
        if (!readOnly) {
            emit({DspOp::Duplicate}, 1);
            storeScalar(destination);
        }
        emit({DspOp::Duplicate}, 1);
        integerParityFlag();
        if (!sum && !singleShift) emit({DspOp::Duplicate}, 1);
        emit({DspOp::Constant, DspType::Int}, 1);
        scalarFlags(DspOp::Equal, DspType::Int, stage.zeroFlag);
        if (!sum && !singleShift) {
            emit({DspOp::Constant, DspType::Int}, 1);
            scalarFlags(DspOp::Less, DspType::Int, stage.lessFlag);
        }
    }

    void assemblyStack(Token opcode, AssemblyForm form)
    {
        const auto operand = scalarOperand();
        expect(";");
        auto& stage = assemblyStages_[activeStage_];
        const bool recursiveFrame = assemblyRecursiveFrames_ != 0;
        auto& slots = recursiveFrame ? stage.recursiveIntegerStacks[assemblyCallDepth_ - 1] : stage.integerStack;
        if (form == AssemblyForm::Push) {
            const bool available = !operand.gpr || stage.written[*operand.gpr];
            const auto reference = operand.gpr ? stage.references[*operand.gpr] : AssemblyReference{};
            const auto depth = stage.stackReferences.size();
            if (depth == slots.size()) slots.push_back(data_->slotCount++);
            if (available) {
                if (operand.gpr) loadGpr(operand.token, *operand.gpr);
                else loadScalar(operand);
                emit({DspOp::Store, DspType::Int, slots[depth]}, -1);
                stage.stackReferences.push_back(reference);
            } else {
                stage.stackReferences.push_back(std::nullopt);
            }
        } else {
            if (!operand.gpr) fail(operand.token, "pop requires an integer register");
            if (stage.stackReferences.empty()) {
                fail(opcode, recursiveFrame ? "local subroutine cannot pop the caller's stack" : "integer stack underflow");
            }
            const auto reference = stage.stackReferences.back();
            stage.stackReferences.pop_back();
            if (reference) {
                emit({DspOp::Load, DspType::Int, slots[stage.stackReferences.size()]}, 1);
                emit({DspOp::Store, DspType::Int, writeGpr(*operand.gpr, *reference)}, -1);
            } else {
                stage.written[*operand.gpr] = false;
                stage.references[*operand.gpr] = {};
            }
        }
    }

    enum class AssemblyEncodingOperand { Other, Integer, Byte, Xmm, Memory, Immediate };

    AssemblyEncodingOperand assemblyEncodingOperand(std::span<const Token> tokens) const
    {
        using enum AssemblyEncodingOperand;
        if (tokens.size() == 1) {
            const auto token = tokens.front();
            if (token.kind == TokenKind::Number || token.kind == TokenKind::Hex) return Immediate;
            if (token.kind != TokenKind::Identifier) return Other;
            const auto name = lowercase(token.text);
            if (std::ranges::contains(gprNames, name)) return Integer;
            if (std::ranges::contains(byteRegisterNames, name)) return Byte;
            if (std::ranges::contains(xmmNames, name)) return Xmm;
            return symbols_.contains(token.text) || std::ranges::find(assemblyConstants, token.text, &AssemblyConstant::name) != std::end(assemblyConstants) || std::ranges::find(assemblyFrameSymbols, token.text, &AssemblyFrameSymbol::name) != std::end(assemblyFrameSymbols) ? Memory : Other;
        }
        if (tokens.size() == 2 && (tokens[0].text == "-" || tokens[0].text == "+")) {
            const auto kind = tokens[1].kind;
            return kind == TokenKind::Number || kind == TokenKind::Hex ? Immediate : Other;
        }
        if (tokens.size() == 4 && tokens[1].text == "[" && tokens[3].text == "]" &&
            (tokens[2].kind == TokenKind::Number || tokens[2].kind == TokenKind::Hex) &&
            (symbols_.contains(tokens[0].text) || std::ranges::find(assemblyConstants, tokens[0].text, &AssemblyConstant::name) != std::end(assemblyConstants) || std::ranges::find(assemblyFrameSymbols, tokens[0].text, &AssemblyFrameSymbol::name) != std::end(assemblyFrameSymbols))) return Memory;
        return Other;
    }

    unsigned assemblyInstructionBytes(AssemblyPosition& position, Token displacement) const
    {
        const auto opcode = position.token;
        if (opcode.kind == TokenKind::End || opcode.text == "}" || startsStageHeader(opcode, position.lexer)) {
            fail(displacement, "numeric branch displacement leaves this Assembler stage");
        }
        position.token = position.lexer.next();
        if (position.token.text == ":") {
            position.token = position.lexer.next();
            return 0;
        }
        std::array<std::vector<Token>, 3> operands;
        std::size_t index = 0;
        while (position.token.text != ";") {
            if (position.token.kind == TokenKind::End || position.token.text == "}" ||
                (startsStageHeader(position.token, position.lexer) && !compactStageHeader(position.token, position.lexer))) {
                fail(displacement, "numeric branch displacement crosses an incomplete instruction");
            }
            if (position.token.text == ",") {
                if (++index >= operands.size()) fail(displacement, "unsupported instruction width in numeric branch span");
            } else {
                operands[index].push_back(position.token);
            }
            position.token = position.lexer.next();
        }
        position.token = position.lexer.next();
        const auto name = lowercase(opcode.text);
        const auto instruction = std::ranges::find(mnemonics, name, &AssemblyMnemonic::name);
        if (instruction == std::end(mnemonics)) fail(opcode, "unsupported instruction width in numeric branch span");
        const auto& target = operands[0];
        const bool indirect = target.size() == 3 && target[0].text == "[" && target[2].text == "]" &&
                              assemblyGpr(target[1].text).has_value();
        const bool scaled = target.size() == 7 && target[0].text == "[" && target[2].text == "+" &&
                            target[4].text == "*" && target[6].text == "]" &&
                            assemblyGpr(target[1].text) && assemblyGpr(target[3].text) &&
                            (target[5].kind == TokenKind::Number || target[5].kind == TokenKind::Hex);
        const auto scale = scaled ? unsignedNumber(target[5]) : 0;
        const bool indexed = scaled && scale >= 1 && scale <= 8 && std::has_single_bit(scale);
        if ((indirect || indexed) && (instruction->form == AssemblyForm::Call ||
                                     (instruction->form == AssemblyForm::Branch && name == "jmp"))) {
            return indexed ? 3 : 2;
        }
        if (!target.empty() && target.front().text == "[" &&
            (instruction->form == AssemblyForm::Call || instruction->form == AssemblyForm::Branch)) {
            fail(opcode, "unsupported instruction width in numeric branch span");
        }
        using enum AssemblyEncodingOperand;
        if (instruction->form == AssemblyForm::ScalarMove && operands[0].size() == 1 && operands[1].size() == 1) {
            const auto pointer = symbols_.find(operands[1][0].text);
            if (pointer != symbols_.end() && pointer->second.storage == Storage::EnginePointer && assemblyGpr(operands[0][0].text))
                return lowercase(operands[0][0].text).starts_with('r') ? 10 : 5;
        }
        const auto left = assemblyEncodingOperand(operands[0]);
        const auto right = assemblyEncodingOperand(operands[1]);
        using enum AssemblyForm;
        switch (instruction->form) {
        case Branch:
            if (name == "jmp" && left == Integer) return 2;
            // FlowStone emits E9 or 0F 8x with a 32-bit displacement, including for nearby labels.
            return name == "jmp" ? 5 : 6;
        case FloatCompare:
            if (left == Xmm && (right == Xmm || (right == Memory && operands[1].front().text != "["))) {
                const auto high = [](const auto& operand) {
                    return operand.size() == 1 && std::ranges::contains(std::span{xmmNames}.subspan(8), lowercase(operand[0].text));
                };
                return 3 + (right == Memory ? 4 : 0) + (high(operands[0]) || high(operands[1]) ? 1 : 0);
            }
            break;
        case MoveHalf:
            if (left == Xmm && right == Xmm) {
                const auto high = [](const auto& operand) {
                    return std::ranges::contains(std::span{xmmNames}.subspan(8), lowercase(operand[0].text));
                };
                return 3 + (high(operands[0]) || high(operands[1]) ? 1 : 0);
            }
            break;
        case ScalarRoundToInt:
            if (left == Integer && operands[0].size() == 1 && std::ranges::contains(std::span{gprNames}, lowercase(operands[0][0].text)) &&
                (right == Xmm || (right == Memory && operands[1].size() == 1))) {
                const bool high = right == Xmm && std::ranges::contains(std::span{xmmNames}.subspan(8), lowercase(operands[1][0].text));
                return 4 + (right == Memory ? 4 : 0) + (high ? 1 : 0);
            }
            break;
        case Return: return 1;
        case Timestamp: return 2;
        case Push: case Pop: if (left == Integer) return 1; break;
        case ScalarStep: if (left == Integer || left == Byte || left == Memory) return left == Memory ? 6 : 2; break;
        case IntegerDivide:
            if (left == Byte) return 2;
            if (!operands[0].empty() && lowercase(operands[0][0].text) == "byte" && !symbols_.contains(operands[0][0].text)) {
                auto memory = std::span{operands[0]}.subspan(1);
                if (!memory.empty() && lowercase(memory[0].text) == "ptr") memory = memory.subspan(1);
                if (assemblyEncodingOperand(memory) == Memory) return 6;
                break;
            }
            if (left == Integer && !lowercase(operands[0][0].text).starts_with('r')) return 2;
            if (left == Memory && operands[0].front().text != "[") return 6;
            break;
        case Exchange:
            if (left == Byte && right == Byte) return 2;
            if ((left == Byte && right == Memory) || (left == Memory && right == Byte)) return 6;
            if (left == Integer && right == Integer) {
                const auto a = lowercase(operands[0][0].text), b = lowercase(operands[1][0].text);
                const bool wide = a.starts_with('r');
                if (wide == b.starts_with('r') && a != "eax" && b != "eax" && a != "rax" && b != "rax") return wide ? 3 : 2;
                // Accumulator exchanges have both short and ModRM encodings; their width remains unknown.
            } else if ((left == Integer && right == Memory && !lowercase(operands[0][0].text).starts_with('r')) ||
                       (left == Memory && right == Integer && !lowercase(operands[1][0].text).starts_with('r'))) return 6;
            break;
        case ScalarTest:
            if (left == Byte) {
                if (right == Byte) return 2;
                if (right == Immediate && lowercase(operands[0][0].text) != "al") return 3;
                break;
            }
            if (left == Memory && right == Byte) return 6;
            if (left == Integer && right == Integer) return 2;
            if (left == Memory && (right == Integer || right == Immediate)) return right == Immediate ? 10 : 6;
            // The accumulator has both A9 and F7 encodings; do not guess its immediate width.
            if (left == Integer && right == Immediate && lowercase(operands[0][0].text) != "eax") return 6;
            break;
        case ScalarMove: case ScalarCompare: case ScalarBinary:
            if (name == "add" || name == "sub" || name == "cmp" || name == "and" || name == "or" || name == "xor") {
                if (left == Byte) {
                    if (right == Byte) return 2;
                    if (right == Memory) return 6;
                    // AL immediates have both accumulator and ModRM encodings; do not guess.
                    if (right == Immediate && lowercase(operands[0][0].text) != "al") return 3;
                    break;
                }
                if (left == Memory && right == Byte) return 6;
            }
            if (instruction->form == ScalarMove && left == Memory && right == Byte) {
                // Absolute AL stores have both moffs and ModRM encodings; do not guess a width.
                if (operands[0].front().text != "[" && lowercase(operands[1][0].text) != "al") return 6;
                break;
            }
            if (instruction->form == ScalarMove && left == Byte) {
                if (right == Byte || right == Immediate) return 2;
                if (right == Memory && lowercase(operands[0][0].text) != "al") return 6;
                break;
            }
            if (name == "shl" || name == "shr") {
                if (left == Byte) {
                    if (right == Byte && lowercase(operands[1][0].text) == "cl") return 2;
                    // Count 1 has both D0 and C0 encodings; other immediate counts require C0 + imm8.
                    if (right == Immediate && operands[1].size() == 1 && unsignedNumber(operands[1][0]) != 1) return 3;
                }
                break;
            }
            if (left == Integer || left == Memory) {
                if (right == Immediate) return left == Memory ? 10 : 6;
                if (right == Integer || right == Memory) return left == Memory || right == Memory ? 6 : 2;
            }
            break;
        default:
            break;
        }

        if ((left == Xmm && (right == Xmm || right == Memory)) ||
            (left == Memory && right == Xmm && instruction->form == Move)) {
            constexpr std::string_view xmm3[]{"movaps", "movups", "addps", "subps", "mulps", "divps", "minps", "maxps",
                                              "sqrtps", "xorps", "andps", "andnps", "orps", "cvtdq2ps", "cvtps2pd", "rcpps"};
            constexpr std::string_view xmm4[]{"movapd", "movupd", "movss", "addpd", "subpd", "mulpd", "divpd", "minpd", "maxpd",
                                              "sqrtpd", "xorpd", "andpd", "andnpd", "orpd", "cvtdq2pd", "cvtpd2ps",
                                              "cvtps2dq", "cvttps2dq", "cvtpd2dq", "cvttpd2dq", "paddd", "psubd", "pand", "pandn", "por", "pxor",
                                              "pcmpeqd", "pcmpgtd", "pmuludq", "pslld", "psrld", "shufps"};
            unsigned bytes = 0;
            if (std::ranges::contains(xmm3, name)) bytes = 3;
            else if (std::ranges::contains(xmm4, name)) bytes = 4;
            else if (instruction->form == Compare || instruction->form == CompareAlias) {
                bytes = instruction->type == DspType::Double ? 5 : 4;
            } else if (instruction->form == Shuffle) {
                bytes = instruction->type == DspType::Double ? 5 : 4;
            }
            if (bytes != 0) {
                const auto highXmm = [](const auto& operand) {
                    return operand.size() == 1 && std::ranges::contains(std::span{xmmNames}.subspan(8), lowercase(operand[0].text));
                };
                return bytes + (left == Memory || right == Memory ? 4 : 0) +
                       (highXmm(operands[0]) || highXmm(operands[1]) ? 1 : 0);
            }
        }
        fail(opcode, "unsupported instruction width in numeric branch span");
    }

    AssemblyJump assemblyJumpState(Token target, std::size_t instruction) const
    {
        const auto& stage = assemblyStages_[activeStage_];
        return {target, instruction, stage};
    }

    bool assemblyRegisterDeadAt(std::size_t slot, std::size_t start) const
    {
        const auto& code = data_->stages[activeStage_];
        std::vector<std::size_t> pending{start};
        std::vector<bool> visited(code.size());
        while (!pending.empty()) {
            const auto pc = pending.back();
            pending.pop_back();
            if (pc >= code.size() || visited[pc]) continue;
            visited[pc] = true;
            const auto& op = code[pc];
            if (op.op == DspOp::Load && op.slot == slot) return false;
            if ((op.op == DspOp::LoadEnginePointer || op.op == DspOp::LoadMemRef || op.op == DspOp::StoreMemRef || op.op == DspOp::LoadMemByte || op.op == DspOp::StoreMemByte) &&
                op.slot >= enginePointerPortBase && op.slot != nativeFramePort && op.count && op.count - 1 == slot) return false;
            if (op.op == DspOp::Store && op.slot == slot && op.lanes.mask == 0xf) continue;
            if (op.op == DspOp::LocalCall || op.op == DspOp::LocalReturn) return false;
            if (op.op == DspOp::Jump || op.op == DspOp::JumpIfTrue) pending.push_back(op.slot);
            if (op.op != DspOp::Jump) pending.push_back(pc + 1);
        }
        return true;
    }

    std::size_t backwardAssemblyTarget(Token target, const std::optional<AssemblyJump>& entry)
    {
        if (!entry) fail(target, "backward branch target must already be reachable");
        const auto& stage = assemblyStages_[activeStage_];
        if (entry->state.x87Depth != stage.x87Depth || entry->state.stackReferences.size() != stage.stackReferences.size()) {
            fail(target, "stack state differs at backward branch target");
        }
        for (std::size_t i = 0; i < stage.stackReferences.size(); ++i) {
            const auto& saved = entry->state.stackReferences[i];
            const auto& current = stage.stackReferences[i];
            if (saved && (!current || !assemblyReferenceIncludes(*saved, *current))) {
                fail(target, "stack state differs at backward branch target");
            }
        }
        const auto checkFlag = [&](bool needed, const std::optional<std::size_t>& slot) {
            if (!needed) return;
            if (!slot) fail(target, "flags are unavailable at backward branch target");
            // A loop may replace this flag before using it. Forward edges must be
            // resolved before testing every path, just as for changed GPR references.
            assemblyDeferredLiveness_[activeStage_].push_back({target, *slot, entry->instruction, true});
        };
        checkFlag(entry->state.flagsAvailable && !stage.flagsAvailable, stage.zeroFlag);
        checkFlag(entry->state.flagsAvailable && !stage.flagsAvailable, stage.lessFlag);
        checkFlag(entry->state.carryAvailable && !stage.carryAvailable, stage.carryFlag);
        checkFlag(entry->state.parityAvailable && !stage.parityAvailable, stage.parityFlag);
        for (std::size_t i = 0; i < entry->state.xmmWritten.size(); ++i) {
            if (entry->state.xmmWritten[i] && !stage.xmmWritten[i]) {
                fail(target, "XMM register state differs at backward branch target");
            }
        }
        for (std::size_t i = 0; i < entry->state.written.size(); ++i) {
            if (entry->state.written[i] && (!stage.written[i] || !assemblyReferenceIncludes(entry->state.references[i], stage.references[i]))) {
                // Forward jump placeholders are not real edges to instruction zero.
                // Verify this kill-before-read requirement once every target is resolved.
                if (!stage.gprs[i]) fail(target, "integer register state differs at backward branch target");
                assemblyDeferredLiveness_[activeStage_].push_back({target, *stage.gprs[i], entry->instruction});
            }
        }
        for (const auto& [cell, targets] : entry->state.memoryTargets) {
            const auto actual = stage.memoryTargets.find(cell);
            if (actual == stage.memoryTargets.end() || mergeAssemblyTargets(targets, actual->second) != targets) {
                fail(target, "saved target state differs at backward branch target");
            }
        }
        return entry->instruction;
    }

    std::size_t backwardDisplacementTarget(Token offset, std::size_t bytes, AssemblyBranch condition)
    {
        const auto branchWidth = condition == AssemblyBranch::Always ? 5U : 6U;
        if (bytes < branchWidth) fail(offset, "numeric branch target falls inside an instruction");
        bytes -= branchWidth;
        const auto& boundaries = assemblyStages_[activeStage_].branches.boundaries;
        auto cursor = boundaries.rbegin();
        if (bytes == 0) return backwardAssemblyTarget(offset, cursor->entry);
        for (++cursor; cursor != boundaries.rend(); ++cursor) {
            auto position = cursor->source;
            const auto width = assemblyInstructionBytes(position, offset);
            if (width > bytes) fail(offset, "numeric branch target falls inside an instruction");
            bytes -= width;
            if (bytes == 0) return backwardAssemblyTarget(offset, cursor->entry);
        }
        fail(offset, "numeric branch displacement leaves this Assembler stage");
    }

    const AssemblyTargets& assemblyRegisterTargets(Token operand, std::string_view operation) const
    {
        const auto reg = *assemblyGpr(operand.text);
        const auto& stage = assemblyStages_[activeStage_];
        const auto* targets = std::get_if<AssemblyTargets>(&stage.references[reg]);
        if (!stage.written[reg] || !targets) {
            fail(operand, std::string(operation) + " register must hold a local subroutine label on every path");
        }
        for (const auto& target : targets->labels) {
            const auto source = assemblyLabels_.find(target.label);
            if (source == assemblyLabels_.end() || source->second.id != target.id) {
                fail(operand, "register target must be a local label in this section");
            }
        }
        return *targets;
    }

    Token assemblyRegisterTarget(Token operand, std::string_view operation) const
    {
        if (assemblyGpr(operand.text)) operand.text = assemblyRegisterTargets(operand, operation).labels.front().label;
        return operand;
    }

    std::size_t assemblyLabelDestination(Token target, const AssemblyJump& jump)
    {
        auto& branches = assemblyStages_[activeStage_].branches;
        if (const auto label = branches.labels.find(target.text); label != branches.labels.end()) {
            if (branches.subroutineEnds.contains(target.text) &&
                (assemblyInvocations_.empty() || assemblyInvocations_.back().label != target.text)) {
                fail(target, "use call to enter this subroutine");
            }
            return backwardAssemblyTarget(target, label->second);
        }
        branches.jumps[target.text].push_back(jump);
        return 0;
    }

    void guardAssemblyIndexedAddress(const AssemblyOperand& operand)
    {
        if (operand.symbol.storage != Storage::MemRef) return;
        // Check provenance at the actual address, including entries initialized through a runtime index.
        loadAssemblyAddress(*operand.address);
        emit({.op = DspOp::IsMemRefTarget, .type = DspType::Bool, .slot = operand.symbol.port}, 0);
        const auto valid = assemblyJumpState(operand.token, data_->stages[activeStage_].size());
        emit({DspOp::JumpIfTrue}, -1);
        exitAssemblyStage();
        mergeAssemblyJumps(operand.token, {valid});
    }

    void guardAssemblySavedTarget(Token operand, const AssemblyTargets& targets)
    {
        std::vector<AssemblyJump> valid;
        for (const auto& target : targets.labels) {
            loadGpr(operand, *assemblyGpr(operand.text));
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = target.id}, 1);
            emit({DspOp::Equal, DspType::Int}, -1);
            valid.push_back(assemblyJumpState(operand, data_->stages[activeStage_].size()));
            emit({DspOp::JumpIfTrue}, -1);
        }
        // Indexed loads can be out of bounds and read zero, even from an otherwise initialized target table.
        exitAssemblyStage();
        mergeAssemblyJumps(operand, valid);
    }

    void assemblyTargetDispatch(Token operand, std::size_t targetSlot, const AssemblyTargets& choices,
                                std::optional<std::size_t> reg, bool jump, AssemblySubroutine* subroutine)
    {
        auto& stage = assemblyStages_[activeStage_];
        std::vector<AssemblyJump> entries;
        for (const auto& target : choices.labels) {
            emit({.op = DspOp::Load, .type = DspType::Int, .slot = targetSlot}, 1);
            emit({.op = DspOp::Constant, .type = DspType::Int, .bits = target.id}, 1);
            emit({DspOp::Equal, DspType::Int}, -1);
            entries.push_back(assemblyJumpState(operand, data_->stages[activeStage_].size()));
            emit({DspOp::JumpIfTrue}, -1);
        }
        // Every proven label has a case. An invalid value ends only this sample's stage.
        exitAssemblyStage();
        std::vector<AssemblyJump> continuations;
        for (std::size_t i = 0; i < choices.labels.size(); ++i) {
            mergeAssemblyJumps(operand, {entries[i]});
            if (reg) stage.references[*reg] = AssemblyTargets{{choices.labels[i]}};
            auto target = operand;
            target.text = choices.labels[i].label;
            if (jump) {
                if (!subroutine || !assemblyTailJump(target, *subroutine)) {
                    const auto branch = assemblyJumpState(target, data_->stages[activeStage_].size());
                    emit({.op = DspOp::Jump, .slot = assemblyLabelDestination(target, branch)});
                    stage.branches.reachable = false;
                }
            } else {
                assemblyCall(target, false, true);
                if (stage.branches.reachable) {
                    continuations.push_back(assemblyJumpState(operand, data_->stages[activeStage_].size()));
                    emit({DspOp::Jump});
                    stage.branches.reachable = false;
                }
            }
        }
        if (jump) {
            if (subroutine) subroutine->endedWithTailJump = true;
        } else if (continuations.empty()) {
            stage.branches.stoppedByCall = true;
        } else {
            mergeAssemblyJumps(operand, continuations);
        }
    }

    bool assemblyRegisterDispatch(Token operand, bool jump, AssemblySubroutine* subroutine = nullptr)
    {
        const auto reg = assemblyGpr(operand.text);
        if (!reg) return false;
        const auto targets = assemblyRegisterTargets(operand, jump ? "jump" : "call");
        if (targets.labels.size() == 1) return false;
        assemblyTargetDispatch(operand, *assemblyStages_[activeStage_].gprs[*reg], targets, reg, jump, subroutine);
        return true;
    }

    void assemblyMemoryTransfer(bool jump, AssemblySubroutine* subroutine)
    {
        const auto operand = token_;
        const auto memory = assemblyOperand();
        expect(";");
        const auto targets = assemblySavedTargets(memory);
        if (!targets) fail(operand, "memory transfer requires a saved local routine target on every path");
        guardAssemblyIndexedAddress(memory);
        loadScalar(memory, 1);
        const auto targetSlot = data_->slotCount++;
        emit({DspOp::Store, DspType::Int, targetSlot}, -1);
        assemblyTargetDispatch(operand, targetSlot, *targets, std::nullopt, jump, subroutine);
    }

    bool assemblyTailJump(Token target, AssemblySubroutine& subroutine)
    {
        auto& stage = assemblyStages_[activeStage_];
        const auto current = stage.branches.labels.find(target.text);
        if (current != stage.branches.labels.end() && current->second) return false;
        const auto source = assemblyLabels_.find(target.text);
        if (source == assemblyLabels_.end() || source->second.reachedInStage) return false;
        const auto& invocation = assemblyInvocations_.back();
        if (target.text == invocation.label) return false;
        const auto sameFrame = std::find_if(assemblyInvocations_.rbegin(), assemblyInvocations_.rend(),
                                           [&](const AssemblyInvocation& entry) {
            return entry.depth == assemblyCallDepth_ && entry.label == target.text;
        });
        const bool earlier = source->second.entry.token.text.data() < invocation.entry.target.text.data();
        if (sameFrame == assemblyInvocations_.rend() && !source->second.possibleSubroutine && !earlier) return false;
        if (stage.stackReferences.size() != subroutine.stackDepth) {
            fail(target, "balance push and pop before a tail jump");
        }
        if (sameFrame != assemblyInvocations_.rend()) {
            emit({.op = DspOp::Jump, .slot = backwardAssemblyTarget(target, sameFrame->entry)});
            stage.branches.stoppedByCall = true;
        } else {
            assemblyCall(target, true);
            if (stage.branches.reachable) {
                subroutine.earlyReturns.push_back(assemblyJumpState(target, data_->stages[activeStage_].size()));
                emit({DspOp::Jump});
            }
        }
        stage.branches.reachable = false;
        subroutine.endedWithTailJump = true;
        return true;
    }

    void assemblyBranch(AssemblyBranch condition, AssemblySubroutine* subroutine)
    {
        using enum AssemblyBranch;
        if (token_.text == "[") {
            if (condition != Always) fail(token_, "only jmp accepts a memory target");
            assemblyMemoryTransfer(true, subroutine);
            return;
        }
        auto target = token_;
        const bool numeric = target.kind != TokenKind::Identifier;
        const bool backwards = numeric && accept("-");
        if (numeric && !backwards) accept("+");
        std::uint64_t bytes = 0;
        if (numeric) {
            if (condition != Always && condition != NotEqual) fail(target, "numeric displacements are supported by jmp and jnz");
            bytes = unsignedNumber(take());
            if (bytes > (backwards ? 0x80000000ULL : 0x7fffffffULL)) {
                fail(target, "numeric branch displacement must fit in signed 32 bits");
            }
        } else {
            take();
        }
        expect(";");
        auto& stage = assemblyStages_[activeStage_];
        if (assemblyGpr(target.text)) {
            if (condition != Always) fail(target, "only jmp accepts a register target");
            if (assemblyRegisterDispatch(target, true, subroutine)) return;
            target = assemblyRegisterTarget(target, "jump");
        }
        if (subroutine && !numeric && condition == Always && assemblyTailJump(target, *subroutine)) return;
        if (condition != Always) {
            if (!stage.flagsAvailable) fail(target, "branch requires an earlier cmp, comiss or integer arithmetic instruction on every path");
            const bool carry = condition == Above || condition == Below || condition == NotAbove || condition == NotBelow;
            const bool parity = condition == Parity || condition == NotParity;
            if (carry && !stage.carryAvailable) fail(target, "carry branch requires a defining instruction on every path");
            if (parity && !stage.parityAvailable) fail(target, "parity branch requires a defining instruction on every path");
            const bool zero = condition == Equal || condition == NotEqual;
            const auto slot = parity ? stage.parityFlag : carry ? stage.carryFlag : zero ? stage.zeroFlag : stage.lessFlag;
            emit({DspOp::Load, DspType::Bool, *slot}, 1);
            if (condition == Greater || condition == NotGreater || condition == Above || condition == NotAbove) {
                emit({DspOp::Load, DspType::Bool, *stage.zeroFlag}, 1);
                emit({DspOp::Or, DspType::Bool}, -1);
            }
            if (condition == NotEqual || condition == NotLess || condition == Greater ||
                condition == Above || condition == NotBelow || condition == NotParity) {
                emit({DspOp::Complement, DspType::Bool});
            }
        }
        const auto jump = assemblyJumpState(target, data_->stages[activeStage_].size());
        std::size_t destination = 0;
        if (!numeric) {
            destination = assemblyLabelDestination(target, jump);
        } else if (backwards && bytes != 0) {
            destination = backwardDisplacementTarget(target, bytes, condition);
        } else {
            AssemblyPosition next{lexer_, token_};
            while (bytes != 0) {
                const auto width = assemblyInstructionBytes(next, target);
                if (width > bytes) fail(target, "numeric branch target falls inside an instruction");
                bytes -= width;
            }
            stage.branches.displacementJumps[next.token.text.data()].push_back(jump);
            if (condition == Always) stage.branches.skipUntil = next.token.text.data();
        }
        emit({condition == Always ? DspOp::Jump : DspOp::JumpIfTrue, DspType::Float, destination},
             condition == Always ? 0 : -1);
        if (condition == Always) stage.branches.reachable = false;
    }

    void indexAssemblySection()
    {
        assemblyLabels_.clear();
        std::unordered_set<std::string_view> targets;
        std::unordered_set<std::string_view> jumpTargets;
        auto scan = lexer_;
        auto at = token_;
        // A compact spelling used as an operand, e.g. `call stage0;`, is not a stage boundary.
        bool statementStart = true;
        while (at.kind != TokenKind::End && !(statementStart && startsStageHeader(at, scan)) && at.text != "}") {
            if (statementStart && (at.text == "__const" || valueType(at.text) || std::ranges::find(ports, at.text, &PortDeclaration::keyword) != std::end(ports))) {
                // Declaration initializers can contain braces. Their names and
                // commas do not mark instruction or stage boundaries.
                do { at = scan.next(); } while (at.kind != TokenKind::End && at.text != ";");
                if (at.text == ";") at = scan.next();
                continue;
            }
            const auto next = scan.next();
            if (at.kind == TokenKind::Identifier) {
                auto operand = scan;
                if (next.text == ":") {
                    const auto first = operand.next();
                    if (assemblyLabelIds_.size() == std::numeric_limits<std::uint32_t>::max()) {
                        fail(at, "too many local Assembler labels");
                    }
                    const auto id = assemblyLabelIds_.try_emplace(
                        at.text.data(), static_cast<std::uint32_t>(assemblyLabelIds_.size() + 1)).first->second;
                    const auto [label, inserted] = assemblyLabels_.try_emplace(
                        at.text, AssemblyLabelSource{{operand, first}, id});
                    if (!inserted) label->second.ambiguous = true;
                } else if (next.kind == TokenKind::Identifier) {
                    const auto name = lowercase(at.text);
                    if (name == "call" && !assemblyGpr(next.text) && operand.next().text == ";") {
                        targets.insert(next.text);
                    } else if (name == "jmp" && operand.next().text == ";") {
                        jumpTargets.insert(next.text);
                    } else if (name == "mov" && assemblyGpr(next.text) && operand.next().text == ",") {
                        const auto source = operand.next();
                        if (source.kind == TokenKind::Identifier && operand.next().text == ";") targets.insert(source.text);
                    }
                }
            }
            statementStart = at.text == ";" || at.text == ":";
            at = next;
        }
        for (const auto target : targets) {
            const auto source = assemblyLabels_.find(target);
            if (source != assemblyLabels_.end()) source->second.possibleSubroutine = true;
        }
        for (const auto target : jumpTargets) {
            const auto source = assemblyLabels_.find(target);
            if (source != assemblyLabels_.end()) source->second.jumpTarget = true;
        }
    }

    void exitAssemblyStage()
    {
        auto& stage = assemblyStages_[activeStage_];
        stage.returns.push_back(data_->stages[activeStage_].size());
        emit({DspOp::Jump});
        stage.branches.reachable = false;
    }

    void assemblyCall(Token operand, bool tail = false, bool indirect = false)
    {
        if (!tail && assemblyRegisterDispatch(operand, false)) return;
        const auto callDepth = assemblyCallDepth_;
        auto& stage = assemblyStages_[activeStage_];
        const auto reg = tail ? std::nullopt : assemblyGpr(operand.text);
        const auto label = tail ? operand.text : assemblyRegisterTarget(operand, "call").text;
        const auto source = assemblyLabels_.find(label);
        if (source == assemblyLabels_.end()) fail(operand, "call requires a local subroutine in this section");
        if (source->second.ambiguous) fail(operand, "call target label must be unique in this section");
        if (tail ? source->second.reachedInStage : source->second.reachedWithoutCall) {
            fail(operand, "use call to enter this subroutine");
        }
        const bool recursive = std::ranges::contains(assemblyInvocations_, label, &AssemblyInvocation::label);
        const bool shareBody = !tail && (recursive || assemblyRecursiveFrames_ != 0);
        if (callDepth == dspNestingLimit && shareBody) {
            // Returning here would resume an invocation whose body has not run.
            exitAssemblyStage();
            stage.branches.stoppedByCall = true;
            return;
        }
        checkNesting(operand, tail ? assemblyInvocations_.size() - callDepth : callDepth);
        auto& bodies = assemblyCallBodies_[tail ? callDepth - 1 : callDepth];
        const auto bodySource = source->second.entry.token.text.data();
        std::vector<std::optional<AssemblyReference>> callerStack;
        if (shareBody) {
            // Recursive bodies own their push/pop slots at this depth. Caller values and types stay in the caller.
            callerStack = std::exchange(stage.stackReferences, {});
            const auto body = std::ranges::find_if(bodies, [&](const AssemblyCallBody& candidate) {
                return candidate.source == bodySource && candidate.input == stage;
            });
            if (body != bodies.end()) {
                emit({.op = DspOp::LocalCall, .slot = body->entry});
                stage.branches.subroutineEnds.insert(body->subroutineEnds.begin(), body->subroutineEnds.end());
                if (body->output) {
                    static_cast<AssemblyState&>(stage) = *body->output;
                } else {
                    stage.branches.reachable = false;
                    stage.branches.stoppedByCall = true;
                }
                stage.stackReferences = std::move(callerStack);
                return;
            }
        }
        if (callDepth != 0 && assemblyNestedCalls_++ >= assemblyNestedCallLimit) {
            fail(operand, "too many nested Assembler calls after expansion");
        }

        auto& code = data_->stages[activeStage_];
        const auto callInstruction = code.size();
        const auto entry = callInstruction + 2;
        std::optional<AssemblyState> input;
        if (shareBody) {
            // Shared bodies have fixed stack slots at this depth. Skip the definition when the call returns.
            input.emplace(stage);
            emit({.op = DspOp::LocalCall, .slot = entry});
            emit({DspOp::Jump});
        }
        const AssemblyPosition resume{lexer_, token_};
        if (!tail) {
            ++assemblyCallDepth_;
            assemblyRecursiveFrames_ += recursive;
        }
        assemblyInvocations_.push_back({label, assemblyCallDepth_, assemblyJumpState(source->second.entry.token, code.size())});
        // Each inlined call has its own branch targets and instruction boundaries.
        auto callerBranches = std::exchange(stage.branches, {});
        auto& branches = stage.branches;
        branches.labels.try_emplace(label);
        branches.pendingLabels.push_back(label);
        lexer_ = source->second.entry.lexer;
        token_ = source->second.entry.token;
        AssemblySubroutine subroutine{stage.stackReferences.size()};
        const bool protectStack = indirect || reg.has_value() || callDepth != 0;
        std::optional<Token> branchPastReturn;
        Token exit = operand;
        for (;;) {
            resolveDisplacementJumps();
            const auto instruction = token_;
            if (instruction.kind == TokenKind::End || instruction.text == "}" || startsStageHeader(instruction, lexer_)) {
                if (branchPastReturn) fail(*branchPastReturn, "branch target must stay inside this local subroutine");
                fail(operand, "call requires a local subroutine ending in ret");
            }
            auto scan = lexer_;
            if (!branches.skippingSubroutine && lowercase(instruction.text) == "ret" && scan.next().text != ":") {
                if (branches.reachable && protectStack && stage.stackReferences.size() != subroutine.stackDepth) {
                    fail(instruction, "balance push and pop before returning from a subroutine");
                }
                if (branches.jumps.empty() && branches.displacementJumps.empty()) {
                    exit = take();
                    expect(";");
                    break;
                }
                if (!branchPastReturn) {
                    branchPastReturn = !branches.jumps.empty() ? branches.jumps.begin()->second.front().target
                                                               : branches.displacementJumps.begin()->second.front().target;
                }
            }
            subroutine.endedWithTailJump = false;
            assemblyStatement(&subroutine);
            if (protectStack && stage.stackReferences.size() < subroutine.stackDepth) {
                fail(instruction, "local subroutine cannot pop the caller's stack");
            }
            if (subroutine.endedWithTailJump && branches.jumps.empty() && branches.displacementJumps.empty()) {
                exit = instruction;
                break;
            }
        }
        // All return paths share one continuation and must agree on the state it can read.
        mergeAssemblyJumps(exit, subroutine.earlyReturns);
        const bool returns = branches.reachable;
        if (!returns && !branches.stoppedByCall) fail(exit, "local subroutine requires a reachable ret");
        const AssemblyPosition subroutineEnd{lexer_, token_};
        if (shareBody) {
            if (returns) emit({.op = DspOp::LocalReturn, .slot = entry});
            code[callInstruction + 1].slot = code.size();
            branches.subroutineEnds.insert_or_assign(label, subroutineEnd);
            bodies.push_back({bodySource, entry, std::move(*input),
                              returns ? std::optional<AssemblyState>{stage} : std::nullopt, branches.subroutineEnds});
        }
        // The stage also needs to skip the definitions of subroutines reached through this call.
        callerBranches.subroutineEnds.merge(branches.subroutineEnds);
        callerBranches.subroutineEnds.insert_or_assign(label, subroutineEnd);
        branches = std::move(callerBranches);
        if (shareBody) stage.stackReferences = std::move(callerStack);
        if (!returns) {
            branches.reachable = false;
            branches.stoppedByCall = true;
        }
        lexer_ = resume.lexer;
        token_ = resume.token;
        assemblyInvocations_.pop_back();
        if (!tail) {
            assemblyRecursiveFrames_ -= recursive;
            --assemblyCallDepth_;
        }
    }

    std::size_t x87Slot(Token at, std::size_t index)
    {
        const auto& stage = assemblyStages_[activeStage_];
        if (index >= stage.x87Depth) fail(at, "x87 stack underflow");
        return *stage.x87Slots[stage.x87Depth - 1 - index];
    }

    std::size_t x87RegisterIndex()
    {
        const bool bracketed = accept("[");
        if (!bracketed) expect("(");
        const auto location = take();
        const auto index = unsignedNumber(location);
        if (index >= 8) fail(location, "x87 stack index must be in the range 0 to 7");
        expect(bracketed ? "]" : ")");
        return static_cast<std::size_t>(index);
    }

    std::size_t x87PushSlot(Token opcode)
    {
        auto& stage = assemblyStages_[activeStage_];
        if (stage.x87Depth == stage.x87Slots.size()) fail(opcode, "x87 stack overflow");
        auto& slot = stage.x87Slots[stage.x87Depth++];
        if (!slot) slot = data_->slotCount++;
        return *slot;
    }

    void x87Push(Token opcode, DspType type = DspType::Double)
    {
        emit({DspOp::X87Load, type, x87PushSlot(opcode)}, -1);
    }

    void x87LoadStore(Token opcode, const AssemblyMnemonic& instruction)
    {
        const auto operand = token_;
        const bool indirect = operand.text == "[";
        if (!indirect) identifier();
        const bool loading = instruction.form == AssemblyForm::X87Load;
        if (lowercase(operand.text) == "st" && (token_.text == "(" || token_.text == "[")) {
            if (instruction.type != DspType::Float) fail(operand, "expected a named memory operand");
            const auto index = x87RegisterIndex();
            const auto slot = x87Slot(operand, index);
            if (loading) {
                emit({.op = DspOp::X87Copy, .slot = x87PushSlot(opcode), .count = slot});
            } else {
                emit({.op = DspOp::X87Copy, .slot = slot, .count = x87Slot(opcode, 0)});
            }
        } else {
            auto memory = indirect ? assemblyOperand() : assemblyNamedOperand(operand);
            const auto symbol = memory.symbol;
            const bool limit = memory.memRefField == 4 && loading && instruction.type == DspType::Float;
            if (memory.reg || memory.gpr || (memory.memRefField && !limit) ||
                symbol.storage == Storage::MemArrayRef) {
                fail(operand, "x87 operand has the wrong type");
            }
            if (loading) {
                loadAssembly(memory, instruction.type, {.broadcast = 0});
                x87Push(opcode, instruction.type);
            } else {
                noteDestination(operand, symbol);
                emit({DspOp::X87Store, instruction.type, x87Slot(opcode, 0)}, 1);
                // x87 chooses the access width; the declaration only supplies the underlying
                // byte layout. A Double store may span two Float/Int cells, or a Float store
                // may update half a Double. Use the same raw byte view as packed instructions.
                if (!memory.address) memory.address = AssemblyAddress{};
                assemblyMemory(DspOp::StoreAssemblyPacket, memory, instruction.type, {.mask = 1});
            }
        }
        if (instruction.form == AssemblyForm::X87Store) --assemblyStages_[activeStage_].x87Depth;
    }

    void x87Operation(Token opcode, const AssemblyMnemonic& instruction)
    {
        using enum AssemblyForm;
        constexpr auto f64 = DspType::Double;
        const auto form = instruction.form;
        switch (form) {
        case X87Load: case X87Store: case X87StoreKeep:
            x87LoadStore(opcode, instruction);
            break;
        case X87One:
            emit({.op = DspOp::Constant, .type = f64, .bits = std::bit_cast<std::uint64_t>(1.0)}, 1);
            x87Push(opcode);
            break;
        case X87Exchange: {
            const auto top = x87Slot(opcode, 0);
            const auto index = token_.text == ";" ? 1 : [&] {
                const auto reg = identifier();
                if (lowercase(reg.text) != "st") fail(reg, "fxch expects st(i)");
                return x87RegisterIndex();
            }();
            const auto other = x87Slot(opcode, index);
            emit({.op = DspOp::X87Exchange, .slot = top, .count = other});
            break;
        }
        case X87Unary: case X87TrigPair: {
            const auto top = x87Slot(opcode, 0);
            if (form == X87TrigPair) {
                if (instruction.op == DspOp::TanRadians) {
                    emit({.op = DspOp::Constant, .type = f64, .bits = std::bit_cast<std::uint64_t>(1.0)}, 1);
                    x87Push(opcode);
                } else {
                    const auto next = x87PushSlot(opcode);
                    emit({.op = DspOp::X87Copy, .slot = next, .count = top});
                    emit({.op = DspOp::X87Unary, .slot = next, .bits = static_cast<std::uint64_t>(DspOp::CosRadians)});
                }
            }
            emit({.op = DspOp::X87Unary, .slot = top, .bits = static_cast<std::uint64_t>(instruction.op)});
            break;
        }
        case X87BinaryPop: case X87BinaryKeep: {
            const auto top = x87Slot(opcode, 0);
            const auto next = x87Slot(opcode, 1);
            emit({.op = DspOp::X87Binary, .slot = form == X87BinaryKeep ? top : next,
                  .count = form == X87BinaryKeep ? next : top, .bits = static_cast<std::uint64_t>(instruction.op)});
            if (form == X87BinaryPop) --assemblyStages_[activeStage_].x87Depth;
            break;
        }
        default: std::abort();
        }
        expect(";");
    }

    void assemblyStatement(AssemblySubroutine* subroutine = nullptr)
    {
        const AssemblyPosition position{lexer_, token_};
        const auto opcode = identifier();
        if (accept(":")) {
            assemblyLabel(opcode);
            return;
        }
        auto& branches = assemblyStages_[activeStage_].branches;
        if (!branches.pendingLabels.empty()) {
            // Adjacent labels share an instruction. Record the entry state after merging all incoming paths.
            if (branches.reachable) {
                const auto entry = assemblyJumpState(opcode, data_->stages[activeStage_].size());
                for (const auto label : branches.pendingLabels) {
                    branches.labels.at(label) = entry;
                    if (assemblyInvocations_.empty() || assemblyInvocations_.back().label != label) {
                        const auto source = assemblyLabels_.find(label);
                        if (source != assemblyLabels_.end()) {
                            source->second.reachedWithoutCall = true;
                            source->second.reachedInStage |= assemblyInvocations_.empty();
                        }
                    }
                }
            }
            branches.pendingLabels.clear();
        }
        if (trackAssemblyBoundaries_) {
            std::optional<AssemblyJump> entry;
            if (branches.reachable) entry = assemblyJumpState(opcode, data_->stages[activeStage_].size());
            branches.boundaries.push_back({position, std::move(entry)});
        }
        if (!branches.reachable) {
            if (branches.skipUntil) {
                auto skipped = position;
                static_cast<void>(assemblyInstructionBytes(skipped, opcode));
                lexer_ = skipped.lexer;
                token_ = skipped.token;
            } else if (branches.skippingSubroutine || branches.stoppedByCall ||
                       (subroutine && (!subroutine->earlyReturns.empty() || lowercase(opcode.text) == "ret"))) {
                while (!accept(";")) {
                    if (token_.kind == TokenKind::End || token_.text == "}" ||
                        (startsStageHeader(token_, lexer_) && !compactStageHeader(token_, lexer_))) {
                        fail(opcode, "expected ';' in Assembler instruction");
                    }
                    take();
                }
            } else {
                fail(opcode, "unreachable Assembler instruction");
            }
            return;
        }
        const auto instruction = std::ranges::find(mnemonics, lowercase(opcode.text), &AssemblyMnemonic::name);
        if (instruction == std::end(mnemonics)) {
            fail(opcode, "unsupported Assembler instruction '" + std::string(opcode.text) + "'");
        }
        using enum AssemblyForm;
        const auto form = instruction->form;
        if (form == ScalarCompare) {
            assemblyCompare();
            return;
        }
        if (form == FloatCompare) {
            assemblyFloatCompare();
            return;
        }
        if (form == Branch || form == Call || form == Return) {
            if (form == Call) {
                if (token_.text == "[") {
                    assemblyMemoryTransfer(false, nullptr);
                } else {
                    const auto operand = identifier();
                    expect(";");
                    assemblyCall(operand);
                }
            } else if (form == Branch) {
                assemblyBranch(static_cast<AssemblyBranch>(instruction->predicate), subroutine);
            } else {
                expect(";");
                if (subroutine) {
                    subroutine->earlyReturns.push_back(assemblyJumpState(opcode, data_->stages[activeStage_].size()));
                    emit({DspOp::Jump});
                    branches.reachable = false;
                } else {
                    checkAssemblyStacks(opcode, assemblyStages_[activeStage_]);
                    exitAssemblyStage();
                }
            }
            return;
        }
        if (form == ScalarRoundToInt) {
            const auto destination = assemblyOperand();
            expect(",");
            const auto source = assemblyOperand();
            expect(";");
            if (!destination.gpr || lowercase(destination.token.text).starts_with('r')) {
                fail(destination.token, "cvtss2si requires a 32-bit integer register destination");
            }
            if (source.gpr || (source.memRefField && *source.memRefField != 4) || source.memArrayField) {
                fail(source.token, "cvtss2si requires an XMM or Float memory source");
            }
            loadAssembly(source, DspType::Float, {.broadcast = 0});
            emit({DspOp::Denorm, DspType::Float});
            emit({DspOp::RoundNearest, DspType::Float});
            emit({DspOp::Convert, DspType::Int});
            // Numeric writes clear any saved pointer/target provenance; EFLAGS are unchanged.
            storeScalar(destination);
            return;
        }
        if (form == IntegerDivide) {
            assemblyIntegerDivide();
            return;
        }
        if (form == Exchange) {
            assemblyExchange();
            return;
        }
        if (form == ScalarMove || form == ScalarBinary || form == ScalarTest || form == ScalarStep) {
            assemblyScalar(*instruction);
            return;
        }
        if (form == Push || form == Pop) {
            assemblyStack(opcode, form);
            return;
        }
        if (form == Timestamp) {
            expect(";");
            emit({DspOp::ReadTimestamp, DspType::Int, writeGpr(0), writeGpr(3)});
            return;
        }
        if (form >= X87Load) {
            x87Operation(opcode, *instruction);
            return;
        }
        const auto destination = assemblyOperand();
        expect(",");
        if (form == PackedShift) {
            const auto count = scalarOperand();
            if (!destination.reg) fail(destination.token, "this instruction requires an XMM destination");
            expect(";");
            loadAssembly(destination, DspType::Int);
            if (count.immediate) {
                if (*count.immediate > 255) fail(count.token, "packed shift count must fit in an unsigned byte");
                loadScalar(count);
            } else {
                loadAssembly(count, DspType::Int, {.mask = 3});
                emit({DspOp::PackedShiftCount, DspType::Int});
            }
            emit({instruction->op, DspType::Int}, -1);
            storeAssembly(destination, XmmWidth::Bits32);
            return;
        }
        const auto source = assemblyOperand();
        unsigned control = instruction->predicate;
        if (form == Compare || form == Shuffle) {
            expect(",");
            const auto location = take();
            const auto value = unsignedNumber(location);
            const unsigned limit = form == Compare ? 7 : 255;
            if (value > limit) fail(location, "Assembler immediate is outside the instruction's range");
            control = static_cast<unsigned>(value);
        }
        expect(";");

        auto width = xmmWidth(instruction->type);
        unsigned writeMask = 0xf;
        if (form == MoveHalf) {
            if (!destination.reg || !source.reg) fail(opcode, "movhlps and movlhps require two XMM registers");
            // Snapshot both operands before storing, including when they name the same register.
            // MOVHLPS = [src2,src3,dst2,dst3]; MOVLHPS = [dst0,dst1,src0,src1].
            const bool lowToHigh = control != 0;
            loadAssembly(lowToHigh ? destination : source, DspType::Float);
            loadAssembly(lowToHigh ? source : destination, DspType::Float);
            emit({.op = DspOp::ShufflePs, .type = DspType::Float, .bits = lowToHigh ? 0x44U : 0xeeU}, -1);
        } else if (form == MoveDword) {
            if (destination.reg && !source.reg) {
                if (source.gpr || source.immediate || source.memRefField) loadScalar(source, 1);
                else loadAssembly(source, DspType::Int, {.mask = 1});
            } else if (source.reg && !destination.reg) {
                loadAssembly(source, DspType::Int, {.broadcast = 0});
                if (destination.gpr) storeScalar(destination);
                else storeAssembly(destination, XmmWidth::Bits32, 1);
                return;
            } else {
                fail(opcode, "movd transfers a 32-bit operand to or from an XMM register");
            }
        } else if (form == MoveScalar) {
            if (!destination.reg && !source.reg) fail(destination.token, "a scalar SIMD move requires an XMM register");
            if (destination.reg && source.reg) {
                writeMask = 1;
                if (!assemblyStages_[activeStage_].xmmWritten[*destination.reg]) {
                    fail(destination.token, "movss needs a written XMM destination to preserve its upper lanes");
                }
            } else if (!destination.reg) {
                writeMask = 1;
            }
            loadAssembly(source, DspType::Float, {.mask = 1});
        } else if (form == ConvertPair || (instruction->type == DspType::Double &&
                                         (form == RoundToInt || form == TruncToInt))) {
            if (!destination.reg) fail(destination.token, "this instruction requires an XMM destination");
            const auto result = form != ConvertPair ? DspType::Int
                : instruction->type == DspType::Double ? DspType::Float : DspType::Double;
            // Pair conversions read two source elements and clear the unused result lanes.
            loadAssembly(source, instruction->type, {.mask = 3});
            if (instruction->type != DspType::Int) emit({DspOp::Denorm, instruction->type});
            if (form == RoundToInt) emit({DspOp::RoundNearest, instruction->type});
            emit({DspOp::Convert, result});
            if (result == DspType::Float) emit({DspOp::Denorm, result});
            width = xmmWidth(result);
        } else if (form == Reciprocal) {
            if (!destination.reg) fail(destination.token, "this instruction requires an XMM destination");
            emit({.op = DspOp::Constant, .type = DspType::Float, .bits = std::bit_cast<std::uint32_t>(1.F)}, 1);
            loadAssembly(source, DspType::Float);
            emit({DspOp::Denorm, DspType::Float});
            emit({DspOp::Divide, DspType::Float}, -1);
            emit({DspOp::Denorm, DspType::Float});
        } else if (form == Move) {
            if (!destination.reg && !source.reg &&
                !(instruction->name == "movaps" && destination.address && !source.gpr)) {
                fail(destination.token, "a SIMD move requires an XMM register or an indexed movaps destination");
            }
            loadAssembly(source, instruction->type);
        } else {
            if (!destination.reg) fail(destination.token, "this instruction requires an XMM destination");
            if (instruction->op == DspOp::Xor && destination.reg == source.reg) {
                emit({DspOp::Constant, instruction->type}, 1);
            } else {
                assemblyOperation(*instruction, destination, source, control);
            }
        }
        storeAssembly(destination, width, writeMask);
    }

    void assemblyOperation(const AssemblyMnemonic& instruction, AssemblyOperand destination,
                           AssemblyOperand source, unsigned control)
    {
        using enum AssemblyForm;
        const auto type = instruction.type;
        const auto form = instruction.form;
        const bool comparison = form == Compare || form == CompareAlias;
        const bool arithmetic = form == Unary || (form == Binary && type != DspType::Int && !isBitwise(instruction.op));
        const auto flushSubnormal = [&] { emit({DspOp::Denorm, type}); };
        const DspSelection selection{.mask = form == Shuffle && type == DspType::Double ? 3U : 0xfU};
        // Match FlowStone's SSE floating-point environment while preserving raw words in moves and bitwise operations.
        if (form == Binary || form == AndNot || form == Shuffle || comparison) {
            loadAssembly(destination, type, selection);
            if (arithmetic || comparison) flushSubnormal();
        }
        if (form == AndNot) emit({DspOp::Complement, type});
        loadAssembly(source, type, selection);
        if (arithmetic || comparison) flushSubnormal();
        switch (form) {
        case Binary: emit({instruction.op, type}, -1); break;
        case Unary: emit({instruction.op, type}); break;
        case AndNot: emit({DspOp::And, type}, -1); break;
        case Compare: case CompareAlias:
            emit({.op = DspOp::SseCompare, .type = type, .bits = control}, -1);
            break;
        case Shuffle:
            emit({.op = type == DspType::Double ? DspOp::ShufflePd : DspOp::ShufflePs,
                  .type = type, .bits = control}, -1);
            break;
        case RoundToInt: emit({DspOp::RoundNearest, type}); [[fallthrough]];
        case TruncToInt: emit({DspOp::Convert, DspType::Int}); break;
        case ToFloat: emit({DspOp::Convert, DspType::Float}); break;
        default: std::abort();
        }
        if (arithmetic) flushSubnormal();
    }

    void assignment()
    {
        const auto name = identifier();
        const auto destination = lookup(name);
        noteDestination(name, destination);
        address(destination, 0);
        const auto lanes = laneSelection();
        const auto assignment = take();
        std::optional<DspOp> compound;
        if (assignment.text != "=") {
            if (std::ranges::find(compoundOperators, assignment.text) == std::end(compoundOperators)) {
                fail(assignment, "expected '=' or a compound assignment");
            }
            compound = std::ranges::find(operators, assignment.text.substr(0, 1), &Operator::spelling)->op;
            if (destination.arraySize != 0) {
                emit({DspOp::Duplicate}, 1);
            }
            load(destination);
        }
        expression(destination.type);
        expect(";");
        if (compound) {
            emit({*compound, destination.type}, -1);
        }
        const auto scalar = destination.arraySize == 0;
        emit({scalar ? DspOp::Store : DspOp::StoreIndexed, destination.type,
              destination.slot, destination.arraySize, 0, lanes}, scalar ? -1 : -2);
    }

    void expression(DspType type, int minPrecedence = 1, std::size_t depth = 0)
    {
        prefix(type, depth);
        for (;;) {
            const auto op = std::ranges::find(operators, token_.text, &Operator::spelling);
            if (op == std::end(operators) || op->precedence < minPrecedence) {
                break;
            }
            take();
            const auto next = op->precedence + (op->op == DspOp::Power ? 0 : 1);
            expression(type, next, depth + 1);
            emit({op->op, type}, -1);
        }
        if (minPrecedence == 1 && accept("?")) {
            expression(type, 1, depth + 1);
            expect(":");
            expression(type, 1, depth + 1);
            emit({DspOp::Select, type}, -2);
        }
    }

    void prefix(DspType type, std::size_t depth)
    {
        checkNesting(token_, depth);
        if (accept("-")) {
            expression(type, 8, depth + 1);
            emit({DspOp::Negate, type});
        } else if (accept("+")) {
            prefix(type, depth + 1);
        } else if (accept("!") || accept("~")) {
            prefix(type, depth + 1);
            emit({DspOp::Complement, type});
        } else if (accept("(")) {
            expression(type, 1, depth + 1);
            expect(")");
        } else if (token_.kind == TokenKind::Number) {
            const auto literal = take();
            const auto kind = literal.text.find_first_of(".eE") == std::string_view::npos
                ? DspType::Decimal : DspType::Fraction;
            emit({.op = DspOp::Constant, .type = kind,
                  .bits = std::bit_cast<std::uint64_t>(decimalNumber(literal))}, 1);
        } else if (token_.kind == TokenKind::Hex) {
            emit({.op = DspOp::Constant, .type = DspType::Hex, .bits = unsignedNumber(take())}, 1);
        } else if (expressionOnly_ && token_.text == "pi") {
            take();
            emit({.op = DspOp::Constant, .type = DspType::Fraction, .bits = std::bit_cast<std::uint64_t>(std::numbers::pi)}, 1);
        } else if (token_.text == "true" || token_.text == "false") {
            emit({.op = DspOp::Constant, .type = DspType::Bool,
                  .bits = dspEncode(take().text == "true", DspType::Bool)}, 1);
        } else {
            const auto name = identifier();
            if (!symbols_.contains(name.text) && accept("(")) {
                function(name, type, depth + 1);
            } else {
                const auto source = lookup(name);
                noteInput(name, source);
                address(source, depth);
                load(source, laneSelection());
            }
        }
    }

    void function(Token name, DspType type, std::size_t depth)
    {
        if (expressionOnly_) {
            using enum DspOp;
            constexpr std::array functions{std::pair{"sin", Sin1}, std::pair{"cos", Cos1}, std::pair{"tan", Tan1}};
            const auto trig = std::ranges::find_if(functions, [&](auto pair) { return name.text == pair.first; });
            if (trig != functions.end()) {
                expression(type, 1, depth);
                expect(")");
                emit({.op = Constant, .type = DspType::Fraction, .bits = std::bit_cast<std::uint64_t>(2 * std::numbers::pi)}, 1);
                emit({Divide, type}, -1);
                emit({trig->second, type});
                return;
            }
        }
        if (const auto cast = valueType(name.text)) {
            // Decimal expressions inside casts keep their fraction until the cast itself.
            const auto input = *cast == DspType::Int || *cast == DspType::Bool
                ? DspType::Double : *cast;
            expression(input, 1, depth);
            expect(")");
            emit({DspOp::Convert, *cast});
            return;
        }
        const auto fn = std::ranges::find(functions, name.text, &Function::name);
        if (fn == std::end(functions)) {
            fail(name, "unsupported DSP function '" + std::string(name.text) + "'");
        }
        const bool integerMinMax = type == DspType::Int && (fn->op == DspOp::Min || fn->op == DspOp::Max);
        const auto mathType = type == DspType::Double || integerMinMax ? type : DspType::Float;
        expression(mathType, 1, depth);
        if (fn->binary) {
            expect(",");
            expression(mathType, 1, depth);
        }
        expect(")");
        emit({fn->op, mathType}, fn->binary ? -1 : 0);
    }

    bool expressionOnly_ = false;
    bool assembler_ = false;
    bool trackAssemblyBoundaries_ = false;
    struct DeferredLiveness { Token target; std::size_t slot, start; bool flags = false; };
    std::array<std::vector<DeferredLiveness>, 4> assemblyDeferredLiveness_;
    std::array<AssemblyStage, 4> assemblyStages_{};
    std::array<std::optional<Symbol>, std::size(assemblyConstants)> assemblyConstantSymbols_{};
    std::unordered_map<std::string_view, AssemblyLabelSource> assemblyLabels_;
    std::unordered_map<const char*, std::uint32_t> assemblyLabelIds_;
    std::vector<AssemblyInvocation> assemblyInvocations_;
    std::size_t assemblyCallDepth_{};
    std::size_t assemblyNestedCalls_{};
    std::size_t assemblyRecursiveFrames_{};
    std::array<std::vector<AssemblyCallBody>, dspNestingLimit> assemblyCallBodies_;
    Lexer lexer_;
    Token token_;
    std::shared_ptr<DspProgramData> data_ = std::make_shared<DspProgramData>();
    std::unordered_map<std::string_view, Symbol> symbols_;
    std::size_t stackDepth_{};
    unsigned activeStage_ = 2;
};

}

std::span<const DspWord> dspLanguageWords(DspLanguage language)
{
    static const auto words = [] {
        using enum DspWordKind;
        std::vector<DspWord> result{
            {"float", Type}, {"double", Type}, {"int", Type}, {"bool", Type},
            {"stage", Section}, {"loop", Section}, {"hop", Section},
            {"true", Literal}, {"false", Literal}, {"rand", Function},
        };
        for (const auto& port : ports) {
            if (supportsPort(port.type, false)) result.push_back({port.keyword, Port});
        }
        for (const auto& function : functions) {
            result.push_back({function.name, Function});
        }
        return result;
    }();
    if (language == DspLanguage::Code) return words;
    static const auto assemblyWords = [] {
        using enum DspWordKind;
        std::vector<DspWord> result{{"float", Type}, {"double", Type}, {"int", Type}, {"bool", Type}, {"stage", Section},
                                    {"true", Literal}, {"false", Literal}, {"st", Register}, {"__const", Type}};
        for (const auto& port : ports) {
            if (supportsPort(port.type, true)) result.push_back({port.keyword, Port});
        }
        for (const auto& mnemonic : mnemonics) result.push_back({mnemonic.name, Function});
        for (const auto reg : xmmNames) result.push_back({reg, Register});
        for (const auto reg : gprNames) result.push_back({reg, Register});
        for (const auto reg : addressRegisterNames) result.push_back({reg, Register});
        for (const auto reg : byteRegisterNames) result.push_back({reg, Register});
        for (const auto& constant : assemblyConstants) result.push_back({constant.name, Literal});
        for (const auto& field : assemblyFrameSymbols) result.push_back({field.name, Literal});
        return result;
    }();
    return assemblyWords;
}

DspProgram compileDspCode(std::string_view source, DspLanguage language)
{
    return DspProgram{Compiler(source, language).compile()};
}

DspProgram compileMathExpression(std::string_view source, std::span<const std::string_view> variables)
{
    return DspProgram{Compiler(source).compileExpression(variables)};
}

}
