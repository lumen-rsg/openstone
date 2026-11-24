#include "stream/Runtime.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using namespace openstone::stream;

namespace {
void quoted(std::string_view text)
{
    std::cout << '"';
    for (const unsigned char c : text) {
        switch (c) {
        case '"': std::cout << "\\\""; break;
        case '\\': std::cout << "\\\\"; break;
        case '\n': std::cout << "\\n"; break;
        case '\r': std::cout << "\\r"; break;
        case '\t': std::cout << "\\t"; break;
        default:
            if (c < 32) {
                constexpr char hex[] = "0123456789abcdef";
                std::cout << "\\u00" << hex[c >> 4] << hex[c & 15];
            } else {
                std::cout << c;
            }
        }
    }
    std::cout << '"';
}

void backend(const DspProgram& program, DspBackend choice)
{
    if (!dspBackendAvailable(choice)) {
        std::cout << "{\"status\":\"unavailable\"}";
        return;
    }
    for (std::size_t binding = 0; binding < program.enginePointers().size(); ++binding) {
        if (program.requiresEnginePointer(binding)) {
            std::cout << "{\"status\":\"requires-bindings\"}";
            return;
        }
    }
    if (program.outputs().empty()) {
        std::cout << "{\"status\":\"no-output\"}";
        return;
    }
    try {
        Graph graph(choice);
        const auto block = graph.addDspCode(program);
        // Tap every outlet so the block is active and ORC materializes every stage.
        // Constructing a runtime does not execute stage 0 or render any samples.
        const auto runtime = graph.compileMonoDouble4(block.outputs);
        std::cout << "{\"status\":\"pass\"}";
    } catch (const std::exception& e) {
        std::cout << "{\"status\":\"fail\",\"message\":";
        quoted(e.what());
        std::cout << '}';
    }
}
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--words") {
        std::cout << "{\"mnemonics\":[";
        bool comma = false;
        for (const auto& word : dspLanguageWords(DspLanguage::Assembler)) {
            if (word.kind != DspWordKind::Function) continue;
            if (comma) std::cout << ',';
            quoted(word.text);
            comma = true;
        }
        std::cout << "],\"llvm_orc_available\":"
                  << (dspBackendAvailable(DspBackend::LlvmOrc) ? "true" : "false") << "}\n";
        return 0;
    }
    if (argc != 2) {
        std::cerr << "Usage: openstone_assembler_probe SOURCE | --words\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "Cannot read Assembler source\n";
        return 2;
    }
    const std::string source{std::istreambuf_iterator<char>(file), {}};
    try {
        const auto program = compileDspCode(source, DspLanguage::Assembler);
        std::cout << "{\"frontend\":{\"status\":\"pass\"},\"inputs\":" << program.inputs().size()
                  << ",\"outputs\":" << program.outputs().size() << ",\"engine_pointers\":[";
        for (std::size_t i = 0; i < program.enginePointers().size(); ++i) {
            if (i) std::cout << ',';
            quoted(std::string_view{program.enginePointers()[i]});
        }
        std::cout << "],\"interpreter\":";
        backend(program, DspBackend::Interpreter);
        std::cout << ",\"llvm_orc\":";
        backend(program, DspBackend::LlvmOrc);
        std::cout << "}\n";
    } catch (const DspCompileError& e) {
        std::cout << "{\"frontend\":{\"status\":\"fail\",\"line\":" << e.line
                  << ",\"column\":" << e.column << ",\"message\":";
        quoted(e.what());
        std::cout << "}}\n";
    } catch (const std::exception& e) {
        std::cout << "{\"frontend\":{\"status\":\"error\",\"message\":";
        quoted(e.what());
        std::cout << "}}\n";
    }
}
