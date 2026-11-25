#include "Schematic.hpp"
#include "ArrayTypes.hpp"

#include "green/ArrayOperations.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <compare>
#include <iterator>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace openstone {
namespace {

using green::ArithmeticOperation;
using green::UnaryOperation;
using green::Type;
using green::Value;
using enum PrimitiveBehavior;
using array::arrayElement;
using array::arrayForElement;
using array::isArray;
using array::maxArrayPaddingBytes;
using array::withGreenArray;

constexpr int closeHoldInput = 0;
constexpr int closeDoneInput = 1;
constexpr int parentViewInput = 0;
constexpr int mouseInput = 1;
constexpr int borderInput = 2;
constexpr int bitmapPathInput = 0;
constexpr int bitmapDrawAlphaInput = 3;
constexpr int bitmapLoadInput = 1;
constexpr int bitmapClearInput = 2;
constexpr int bitmapArrayIndexInput = 1;
constexpr int bitmapArrayLoadInput = 2;
constexpr int bitmapArrayClearInput = 3;
constexpr float defaultDotRadius = 0.15F;
constexpr int firstPointPlotBoundInput = 4;
constexpr int dotRadiusInput = 8;
constexpr std::array pointPlotBounds{-1.0F, 1.0F, 1.0F, -1.0F};
constexpr int firstBusChannelInput = 1;
constexpr int extractBusInput = 1;
constexpr int arrayIndexInput = 1;
constexpr int arrayGetInput = 2;
constexpr int arrayTransformAmountInput = 1;
constexpr int arraySectionFirstInput = 2;
constexpr int arrayFixedSizeDefaultInput = 2;
constexpr int storeValueInput = 0;
constexpr int storeIndexInput = 1;
constexpr int storeSetInput = 2;
constexpr int storeClearInput = 3;
constexpr int storeAssignInput = 4;
constexpr int storeInsertInput = 5;
constexpr int storeDeleteInput = 6;
constexpr int historySizeInput = 0;
constexpr int historyValueInput = 1;
constexpr int historyAddInput = 2;
constexpr int historyReverseInput = 3;
constexpr int queuePushInput = 1;
constexpr int queuePopInput = 2;
constexpr int queueClearInput = 3;
constexpr int queueReplaceInput = 4;
constexpr int loopIncrementInput = 1;
constexpr int loopRunInput = 4;
constexpr int counterStepInput = 0;
constexpr int counterResetInput = 1;
constexpr int counterMaxInput = 2;
constexpr int counterMinInput = 3;
constexpr int counterTypeInput = 4;
constexpr int counterStartInput = 5;
constexpr int reachedMaxOutput = 1;
constexpr int reachedMinOutput = 2;

bool isQueueOrStack(PrimitiveKind kind)
{
    return kind == PrimitiveKind::ArrayQueue || kind == PrimitiveKind::ArrayStack;
}

int storageArrayOutput(PrimitiveKind kind)
{
    return isQueueOrStack(kind) ? 2 : 0;
}

bool hasGenericArrayInput(PrimitiveKind kind)
{
    return kind == PrimitiveKind::ArrayInterpolatedGet || kind == PrimitiveKind::ArrayLoop ||
           kind == PrimitiveKind::ArraySplitter || kind == PrimitiveKind::ArraySum;
}

Primitive binary(PrimitiveKind kind, std::string_view name, std::string_view symbol,
                 std::string_view result, ArithmeticOperation operation)
{
    return {kind, name, symbol, {{"A", {}}, {"B", {}}}, {{result, {}}}, Binary, operation};
}

Primitive bitwiseBinary(PrimitiveKind kind, std::string_view name)
{
    return {kind, name, name, {{"A", {}}, {"B", {}}}, {{"Result", {}}}, GreenLogic};
}

Primitive unaryMath(PrimitiveKind kind, std::string_view name, std::string_view symbol,
                    std::string_view result, UnaryOperation operation, std::optional<Type> type = std::nullopt)
{
    return {kind, name, symbol, {{"Value", type}}, {{result, type}}, UnaryMath, std::nullopt, operation};
}

Primitive pitchMath(PrimitiveKind kind, std::string_view name, std::string_view symbol,
                    std::string_view input, std::string_view output, math::PitchConversion operation)
{
    return {kind, name, symbol, {{input, {}}}, {{output, {}}}, UnaryMath, std::nullopt, std::nullopt, operation};
}

Primitive comparison(PrimitiveKind kind, std::string_view name, std::string_view symbol, Type result = Type::Boolean)
{
    return {kind, name, symbol, {{"A", {}}, {"B", {}}}, {{"Result", result}}, Compare};
}

Primitive streamMath(PrimitiveKind kind, std::string_view name, std::string_view symbol, bool binary = false)
{
    Primitive spec{kind, name, symbol, {{binary ? "A" : "In", {}}}, {{"Out", {}}}, StreamMath};
    if (binary) spec.inputs.push_back({"B", {}});
    return spec;
}

Primitive streamOscillator(PrimitiveKind kind, std::string_view name, std::string_view symbol, Type type)
{
    return {kind, name, symbol, {{"Freq", type}, {"Phase", type}, {"Sync", Type::StreamBoolean}},
            {{"Out", type}}, StreamRouting};
}

Primitive streamTrig(PrimitiveKind kind, std::string_view name, std::string_view symbol, Type type)
{
    return {kind, name, symbol, {{"In", type}}, {{"Out", type}}, StreamRouting};
}

Primitive streamComparison(PrimitiveKind kind, std::string_view name, std::string_view symbol, Type type)
{
    return {kind, name, symbol, {{"A", type}, {"B", type}}, {{"Out", Type::StreamBoolean}}, StreamRouting};
}

Primitive biquadFilter(PrimitiveKind kind, std::string_view name, Type type)
{
    return {kind, name, "Biquad",
            {{"In", type}, {"a0", type}, {"a1", type}, {"a2", type}, {"b1", type}, {"b2", type}},
            {{"Out", type}}, StreamRouting};
}

Primitive filterCoefficients(PrimitiveKind kind, std::string_view name, Type type)
{
    return {kind, name, "Coefs", {{"Cut", type}, {"Res", type}},
            {{"a0", type}, {"a1", type}, {"a2", type}, {"b1", type}, {"b2", type}}, StreamRouting};
}

Primitive biquadResponse(PrimitiveKind kind, std::string_view name, std::string_view symbol)
{
    Primitive spec{kind, name, symbol,
                   {{"a0", Type::Float}, {"a1", Type::Float}, {"a2", Type::Float},
                    {"b0", Type::Float}, {"b1", Type::Float}, {"b2", Type::Float},
                    {"LowF", Type::Float}, {"HighF", Type::Float}, {"N", Type::Int}, {"SR", Type::Float}},
                   {{"Response", Type::FloatArray}}, ProcessArray};
    if (kind == PrimitiveKind::BiquadFrequencyResponse) spec.inputs.push_back({"LogA", Type::Boolean});
    spec.inputs.push_back({"LogF", Type::Boolean});
    return spec;
}

Primitive mouseParser(PrimitiveKind kind, std::string_view name, std::string_view outputName, Type type)
{
    return {kind, name, "M", {{"Mouse Messages", Type::MouseMessage}}, {{outputName, type}}, ParseMouseMessage};
}

Primitive monoToGraph(PrimitiveKind kind, std::string_view name, std::size_t channels)
{
    constexpr std::array<std::string_view, 8> numbers{"1", "2", "3", "4", "5", "6", "7", "8"};
    Primitive spec{kind, name, "∿", {}, {}, CaptureSignal};
    for (std::size_t channel = 0; channel < channels; ++channel) {
        spec.inputs.push_back({channels == 1 ? "In" : numbers[channel], Type::Mono});
        spec.outputs.push_back({channels == 1 ? "Samples" : numbers[channel], Type::FloatArray});
    }
    spec.inputs.push_back({"NS", Type::Int});
    spec.inputs.push_back({"Get", Type::Trigger});
    return spec;
}

bool isBitwiseBinary(PrimitiveKind kind)
{
    return kind == PrimitiveKind::And || kind == PrimitiveKind::Or || kind == PrimitiveKind::Xor;
}

const std::array catalog{
    Primitive{PrimitiveKind::AudioIn, "Audio In", "∿", {}, {{"Left", Type::Mono}, {"Right", Type::Mono}}, AudioEndpoint},
    Primitive{PrimitiveKind::AudioOut, "Audio Out", "∿", {{"Left", Type::Mono}, {"Right", Type::Mono}}, {}, AudioEndpoint},
    Primitive{PrimitiveKind::DirectSoundIn, "Direct Sound In", "DS In", {},
              {{"Left", Type::Mono}, {"Right", Type::Mono}}, AudioEndpoint},
    Primitive{PrimitiveKind::DirectSoundOut, "Direct Sound Out", "DS Out",
              {{"Left", Type::Mono}, {"Right", Type::Mono}}, {}, AudioEndpoint},
    Primitive{PrimitiveKind::AsioIn, "ASIO In", "ASIO In", {}, {{"1", Type::Mono}, {"2", Type::Mono}}, AudioEndpoint},
    Primitive{PrimitiveKind::AsioOut, "ASIO Out", "ASIO Out", {{"1", Type::Mono}, {"2", Type::Mono}}, {}, AudioEndpoint},
    Primitive{PrimitiveKind::AudioDevices, "Audio Devices", "Devices", {},
              {{"Count", Type::Int}, {"Names", Type::StringArray}}, AudioControl},
    Primitive{PrimitiveKind::AudioInputsOutputs, "Audio Inputs/Outputs", "Channels", {},
              {{"Ins", Type::StringArray}, {"Outs", Type::StringArray}}, AudioControl},
    Primitive{PrimitiveKind::AudioSelect, "Audio Select", "Select", {{"Device", Type::Int}},
              {{"Selected", Type::Int}}, AudioControl},
    Primitive{PrimitiveKind::DirectSoundInDevices, "Direct Sound In Devices", "DS In", {},
              {{"N", Type::Int}, {"Names", Type::StringArray}}, AudioControl},
    Primitive{PrimitiveKind::DirectSoundInSelect, "Direct Sound In Select", "DS Select", {{"Device", Type::Int}},
              {{"Selected", Type::Int}}, AudioControl},
    Primitive{PrimitiveKind::SetSampleRate, "Set Sample Rate", "Set Rate", {{"Rate", Type::Int}, {"Set", Type::Trigger}},
              {{"Error", Type::Trigger}}, AudioControl},
    Primitive{PrimitiveKind::OpenAsioSettings, "Open ASIO Settings", "ASIO", {{"Open", Type::Trigger}}, {}, AudioControl},
    Primitive{PrimitiveKind::ClearAudio, "Clear Audio", "Clear", {{"Clear", Type::Trigger}}, {}, AudioEndpoint},
    Primitive{PrimitiveKind::MonoToFrame, "Mono to Frame", "Frame",
              {{"In", Type::Mono}, {"Samples", Type::Int}}, {{"Samples", Type::FloatArray}}, AudioControl},
    Primitive{PrimitiveKind::DspCode, "DSP Code", "</>", {}, {}, DspCode},
    Primitive{PrimitiveKind::Assembler, "Assembler", "ASM", {}, {}, DspCode},
    Primitive{PrimitiveKind::Ruby, "Ruby", "Ruby", {{"value", Type::Float}}, {{"result", Type::Float}}, RubyCode},
    streamMath(PrimitiveKind::StreamInverse, "Stream Inverse", "−"),
    streamMath(PrimitiveKind::StreamReciprocal, "Stream Reciprocal", "1/x"),
    streamMath(PrimitiveKind::StreamSquareRoot, "Stream Square Root", "√"),
    streamMath(PrimitiveKind::StreamMin, "Stream Min", "Min", true),
    streamMath(PrimitiveKind::StreamMax, "Stream Max", "Max", true),
    streamMath(PrimitiveKind::StreamSign, "Stream Sign", "Sign"),
    Primitive{PrimitiveKind::StreamToDouble, "Stream to Double", "→",
              {{"In", Type::Stream}}, {{"Out", Type::StreamDouble}}, StreamConversion},
    Primitive{PrimitiveKind::DoubleToStream, "Double to Stream", "→",
              {{"In", Type::StreamDouble}}, {{"Out", Type::Stream}}, StreamConversion},
    Primitive{PrimitiveKind::Ramp, "Ramp", "Ramp",
              {{"Freq", Type::Stream}, {"Sync", Type::StreamBoolean}}, {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::Impulse, "Impulse", "Impulse", {}, {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::StreamCounter, "Stream Counter", "Counter",
              {{"Reset", Type::Trigger}}, {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DoubleStreamCounter, "Double Stream Counter", "Counter",
              {{"Reset", Type::Trigger}}, {{"Out", Type::StreamDouble}}, StreamRouting},
    streamTrig(PrimitiveKind::StreamSine, "Stream Sine", "Sin", Type::Stream),
    streamTrig(PrimitiveKind::DoubleStreamSine, "Double Stream Sine", "Sin", Type::StreamDouble),
    streamTrig(PrimitiveKind::StreamCosine, "Stream Cosine", "Cos", Type::Stream),
    streamTrig(PrimitiveKind::DoubleStreamCosine, "Double Stream Cosine", "Cos", Type::StreamDouble),
    streamTrig(PrimitiveKind::StreamTangent, "Stream Tangent", "Tan", Type::Stream),
    streamTrig(PrimitiveKind::DoubleStreamTangent, "Double Stream Tangent", "Tan", Type::StreamDouble),
    streamComparison(PrimitiveKind::StreamEqualTo, "Stream Equal To", "=", Type::Stream),
    streamComparison(PrimitiveKind::DoubleStreamEqualTo, "Double Stream Equal To", "=", Type::StreamDouble),
    streamComparison(PrimitiveKind::StreamLess, "Stream Less", "<", Type::Stream),
    streamComparison(PrimitiveKind::DoubleStreamLess, "Double Stream Less", "<", Type::StreamDouble),
    streamComparison(PrimitiveKind::StreamLessOrEqualTo, "Stream Less or Equal To", "≤", Type::Stream),
    streamComparison(PrimitiveKind::DoubleStreamLessOrEqualTo, "Double Stream Less or Equal To", "≤", Type::StreamDouble),
    streamComparison(PrimitiveKind::StreamGreater, "Stream Greater", ">", Type::Stream),
    streamComparison(PrimitiveKind::DoubleStreamGreater, "Double Stream Greater", ">", Type::StreamDouble),
    streamComparison(PrimitiveKind::StreamGreaterOrEqualTo, "Stream Greater or Equal To", "≥", Type::Stream),
    streamComparison(PrimitiveKind::DoubleStreamGreaterOrEqualTo, "Double Stream Greater or Equal To", "≥", Type::StreamDouble),
    Primitive{PrimitiveKind::StreamSampleAndHold, "Stream Sample and Hold", "S&H",
              {{"In", Type::Stream}, {"Control", Type::Stream}, {"Zero Cross", Type::Boolean}},
              {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DoubleSampleAndHold, "Double Sample and Hold", "S&H",
              {{"In", Type::StreamDouble}, {"Control", Type::StreamDouble}, {"Zero Cross", Type::Boolean}},
              {{"Out", Type::StreamDouble}}, StreamRouting},
    Primitive{PrimitiveKind::StreamPitchToFreq, "Pitch to Freq", "Freq", {{"Pitch", Type::Stream}},
              {{"Frequency", Type::Stream}}, StreamRouting, std::nullopt, std::nullopt,
              math::PitchConversion::PitchToFrequency},
    Primitive{PrimitiveKind::DoublePitchToFreq, "Double Pitch to Freq", "Freq", {{"Pitch", Type::StreamDouble}},
              {{"Frequency", Type::StreamDouble}}, StreamRouting, std::nullopt, std::nullopt,
              math::PitchConversion::PitchToFrequency},
    Primitive{PrimitiveKind::SampleRate, "Sample Rate", "SR", {},
              {{"Rate", Type::Float}}, GeneralProcessing},
    Primitive{PrimitiveKind::StreamSelect, "Stream Select", "Select",
              {{"False", Type::Stream}, {"True", Type::Stream}, {"Select", Type::StreamBoolean}},
              {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DoubleSelect, "Double Select", "Select",
              {{"False", Type::StreamDouble}, {"True", Type::StreamDouble}, {"Select", Type::StreamBoolean}},
              {{"Out", Type::StreamDouble}}, StreamRouting},
    Primitive{PrimitiveKind::FloatArrayRead, "Float Array Read", "FA Read",
              {{"Array", Type::FloatArray}, {"Index", Type::Stream}}, {{"Value", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::SamplePosition, "Sample Position", "SplPos", {}, {{"Position", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DoubleSamplePosition, "Double Sample Position", "SplPos",
              {}, {{"Position", Type::StreamDouble}}, StreamRouting},
    Primitive{PrimitiveKind::PpqPosition, "PPQ Position", "PPQPos", {}, {{"Position", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DoublePpqPosition, "Double PPQ Position", "PPQPos",
              {}, {{"Position", Type::StreamDouble}}, StreamRouting},
    Primitive{PrimitiveKind::BarStartPosition, "Bar Start Position", "BarStart", {}, {{"Position", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DoubleBarStartPosition, "Double Bar Start Position", "BarStart",
              {}, {{"Position", Type::StreamDouble}}, StreamRouting},
    Primitive{PrimitiveKind::StreamTempo, "Tempo", "Tempo", {}, {{"BPM", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DoubleTempo, "Double Tempo", "Tempo", {}, {{"BPM", Type::StreamDouble}}, StreamRouting},
    Primitive{PrimitiveKind::StreamTimeSignature, "Time Signature", "TimeSig",
              {}, {{"Num", Type::Stream}, {"Den", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DoubleTimeSignature, "Double Time Signature", "TimeSig",
              {}, {{"Num", Type::StreamDouble}, {"Den", Type::StreamDouble}}, StreamRouting},
    Primitive{PrimitiveKind::StreamIsPlaying, "Is Playing", "Playing", {}, {{"Playing", Type::StreamBoolean}}, StreamRouting},
    Primitive{PrimitiveKind::EnvelopeControl, "Envelope Control", "Envelope",
              {{"End", Type::PolyBoolean}, {"Hold", Type::Boolean}}, {{"Stage", Type::Poly}}, VoiceSignal},
    Primitive{PrimitiveKind::LoadWaveFile, "Load Wave File", "WaveLoad",
              {{"Path", Type::String}, {"Load", Type::Trigger}, {"Del", Type::Trigger}, {"Channel", Type::Int}},
              {{"Mem", Type::Mem}, {"Rate", Type::Int}, {"Channels", Type::Int}, {"Samples", Type::Int}, {"Status", Type::Int}}, WaveFile},
    Primitive{PrimitiveKind::SaveWaveFile, "Save Wave File", "WaveSave",
              {{"Mem", Type::Mem}, {"Save", Type::Trigger}, {"Path", Type::String}, {"Rate", Type::Int}, {"Format", Type::String}},
              {{"Saved", Type::Trigger}, {"Error", Type::Trigger}}, WaveFile},
    Primitive{PrimitiveKind::WaveFileArray, "Wave File Array", "WaveArray",
              {{"N", Type::Int}, {"Resize", Type::Trigger}, {"Del", Type::Trigger}, {"Index", Type::Int},
               {"Path", Type::String}, {"Load", Type::Trigger}, {"Mono", Type::Boolean}, {"Clear", Type::Trigger}, {"NoSave", Type::Boolean}},
              {{"Waves", Type::MemArray}, {"Rates", Type::IntArray}, {"Sizes", Type::IntArray}, {"Channels", Type::Int}}, WaveFile},
    Primitive{PrimitiveKind::WaveTable, "Wave Table", "Table",
              {{"Wave", Type::FloatArray}, {"DC", Type::Boolean}, {"Harmonics", Type::IntArray}, {"Create", Type::Trigger}},
              {{"Table", Type::Mem}, {"Done", Type::Trigger}}, WaveFile},
    Primitive{PrimitiveKind::WaveRead, "Wave Read", "Read",
              {{"Mem", Type::Mem}, {"Index", Type::Stream}}, {{"Left", Type::Stream}, {"Right", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::WaveReadHop, "Wave Read Hop", "ReadH",
              {{"Mem", Type::Mem}, {"Index", Type::Stream}, {"Hop", Type::Int}}, {{"Left", Type::Stream}, {"Right", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::WaveArrayRead, "Wave Array Read", "Read",
              {{"Waves", Type::MemArray}, {"WIndex", Type::Stream}, {"Index", Type::Stream}},
              {{"Left", Type::Stream}, {"Right", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::WaveTableRead, "Wave Table Read", "Read",
              {{"Table", Type::Mem}, {"Freq", Type::Stream}, {"Phase", Type::Stream}, {"Sync", Type::StreamBoolean}},
              {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::WaveWriteMono, "Wave Write Mono", "Write",
              {{"Mem", Type::Mem}, {"Index", Type::Stream}, {"In", Type::Stream}}, {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::WaveWriteStereo, "Wave Write Stereo", "Write",
              {{"Mem", Type::Mem}, {"Index", Type::Stream}, {"Left", Type::Stream}, {"Right", Type::Stream}},
              {{"Left", Type::Stream}, {"Right", Type::Stream}}, StreamRouting},
    biquadFilter(PrimitiveKind::BiquadFilterCoeff, "Biquad Filter Coeff", Type::Stream),
    biquadFilter(PrimitiveKind::DoubleBiquadFilterCoeff, "Double Biquad Filter Coeff", Type::StreamDouble),
    filterCoefficients(PrimitiveKind::FilterCoefficients, "Filter Coefficients", Type::Stream),
    filterCoefficients(PrimitiveKind::DoubleFilterCoefficients, "Double Filter Coefficients", Type::StreamDouble),
    biquadResponse(PrimitiveKind::BiquadFrequencyResponse, "Biquad Frequency Response", "BiFreq"),
    biquadResponse(PrimitiveKind::BiquadPhaseResponse, "Biquad Phase Response", "BiPhase"),
    streamOscillator(PrimitiveKind::Sawtooth, "Sawtooth", "Sawtooth", Type::Stream),
    streamOscillator(PrimitiveKind::DoubleSawtooth, "Double Sawtooth", "Sawtooth", Type::StreamDouble),
    streamOscillator(PrimitiveKind::Sine, "Sine", "Sine", Type::Stream),
    streamOscillator(PrimitiveKind::DoubleSine, "Double Sine", "Sine", Type::StreamDouble),
    streamOscillator(PrimitiveKind::Triangle, "Triangle", "Triangle", Type::Stream),
    streamOscillator(PrimitiveKind::DoubleTriangle, "Double Triangle", "Triangle", Type::StreamDouble),
    Primitive{PrimitiveKind::Glide, "Glide", "Glide",
              {{"In", Type::Stream}, {"Glide", Type::Stream}}, {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DeZipper, "De-Zipper", "DeZip",
              {{"In", Type::Stream}, {"Time", Type::Float}, {"Reset", Type::Trigger}}, {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::FloatDeZipper, "Float De-Zipper", "DeZip",
              {{"In", Type::Float}, {"Time", Type::Float}, {"Reset", Type::Trigger}}, {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::Delay, "Delay", "Delay",
              {{"In", Type::Mono}, {"NS", Type::Int}}, {{"Out", Type::Mono}}, StreamRouting},
    Primitive{PrimitiveKind::DoubleDelay, "Double Delay", "Delay",
              {{"In", Type::MonoDouble}, {"NS", Type::Int}}, {{"Out", Type::MonoDouble}}, StreamRouting},
    Primitive{PrimitiveKind::DelayOneSample, "Delay by One Sample", "Delay1",
              {{"In", Type::Stream}}, {{"Out", Type::Stream}}, StreamRouting},
    Primitive{PrimitiveKind::DelayDoubleOneSample, "Delay Double by One Sample", "Delay1",
              {{"In", Type::StreamDouble}}, {{"Out", Type::StreamDouble}}, StreamRouting},
    Primitive{PrimitiveKind::Pack, "Pack", "Pack",
              {{"1", Type::Mono}, {"2", Type::Mono}, {"3", Type::Mono}, {"4", Type::Mono}},
              {{"Packed", Type::Mono4}}, StreamRouting},
    Primitive{PrimitiveKind::PackDouble, "Pack Double", "Pack",
              {{"1", Type::MonoDouble}, {"2", Type::MonoDouble}, {"3", Type::MonoDouble}, {"4", Type::MonoDouble}},
              {{"Packed", Type::Mono4Double}}, StreamRouting},
    Primitive{PrimitiveKind::Unpack, "Unpack", "Unpack", {{"Packed", Type::Mono4}},
              {{"1", Type::Mono}, {"2", Type::Mono}, {"3", Type::Mono}, {"4", Type::Mono}}, StreamRouting},
    Primitive{PrimitiveKind::UnpackDouble, "Unpack Double", "Unpack", {{"Packed", Type::Mono4Double}},
              {{"1", Type::MonoDouble}, {"2", Type::MonoDouble}, {"3", Type::MonoDouble}, {"4", Type::MonoDouble}}, StreamRouting},
    Primitive{PrimitiveKind::MonoToPoly, "Mono to Poly", "→", {{"Mono", Type::Mono}, {"Voice Index", Type::Int}},
              {{"Poly", Type::Poly}}, PolyBridge},
    Primitive{PrimitiveKind::PolyToMono, "Poly to Mono", "→", {{"Poly", Type::Poly}, {"Voice Index", Type::Int}},
              {{"Mono", Type::Mono}}, PolyBridge},
    Primitive{PrimitiveKind::PolyToMonoMin, "Min. Poly to Mono", "Min",
              {{"Poly", Type::Poly}}, {{"Mono", Type::Mono}}, PolyBridge},
    Primitive{PrimitiveKind::PolyToMonoMax, "Max. Poly to Mono", "Max",
              {{"Poly", Type::Poly}}, {{"Mono", Type::Mono}}, PolyBridge},
    Primitive{PrimitiveKind::PolyToMonoHop, "Poly to Mono Hop", "Hop",
              {{"Poly", Type::Poly}, {"Hop", Type::Int}}, {{"Mono", Type::Mono}}, PolyBridge},
    Primitive{PrimitiveKind::PolyToMonoSelective, "Poly to Mono Selective", "Select",
              {{"Poly", Type::Poly}, {"Group", Type::Mono}}, {{"Mono", Type::Mono}}, PolyBridge},
    Primitive{PrimitiveKind::SignalAnalyser, "Signal Analyser", "∿",
              {{"Poly", Type::Poly}, {"NS", Type::Int}, {"Recalc", Type::Trigger}},
              {{"Samples", Type::FloatArray}, {"Code", Type::String}, {"Time (ns)", Type::FloatArray}}, CaptureSignal},
    monoToGraph(PrimitiveKind::MonoToGraph, "Mono to Graph", 1),
    monoToGraph(PrimitiveKind::MonoToGraph2, "Mono to Graph (2)", 2),
    monoToGraph(PrimitiveKind::MonoToGraph4, "Mono to Graph (4)", 4),
    monoToGraph(PrimitiveKind::MonoToGraph8, "Mono to Graph (8)", 8),
    Primitive{PrimitiveKind::MonoToFloat, "Mono to Float", "∿",
              {{"Mono", Type::Mono}, {"Get", Type::Trigger}}, {{"Sample", Type::Float}}, SampleMono},
    Primitive{PrimitiveKind::StreamToGreen, "Stream to Green", "ToGreen",
              {{"Stream", {}}, {"Get", Type::Trigger}}, {{"Sample", {}}}, SampleMono},
    Primitive{PrimitiveKind::Int, "Int", "I", {{"Value", Type::Int}}, {{"Value", Type::Int}}, ValueBox},
    Primitive{PrimitiveKind::Float, "Float", "F", {{"Value", Type::Float}}, {{"Value", Type::Float}}, ValueBox},
    Primitive{PrimitiveKind::String, "String", "S", {{"Value", Type::String}}, {{"Value", Type::String}}, ValueBox},
    Primitive{PrimitiveKind::MultilineString, "Multiline String", "S", {{"Value", Type::String}}, {{"Value", Type::String}}, ValueBox},
    Primitive{PrimitiveKind::Boolean, "Boolean", "B", {{"Value", Type::Boolean}}, {{"Value", Type::Boolean}}, ValueBox},
    binary(PrimitiveKind::Add, "Add", "+", "Sum", ArithmeticOperation::Add),
    binary(PrimitiveKind::Subtract, "Subtract", "−", "Difference", ArithmeticOperation::Subtract),
    binary(PrimitiveKind::Multiply, "Multiply", "×", "Product", ArithmeticOperation::Multiply),
    binary(PrimitiveKind::Divide, "Divide", "÷", "Quotient", ArithmeticOperation::Divide),
    binary(PrimitiveKind::Modulus, "Modulus", "%", "Remainder", ArithmeticOperation::Modulus),
    binary(PrimitiveKind::Min, "Min", "Min", "Minimum", ArithmeticOperation::Min),
    binary(PrimitiveKind::Max, "Max", "Max", "Maximum", ArithmeticOperation::Max),
    Primitive{PrimitiveKind::Power, "Power", "xʸ", {{"Base", {}}, {"Exponent", {}}},
              {{"Result", {}}}, Binary, ArithmeticOperation::Power},
    Primitive{PrimitiveKind::Logarithm, "Logarithm", "Log", {{"Value", {}}, {"Base", {}}},
              {{"Result", {}}}, Binary, ArithmeticOperation::Logarithm},
    unaryMath(PrimitiveKind::Ln, "Ln", "Ln", "Result", UnaryOperation::Ln),
    unaryMath(PrimitiveKind::Log2, "Log2", "Log₂", "Result", UnaryOperation::Log2),
    unaryMath(PrimitiveKind::Log10, "Log10", "Log₁₀", "Result", UnaryOperation::Log10),
    unaryMath(PrimitiveKind::Sin, "Sin", "Sin", "Result", UnaryOperation::Sin, Type::Float),
    unaryMath(PrimitiveKind::Cos, "Cos", "Cos", "Result", UnaryOperation::Cos, Type::Float),
    unaryMath(PrimitiveKind::Tan, "Tan", "Tan", "Result", UnaryOperation::Tan, Type::Float),
    unaryMath(PrimitiveKind::SinInverse, "Sin Inverse", "Asin", "Radians", UnaryOperation::Asin, Type::Float),
    unaryMath(PrimitiveKind::CosInverse, "Cos Inverse", "Acos", "Radians", UnaryOperation::Acos, Type::Float),
    unaryMath(PrimitiveKind::TanInverse, "Tan Inverse", "Atan", "Radians", UnaryOperation::Atan, Type::Float),
    Primitive{PrimitiveKind::Atan2, "Atan2", "∠", {{"Y", Type::Float}, {"X", Type::Float}},
              {{"Radians", Type::Float}}, Binary, ArithmeticOperation::Atan2},
    binary(PrimitiveKind::Hypotenuse, "Hypotenuse", "Hypot", "Length", ArithmeticOperation::Hypotenuse),
    unaryMath(PrimitiveKind::Reciprocal, "Reciprocal", "1/x", "Reciprocal", UnaryOperation::Reciprocal),
    unaryMath(PrimitiveKind::Sign, "Sign", "Sign", "Sign", UnaryOperation::Sign),
    unaryMath(PrimitiveKind::Inverse, "Inverse", "−", "Result", UnaryOperation::Inverse),
    unaryMath(PrimitiveKind::FromDecibel, "From Decibel", "↤dB", "Linear", UnaryOperation::FromDecibel),
    unaryMath(PrimitiveKind::ToDecibel, "To Decibel", "dB", "Decibel", UnaryOperation::ToDecibel),
    pitchMath(PrimitiveKind::FrequencyToHz, "Frequency to Hz", "Hz", "Frequency", "Hz", math::PitchConversion::FrequencyToHz),
    pitchMath(PrimitiveKind::FrequencyToPitch, "Frequency to Pitch", "Pitch", "Frequency", "Pitch", math::PitchConversion::FrequencyToPitch),
    pitchMath(PrimitiveKind::HzToFrequency, "Hz to Frequency", "Freq", "Hz", "Frequency", math::PitchConversion::HzToFrequency),
    pitchMath(PrimitiveKind::HzToPitch, "Hz to Pitch", "Pitch", "Hz", "Pitch", math::PitchConversion::HzToPitch),
    pitchMath(PrimitiveKind::PitchToFrequency, "Pitch to Frequency", "Freq", "Pitch", "Frequency", math::PitchConversion::PitchToFrequency),
    pitchMath(PrimitiveKind::PitchToHz, "Pitch to Hz", "Hz", "Pitch", "Hz", math::PitchConversion::PitchToHz),
    Primitive{PrimitiveKind::NoteNameToInt, "Note name to Int", "Note", {{"Note Name", {}}}, {{"Note", {}}}, NoteConversion},
    Primitive{PrimitiveKind::IntToNoteName, "Int to Note Name", "Note", {{"Note", {}}}, {{"Note Name", {}}}, NoteConversion},
    Primitive{PrimitiveKind::Evaluator, "Evaluator", "Eval",
              {{"Function", Type::String}, {"x", Type::Float}, {"y", Type::Float}, {"z", Type::Float},
               {"a", Type::Float}, {"b", Type::Float}, {"c", Type::Float}},
              {{"Result", Type::Float}}, GeneralProcessing},
    Primitive{PrimitiveKind::FloatSlide, "Float Slide", "Slide",
              {{"Target", Type::Float}, {"Rate", Type::Int}, {"Step", Type::Float}, {"Set", Type::Float}},
              {{"Value", Type::Float}}, GeneralProcessing},
    Primitive{PrimitiveKind::RandomNumber, "Random Number", "Rand",
              {{"Get", Type::Trigger}, {"Seed", Type::Int}}, {{"Value", Type::Int}}, GeneralProcessing},
    Primitive{PrimitiveKind::TeachingGoal, "Teaching Goal", "Goal", {{"Achieved", Type::Trigger}}, {}, GeneralProcessing},
    Primitive{PrimitiveKind::GoalProgress, "Goal Progress", "Progress", {{"Reset", Type::Trigger}},
              {{"Goals", Type::Int}, {"Complete", Type::Int}, {"Results", Type::String}}, GeneralProcessing},
    unaryMath(PrimitiveKind::Sinh, "Sinh", "Sinh", "Result", UnaryOperation::Sinh, Type::Float),
    unaryMath(PrimitiveKind::Cosh, "Cosh", "Cosh", "Result", UnaryOperation::Cosh, Type::Float),
    unaryMath(PrimitiveKind::Tanh, "Tanh", "Tanh", "Result", UnaryOperation::Tanh, Type::Float),
    unaryMath(PrimitiveKind::Abs, "Abs", "Abs", "Absolute value", UnaryOperation::Absolute),
    unaryMath(PrimitiveKind::Round, "Round", "Round", "Rounded value", UnaryOperation::Round),
    unaryMath(PrimitiveKind::Floor, "Floor", "Floor", "Rounded value", UnaryOperation::Floor),
    unaryMath(PrimitiveKind::Ceiling, "Ceiling", "Ceil", "Rounded value", UnaryOperation::Ceiling),
    unaryMath(PrimitiveKind::Truncate, "Truncate", "Trunc", "Truncated value", UnaryOperation::Truncate),
    comparison(PrimitiveKind::Equal, "Equal", "="),
    comparison(PrimitiveKind::LessThan, "Less Than", "<"),
    comparison(PrimitiveKind::LessThanOrEqual, "Less Than or Equal", "≤"),
    comparison(PrimitiveKind::GreaterThan, "Greater Than", ">"),
    comparison(PrimitiveKind::GreaterThanOrEqual, "Greater Than or Equal", "≥"),
    comparison(PrimitiveKind::EqualTrigger, "Equal (Trigger)", "=", Type::Trigger),
    comparison(PrimitiveKind::LessThanTrigger, "Less Than (Trigger)", "<", Type::Trigger),
    comparison(PrimitiveKind::LessThanOrEqualTrigger, "Less Than or Equal (Trigger)", "≤", Type::Trigger),
    comparison(PrimitiveKind::GreaterThanTrigger, "Greater Than (Trigger)", ">", Type::Trigger),
    comparison(PrimitiveKind::GreaterThanOrEqualTrigger, "Greater Than or Equal (Trigger)", "≥", Type::Trigger),
    bitwiseBinary(PrimitiveKind::And, "And"),
    bitwiseBinary(PrimitiveKind::Or, "Or"),
    bitwiseBinary(PrimitiveKind::Xor, "XOr"),
    Primitive{PrimitiveKind::Not, "Not", "Not",
              {{"Value", Type::Boolean}}, {{"Result", Type::Boolean}}, GreenLogic},
    Primitive{PrimitiveKind::IntNot, "Int Not", "Not",
              {{"Value", Type::Int}}, {{"Result", Type::Int}}, GreenLogic},
    Primitive{PrimitiveKind::ShiftLeft, "Shift Left", "<<",
              {{"Value", Type::Int}, {"Bits", Type::Int}}, {{"Result", Type::Int}}, GreenLogic},
    Primitive{PrimitiveKind::ShiftRight, "Shift Right", ">>",
              {{"Value", Type::Int}, {"Bits", Type::Int}}, {{"Result", Type::Int}}, GreenLogic},
    Primitive{PrimitiveKind::IfThenElse, "If Then Else", "If",
              {{"Type", Type::String}, {"If", Type::String}, {"Op", Type::String},
               {"Conditions", Type::StringArray}, {"Then", Type::StringArray}, {"Else", Type::String}},
              {{"Out", Type::String}}, GreenLogic},
    Primitive{PrimitiveKind::Select, "Select", "Select",
              {{"False", {}}, {"True", {}}, {"Condition", Type::Boolean}}, {{"Value", {}}}, GreenLogic},
    Primitive{PrimitiveKind::LastSwitch, "Last Switch", "Last",
              {{"A", {}}, {"B", {}}}, {{"Value", {}}}, GreenLogic},
    Primitive{PrimitiveKind::Selector, "Selector", "Selector",
              {{"Index", Type::Int}, {"Add input", {}}}, {{"Value", {}}}, GreenLogic},
    Primitive{PrimitiveKind::Multiplexer, "Multiplexer", "Multiplex",
              {{"Index", Type::Int}, {"Value", {}}}, {{"Add output", {}}}, GreenLogic},
    Primitive{PrimitiveKind::Matrix, "Matrix", "Matrix",
              {{"Connections", Type::String}, {"Add input", {}}}, {{"Add output", {}}}, MatrixRouting},
    Primitive{PrimitiveKind::IntTransition, "Int Transition", "Transition",
              {{"Value", Type::Int}, {"From", Type::Int}, {"To", Type::Int}},
              {{"Transition", Type::Trigger}}, GreenLogic},
    Primitive{PrimitiveKind::BoolToFalseTrigger, "Bool to False Trigger", "Fall",
              {{"Value", Type::Boolean}}, {{"False", Type::Trigger}}, GreenLogic},
    Primitive{PrimitiveKind::BoolToTrueTrigger, "Bool to True Trigger", "Rise",
              {{"Value", Type::Boolean}}, {{"True", Type::Trigger}}, GreenLogic},
    Primitive{PrimitiveKind::Changed, "Changed", "Changed",
              {{"Value", {}}}, {{"Value", {}}}, GreenLogic},
    Primitive{PrimitiveKind::SampleAndHold, "Sample and Hold", "Hold",
              {{"Value", {}}, {"Sample", Type::Trigger}}, {{"Held", {}}}, GreenLogic},
    Primitive{PrimitiveKind::ArrayInterpolatedGet, "Array Interpolated Get", "Interpolate",
              {{"Array", {}}, {"Interp", Type::Int}, {"Index", Type::Float}, {"Get", Type::Trigger}},
              {{"Value", {}}, {"Done", Type::Trigger}}, ProcessArray},
    Primitive{PrimitiveKind::ArrayLoop, "Array Loop", "Loop",
              {{"Array", {}}, {"Rev", Type::Boolean}, {"NoLim", Type::Boolean}, {"Run", Type::Trigger}},
              {{"Value", {}}, {"Index", Type::Int}, {"Step", Type::Trigger}, {"Finished", Type::Trigger}}, ProcessArray},
    Primitive{PrimitiveKind::ArraySplitter, "Array Splitter", "Split", {{"Array", {}}},
              std::vector<Port>(50, Port{"", {}}), ProcessArray},
    Primitive{PrimitiveKind::FloatArrayDraw, "Float Array Draw", "Draw",
              {{"N", Type::Int}, {"Index", Type::Int}, {"Value", Type::Float}, {"Drag", Type::Boolean},
               {"Set", Type::Trigger}, {"Assign", Type::FloatArray}}, {{"Array", Type::FloatArray}}, ProcessArray},
    Primitive{PrimitiveKind::FloatArrayResample, "Float Array Resample", "Resample",
              {{"Array", Type::FloatArray}, {"N", Type::Int}, {"Interp", Type::Int}, {"Smth", Type::Boolean}},
              {{"Array", Type::FloatArray}}, ProcessArray},
    Primitive{PrimitiveKind::FloatArrayToPointArray, "Float Array To Point Array", "PArray",
              {{"X", Type::FloatArray}, {"Y", Type::FloatArray}}, {{"Point Array", Type::PointArray}}, ProcessArray},
    Primitive{PrimitiveKind::PointArrayToFloatArray, "Point Array To Float Array", "PArray",
              {{"Point Array", Type::PointArray}}, {{"X", Type::FloatArray}, {"Y", Type::FloatArray}}, ProcessArray},
    Primitive{PrimitiveKind::MinFloatArray, "Min Float Array", "Min",
              {{"Array", Type::FloatArray}}, {{"Val", Type::Float}, {"Idx", Type::Int}}, ProcessArray},
    Primitive{PrimitiveKind::MaxFloatArray, "Max Float Array", "Max",
              {{"Array", Type::FloatArray}}, {{"Val", Type::Float}, {"Idx", Type::Int}}, ProcessArray},
    Primitive{PrimitiveKind::ArrayNorm, "Norm", "Norm",
              {{"Array", Type::FloatArray}}, {{"Array", Type::FloatArray}}, ProcessArray},
    Primitive{PrimitiveKind::ArrayFft, "FFT", "FFT",
              {{"Real", Type::FloatArray}, {"Img", Type::FloatArray}, {"Calc", Type::Trigger}},
              {{"Real", Type::FloatArray}, {"Img", Type::FloatArray}}, ProcessArray},
    Primitive{PrimitiveKind::ArrayInverseFft, "iFFT", "iFFT",
              {{"Real", Type::FloatArray}, {"Img", Type::FloatArray}, {"Calc", Type::Trigger}},
              {{"Real", Type::FloatArray}, {"Img", Type::FloatArray}}, ProcessArray},
    Primitive{PrimitiveKind::MagnitudePhaseToRealImaginary, "Magnitude/Phase to Real/Img", "RealImg",
              {{"Mag", Type::FloatArray}, {"Phase", Type::FloatArray}, {"Calc", Type::Trigger}},
              {{"Real", Type::FloatArray}, {"Img", Type::FloatArray}}, ProcessArray},
    Primitive{PrimitiveKind::RealImaginaryToMagnitudePhase, "Real/Img to Magnitude/Phase", "MagPhase",
              {{"Real", Type::FloatArray}, {"Img", Type::FloatArray}, {"Calc", Type::Trigger}},
              {{"Mag", Type::FloatArray}, {"Phase", Type::FloatArray}}, ProcessArray},
    Primitive{PrimitiveKind::SortArray, "Sort Array", "Sort",
              {{"Array", {}}}, {{"Array", {}}}, ProcessArray},
    Primitive{PrimitiveKind::ArraySum, "Sum", "Sum",
              {{"Array", {}}}, {{"Sum", {}}}, ProcessArray},
    Primitive{PrimitiveKind::ArrayLength, "Length", "Length",
              {{"Value", {}}}, {{"Length", Type::Int}}, ProcessArray},
    Primitive{PrimitiveKind::Array, "Array", "Array",
              {{"Value", {}}, {"Index", Type::Int}, {"Set", Type::Trigger}, {"Clear", Type::Trigger},
               {"Assign", {}}, {"Insert", Type::Trigger}, {"Delete", Type::Trigger}},
              {{"Array", {}}, {"Changed", Type::Trigger}, {"Size", Type::Int}}, StoreArray},
    Primitive{PrimitiveKind::ArrayHistory, "Array History", "History",
              {{"Size", Type::Int}, {"Val", {}}, {"Add", Type::Trigger}, {"Rev", Type::Boolean}},
              {{"History", {}}}, StoreArray},
    Primitive{PrimitiveKind::ArrayQueue, "Array Queue", "Queue",
              {{"In", {}}, {"Push", Type::Trigger}, {"Pop", Type::Trigger}, {"Clear", Type::Trigger}, {"Replace", {}}},
              {{"Top", {}}, {"N", Type::Int}, {"All", {}}}, StoreArray},
    Primitive{PrimitiveKind::ArrayStack, "Array Stack", "Stack",
              {{"In", {}}, {"Push", Type::Trigger}, {"Pop", Type::Trigger}, {"Clear", Type::Trigger}, {"Replace", {}}},
              {{"Top", {}}, {"N", Type::Int}, {"All", {}}}, StoreArray},
    Primitive{PrimitiveKind::ArrayBuilder, "Array Builder", "Array", {{"Add value", {}}}, {{"Array", {}}}, BuildArray},
    Primitive{PrimitiveKind::ArrayGet, "Array Get", "Get", {{"Array", {}}, {"Index", Type::Int}, {"Get", Type::Trigger}},
              {{"Value", {}}, {"Done", Type::Trigger}}, GetArrayElement},
    Primitive{PrimitiveKind::AppendArray, "Append Array", "Append", {{"A", {}}, {"B", {}}}, {{"Array", {}}}, AppendArrays},
    Primitive{PrimitiveKind::ArraySection, "Array Section", "Section",
              {{"Array", {}}, {"N", Type::Int}, {"First", Type::Int}, {"DoIt", Type::Trigger}},
              {{"Array", {}}}, TransformArray},
    Primitive{PrimitiveKind::RotateArray, "Rotate Array", "Rotate",
              {{"Array", {}}, {"Rotate", Type::Int}, {"DoIt", Type::Trigger}}, {{"Array", {}}}, TransformArray},
    Primitive{PrimitiveKind::ArrayFixedSize, "Array Fixed Size", "FixedSize",
              {{"Arr", {}}, {"Size", Type::Int}, {"Default", {}}, {"Update", Type::Trigger}},
              {{"Array", {}}}, TransformArray},
    Primitive{PrimitiveKind::StringExtract, "String Extract", "Extract",
              {{"String", Type::String}, {"Pos", Type::Int}, {"Count", Type::Int}}, {{"String", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::StringSplit, "String Split", "Split",
              {{"String", Type::String}, {"Pos", Type::Int}},
              {{"Left", Type::String}, {"Right", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::StringFind, "String Find", "Find",
              {{"String", Type::String}, {"Find", Type::String}, {"Case", Type::Boolean}, {"Rev", Type::Boolean}},
              {{"Index", Type::Int}}, ProcessString},
    Primitive{PrimitiveKind::StringReplace, "String Replace", "Replace",
              {{"String", Type::String}, {"Needle", Type::String}, {"Replace", Type::String}, {"Once", Type::Boolean}},
              {{"String", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::StringOverride, "String Override", "Override",
              {{"String", Type::String}, {"Pos", Type::Int}, {"Str", Type::String}}, {{"String", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::StringArrayFind, "String Array Find", "Find",
              {{"Array", Type::StringArray}, {"Find", Type::String}, {"Case", Type::Boolean}, {"Rev", Type::Boolean}},
              {{"Index", Type::Int}}, ProcessString},
    Primitive{PrimitiveKind::StringArraySplit, "String Array Split", "Split",
              {{"Array", Type::StringArray}, {"Pos", Type::Int}},
              {{"Left", Type::StringArray}, {"Right", Type::StringArray}}, ProcessString},
    Primitive{PrimitiveKind::StringArrayToString, "String Array to String", "Join",
              {{"Array", Type::StringArray}, {"Sep", Type::String}}, {{"String", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::StringToStringArray, "String to String Array", "Split",
              {{"String", Type::String}, {"Delimiter", Type::String}, {"Width", Type::Int}},
              {{"Array", Type::StringArray}}, ProcessString},
    Primitive{PrimitiveKind::SpecialCharacters, "Special Characters", "Char",
              {{"Index", Type::Int}}, {{"Character", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::AsciiToString, "ASCII to String", "Chr",
              {{"ASCII", Type::Int}}, {{"String", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::StringToAscii, "String to ASCII", "Asc",
              {{"String", Type::String}}, {{"ASCII", Type::Int}}, ProcessString},
    Primitive{PrimitiveKind::StringToHex, "String to Hex", "Hex",
              {{"String", Type::String}}, {{"Hex", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::HexToString, "Hex to String", "String",
              {{"Hex", Type::String}}, {{"String", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::BinaryToHex, "Binary to Hex", "Hex",
              {{"Binary", Type::String}}, {{"Hex", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::HexToBinary, "Hex to Binary", "Binary",
              {{"Hex", Type::String}}, {{"Binary", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::HexToInt, "Hex to Int", "Int",
              {{"Hex", Type::String}}, {{"Int", Type::Int}}, ProcessString},
    Primitive{PrimitiveKind::IntToHex, "Int to Hex", "Hex",
              {{"Int", Type::Int}}, {{"Hex", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::StringCharset, "String Charset", "Charset",
              {{"String", Type::String}, {"From", Type::String}, {"To", Type::String}},
              {{"String", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::FormatString, "Format String", "Format",
              {{"Value", Type::String}, {"Format", Type::String}}, {{"String", Type::String}}, ProcessString},
    Primitive{PrimitiveKind::ComPort, "COM Port", "COM-Port",
              {{"Port", Type::Int}, {"Baud", Type::Int}, {"StopBit", Type::Int}, {"Parity", Type::Int}, {"ByteSz", Type::Int},
               {"Open", Type::Trigger}, {"Close", Type::Trigger}, {"Data", Type::String}, {"RxTerminator", Type::Int},
               {"Send", Type::Trigger}, {"Hex", Type::Boolean}, {"Log", Type::Boolean}, {"ClearLog", Type::Trigger},
               {"NoNull", Type::Boolean}, {"Refresh", Type::Trigger}, {"ClearSends", Type::Trigger}},
              {{"Open", Type::Boolean}, {"In", Type::String}, {"Out", Type::Trigger}, {"Log", Type::String},
               {"Ports", Type::IntArray}, {"Opening", Type::Boolean}, {"Buffer", Type::Int}}, SerialDevice},
    Primitive{PrimitiveKind::ComPorts, "COM Ports", "COM-Ports", {{"Refresh", Type::Trigger}},
              {{"Ports", Type::IntArray}}, SerialDevice},
    Primitive{PrimitiveKind::ColorDialog, "Color Dialog", "ColorDlg",
              {{"Open", Type::Trigger}, {"Alpha", Type::Int}, {"Set", Type::Color}},
              {{"Color", Type::Color}, {"OK", Type::Trigger}, {"Cancel", Type::Trigger}}, FileUtility},
    Primitive{PrimitiveKind::WebUrl, "Web URL", "WebURL",
              {{"URL", Type::String}, {"DoIt", Type::Trigger}}, {}, SystemAction},
    Primitive{PrimitiveKind::ShellExecute, "Shell Execute", "Exec",
              {{"Path", Type::String}, {"Args", Type::String}, {"Run", Type::Trigger}, {"BG", Type::Boolean}},
              {{"", Type::Trigger}}, SystemAction},
    Primitive{PrimitiveKind::MidiSequencerTrack, "MIDI Sequencer Track", "MIDI Track",
              {{"In", Type::MIDI}}, {{"Out", Type::MIDI}}, SequencerTrack},
    Primitive{PrimitiveKind::StreamSequencerTrack, "Stream Sequencer Track", "Stream Track",
              {{"In", Type::Mono4}}, {{"Out", Type::Mono4}}, SequencerTrack},
    Primitive{PrimitiveKind::AudioStreamIn, "Audio Stream In", "Audio Stream",
              {{"Source", Type::String}, {"Start", Type::Trigger}, {"Pause/Play", Type::Trigger},
               {"Stop", Type::Trigger}, {"Buffer", Type::Float}},
              {{"Left", Type::Mono}, {"Right", Type::Mono}, {"State", Type::String}, {"Pos", Type::Float}}, MediaSource},
    Primitive{PrimitiveKind::VideoStreamIn, "Video Stream In", "Video Stream",
              {{"Source", Type::String}, {"Width", Type::Int}, {"Height", Type::Int}, {"Start", Type::Trigger},
               {"Step", Type::Trigger}, {"Pause/Play", Type::Trigger}, {"Stop", Type::Trigger},
               {"Manual", Type::Boolean}, {"Grab", Type::Trigger}, {"Mute", Type::Boolean}},
              {{"Frame", Type::Bitmap}, {"State", Type::Int}, {"Pos", Type::Float}}, MediaSource},
    Primitive{PrimitiveKind::NetworkClient, "Network Client", "Client",
              {{"Address", Type::String}, {"Port", Type::Int}, {"Protocol", Type::String},
               {"Open", Type::Trigger}, {"Close", Type::Trigger}, {"Data", Type::String}, {"Hex", Type::Boolean},
               {"Send", Type::Trigger}, {"Log", Type::Boolean}, {"Clear Log", Type::Trigger}},
              {{"Status", Type::String}, {"Data", Type::String}, {"Sent", Type::Trigger}, {"Log", Type::String}},
              NetworkTransfer},
    Primitive{PrimitiveKind::NetworkServer, "Network Server", "Server",
              {{"Port", Type::Int}, {"Protocol", Type::String}, {"Start", Type::Trigger}, {"Stop", Type::Trigger},
               {"Data", Type::String}, {"Hex", Type::Boolean}, {"Send", Type::Trigger}, {"Log", Type::Boolean},
               {"Clear Log", Type::Trigger}},
              {{"Status", Type::String}, {"IP", Type::String}, {"Data", Type::String}, {"Sent", Type::Trigger},
               {"Log", Type::String}}, NetworkTransfer},
    Primitive{PrimitiveKind::SfzParser, "SFZ Parser", "SFZ",
              {{"Path", Type::String}, {"Load", Type::Trigger}}, {{"Result", Type::Int}, {"Data", Type::Bus}}, ParseSfz},
    Primitive{PrimitiveKind::HttpTextRequest, "HTTP Request", "HTTP Request",
              {{"URL", Type::String}, {"Names", Type::StringArray}, {"Values", Type::StringArray}, {"Send", Type::Trigger}},
              {{"Response", Type::String}, {"Ok", Type::Boolean}, {"Error", Type::String}}, HttpTransfer},
    Primitive{PrimitiveKind::HttpImageRequest, "HTTP Image Request", "HTTP Image",
              {{"URL", Type::String}, {"Names", Type::StringArray}, {"Values", Type::StringArray}, {"Send", Type::Trigger}},
              {{"Response", Type::Bitmap}, {"Ok", Type::Boolean}, {"Error", Type::String}}, HttpTransfer},
    Primitive{PrimitiveKind::HttpFileRequest, "HTTP File Request", "HTTP File",
              {{"URL", Type::String}, {"Names", Type::StringArray}, {"Values", Type::StringArray},
               {"Dest", Type::String}, {"Override", Type::Boolean}, {"Send", Type::Trigger}},
              {{"Ok", Type::Boolean}, {"Error", Type::String}}, HttpTransfer},
    Primitive{PrimitiveKind::HardDiskSerial, "Hard Disk Serial", "HDSerial",
              {{"Get", Type::Trigger}}, {{"", Type::String}}, SystemAction},
    Primitive{PrimitiveKind::MacAddresses, "MAC Addresses", "MAC",
              {{"Get", Type::Trigger}}, {{"", Type::String}}, SystemAction},
    Primitive{PrimitiveKind::FileDialog, "File Dialog", "FileDlg",
              {{"Open", Type::Trigger}, {"Filter", Type::String}, {"Type", Type::Int}, {"Title", Type::String},
               {"Folder", Type::String}, {"Name", Type::String}, {"Ext", Type::String}},
              {{"Path", Type::String}, {"OK", Type::Trigger}, {"Cancel", Type::Trigger}}, FileUtility},
    Primitive{PrimitiveKind::FolderDialog, "Folder Dialog", "FolderDlg",
              {{"Open", Type::Trigger}, {"Title", Type::String}, {"Folder", Type::String}},
              {{"Path", Type::String}, {"OK", Type::Trigger}, {"Cancel", Type::Trigger}}, FileUtility},
    Primitive{PrimitiveKind::MessageBox, "Message Box", "MsgBox",
              {{"Open", Type::Trigger}, {"Msg", Type::String}, {"Title", Type::String},
               {"Icon", Type::Int}, {"Type", Type::Int}}, {{"Result", Type::Boolean}}, FileUtility},
    Primitive{PrimitiveKind::FindFiles, "Find Files", "Find",
              {{"Filter", Type::String}, {"Find", Type::Trigger}},
              {{"Files", Type::StringArray}, {"N", Type::Int}, {"Done", Type::Trigger}}, FileUtility},
    Primitive{PrimitiveKind::GenerateGuid, "Generate GUID", "GUID",
              {{"Gen", Type::Trigger}}, {{"GUID", Type::String}}, FileUtility},
    Primitive{PrimitiveKind::SchematicFilePath, "Schematic File Path", "FilePath",
              {}, {{"Folder", Type::String}, {"Filename", Type::String}}, FileUtility},
    Primitive{PrimitiveKind::TextLoad, "Text Load", "Load",
              {{"Path", Type::String}, {"Load", Type::Trigger}},
              {{"Text", Type::String}, {"Loaded", Type::Trigger}}, TextFile},
    Primitive{PrimitiveKind::TextSave, "Text Save", "Save",
              {{"Text", Type::String}, {"Path", Type::String}, {"Save", Type::Trigger}},
              {{"Saved", Type::Trigger}}, TextFile},
    Primitive{PrimitiveKind::StateMachine, "State Machine", "State",
              {{"Ctrl", Type::Int}, {"Map", Type::String}, {"Reset", Type::Trigger}},
              {{"", Type::Int}, {"", Type::Boolean}}, StateMachineLogic},
    Primitive{PrimitiveKind::Settings, "Settings", "Settings", {}, {{"", Type::Float}}, SystemSettings},
    Primitive{PrimitiveKind::SettingsStore, "Settings Store", "Settings Store",
              {{"Group", Type::String}, {"Key", Type::String}, {"Value", Type::String},
               {"Get", Type::Trigger}, {"Set", Type::Trigger}},
              {{"Value", Type::String}, {"Done", Type::Trigger}, {"Error", Type::Trigger}}, PersistSettings},
    Primitive{PrimitiveKind::SystemFolders, "System Folders", "SysFolders",
              {{"ID", Type::Int}}, {{"Docs", Type::String}, {"AppData", Type::String}, {"Custom", Type::String}}, SystemInfo},
    Primitive{PrimitiveKind::SystemFonts, "System Fonts", "Fonts", {{"Update", Type::Trigger}},
              {{"Fonts", Type::StringArray}, {"Text", Type::String}, {"Count", Type::Int}}, SystemInfo},
    Primitive{PrimitiveKind::Point, "Point", "⊙", {{"X", Type::Float}, {"Y", Type::Float}},
              {{"Point", Type::Point}}, MakePoint},
    Primitive{PrimitiveKind::PointToFloat, "Point to Float", "Point",
              {{"Point", Type::Point}}, {{"X", Type::Float}, {"Y", Type::Float}}, SplitPoint},
    Primitive{PrimitiveKind::IndexSelector, "Index Selector", "Index",
              {{"List", Type::String}, {"Sort", Type::Boolean}, {"Set", Type::String}},
              {{"Index", Type::Int}, {"Text", Type::String}}, IndexSelector},
    Primitive{PrimitiveKind::Trigger, "Trigger", "↯", {}, {{"Pulse", Type::Trigger}}, TriggerButton},
    Primitive{PrimitiveKind::Tick, "Tick", "↯", {{"TPS", Type::Float}}, {{"Tick", Type::Trigger}}, Tick},
    Primitive{PrimitiveKind::FloatArrayToFloatMem, "Float Array to Float Mem", "FloatMem",
              {{"Array", Type::FloatArray}}, {{"Mem", Type::Mem}}, MemoryOperation},
    Primitive{PrimitiveKind::IntArrayToFloatMem, "Int Array to Float Mem", "FloatMem",
              {{"Array", Type::IntArray}}, {{"Mem", Type::Mem}}, MemoryOperation},
    Primitive{PrimitiveKind::IntArrayToIntMem, "Int Array to Int Mem", "IntMem",
              {{"Array", Type::IntArray}}, {{"Mem", Type::Mem}}, MemoryOperation},
    Primitive{PrimitiveKind::MemCreate, "Mem Create", "Mem",
              {{"Size", Type::Int}, {"Create", Type::Trigger}, {"Delete", Type::Trigger}, {"Stereo", Type::Boolean}},
              {{"Mem", Type::Mem}}, MemoryOperation},
    Primitive{PrimitiveKind::MemArrayGetAt, "Mem Get At", "GetAt",
              {{"Array", Type::MemArray}, {"Index", Type::Int}, {"Get", Type::Trigger}},
              {{"Mem", Type::Mem}, {"Done", Type::Trigger}}, MemoryOperation},
    Primitive{PrimitiveKind::MemInfo, "Mem Info", "MemInfo", {{"Mem", Type::Mem}},
              {{"Section", Type::Int}, {"Address", Type::Int}, {"Size", Type::Int}, {"Channels", Type::Int}}, MemoryOperation},
    Primitive{PrimitiveKind::MemToFloatArray, "Mem to Float Array", "Mem2FA",
              {{"Mem", Type::Mem}, {"Index", Type::Int}, {"Size", Type::Int}, {"Get", Type::Trigger}},
              {{"Array", Type::FloatArray}}, MemoryOperation},
    Primitive{PrimitiveKind::MemMinMax, "Mem to Float Array Min/Max", "MinMax",
              {{"Mem", Type::Mem}, {"Step", Type::Int}},
              {{"LMin", Type::FloatArray}, {"LMax", Type::FloatArray}, {"RMin", Type::FloatArray},
               {"RMax", Type::FloatArray}, {"N", Type::Int}}, MemoryOperation},
    Primitive{PrimitiveKind::MemToMemArray, "Mem to MemArray", "M2MA",
              {{"Mem", Type::Mem}, {"Size", Type::Int}}, {{"Array", Type::MemArray}}, MemoryOperation},
    Primitive{PrimitiveKind::MemToMipmap, "Mem to Mipmap", "Mipmap",
              {{"Mem", Type::Mem}, {"Size", Type::Int}, {"MaxCut", Type::Float}, {"PerOct", Type::Float},
               {"Stack", Type::Int}, {"FFT", Type::Boolean}},
              {{"Mipmaps", Type::MemArray}, {"Count", Type::Int}}, MemoryOperation},
    Primitive{PrimitiveKind::MemCopy, "Memory Copy", "Copy",
              {{"Dest", Type::Mem}, {"Dest.Index", Type::Int}, {"Src", Type::Mem}, {"Copy", Type::Trigger}},
              {}, MemoryOperation},
    Primitive{PrimitiveKind::MemSection, "Memory Section", "Section",
              {{"Mem", Type::Mem}, {"Index", Type::Int}, {"Size", Type::Int}, {"Get", Type::Trigger}},
              {{"Mem", Type::Mem}}, MemoryOperation},
    Primitive{PrimitiveKind::MemReadByte, "ReadB", "ReadB",
              {{"Mem", Type::Mem}, {"Idx", Type::Int}}, {{"Val", Type::Int}}, MemoryOperation},
    Primitive{PrimitiveKind::MemArraySetAt, "Wave Array Set", "SetArray",
              {{"N", Type::Int}, {"Resize", Type::Trigger}, {"Del", Type::Trigger}, {"Index", Type::Int},
               {"Wave", Type::Mem}, {"Add", Type::Trigger}, {"Mono", Type::Boolean}},
              {{"Array", Type::MemArray}, {"Samples", Type::IntArray}, {"Channels", Type::Int}}, MemoryOperation},
    Primitive{PrimitiveKind::MemWriteByte, "Write Byte", "WriteB",
              {{"Mem", Type::Mem}, {"Idx", Type::Int}, {"Val", Type::Int}, {"Write", Type::Trigger}},
              {}, MemoryOperation},
    Primitive{PrimitiveKind::NotesPitchFilter, "Notes Pitch-Filter", "NPitch",
              {{"Notes", Type::Notes}, {"Low", Type::IntArray}, {"High", Type::IntArray}, {"T-Idx", Type::Int},
               {"T-Val", Type::IntArray}, {"T-Cmp", Type::Int}, {"Dup", Type::Boolean}},
              {{"Match", Type::Notes}, {"Remaining", Type::Notes}}, NotesFilter},
    Primitive{PrimitiveKind::NotesVelocityFilter, "Notes Velocity-Filter", "NVel",
              {{"Notes", Type::Notes}, {"Low", Type::FloatArray}, {"High", Type::FloatArray}, {"T-Idx", Type::Int},
               {"T-Val", Type::IntArray}, {"T-Cmp", Type::Int}, {"Dup", Type::Boolean}},
              {{"Match", Type::Notes}, {"Remaining", Type::Notes}}, NotesFilter},
    Primitive{PrimitiveKind::NotesChannelFilter, "Notes Channel-Filter", "NChan",
              {{"Notes", Type::Notes}, {"Low", Type::IntArray}, {"High", Type::IntArray}, {"T-Idx", Type::Int},
               {"T-Val", Type::IntArray}, {"T-Cmp", Type::Int}, {"Dup", Type::Boolean}},
              {{"Match", Type::Notes}, {"Remaining", Type::Notes}}, NotesFilter},
    Primitive{PrimitiveKind::Voices, "Voices", "Voices",
              {{"NrVoices", Type::Int}, {"NrStolen", Type::Int}, {"RetStolen", Type::Boolean},
               {"TrgSteal", Type::Boolean}, {"TrgReturn", Type::Boolean}}, {{"Voices", Type::Voices}}, VoiceRouting},
    Primitive{PrimitiveKind::NotesToVoices, "Notes to Voices", "N to V",
              {{"Voices", Type::Voices}, {"Notes", Type::Notes}}, {{"Voices", Type::Voices}}, VoiceRouting},
    Primitive{PrimitiveKind::VoicesToPoly, "Voices to Poly", "V to P",
              {{"Voices", Type::Voices}, {"Done", Type::PolyBoolean}},
              {{"Freq", Type::Poly}, {"Pitch", Type::Poly}, {"Vel", Type::Poly}, {"Gate", Type::PolyBoolean},
               {"Trig", Type::Poly}}, VoiceSignal},
    Primitive{PrimitiveKind::PolyChannelId, "Stream Poly Channel-Id", "ChanId", {}, {{"Channel", Type::Poly}}, VoiceSignal},
    Primitive{PrimitiveKind::PolyVoiceId, "Stream Poly Voice-Id", "VoiceId", {}, {{"Voice", Type::Poly}}, VoiceSignal},
    Primitive{PrimitiveKind::PolyNoteId, "Stream Poly Note-Id", "NoteId", {}, {{"Note", Type::Poly}}, VoiceSignal},
    Primitive{PrimitiveKind::PolyTag, "Stream Poly Tag", "Tag", {{"Id", Type::Int}}, {{"Tag", Type::Poly}}, VoiceSignal},
    Primitive{PrimitiveKind::VstMidiIn, "VST-M In", "VST-M In", {}, {{"MIDI", Type::MIDI}}, HostMidi},
    Primitive{PrimitiveKind::VstMidiOut, "VST-M Out", "VST-M Out", {{"MIDI", Type::MIDI}}, {}, HostMidi},
    Primitive{PrimitiveKind::PresetManager, "Preset Manager", "Manager",
              {{"NPrgs", Type::Int}, {"Prog", Type::Int}, {"Name", Type::String}, {"Lock", Type::Boolean},
               {"Save", Type::Trigger}, {"Names", Type::String}, {"Clear", Type::Trigger}, {"Clear All", Type::Trigger}},
              {{"Preset", Type::Preset}, {"Prog", Type::Int}, {"Name", Type::String}, {"Names", Type::String},
               {"Before", Type::Trigger}, {"After", Type::Trigger}}, ManagePresets},
    Primitive{PrimitiveKind::PresetFloat, "Preset Float", "PFloat",
              {{"Preset Bus", Type::Preset}, {"Val", Type::Float}, {"Name", Type::String}, {"Disp", Type::String},
               {"Label", Type::String}, {"Auto", Type::Boolean}, {"Preset", Type::Boolean}, {"Def", Type::Float},
               {"Rec", Type::Boolean}, {"Order", Type::Int}},
              {{"Val", Type::Float}, {"Before", Type::Trigger}, {"After", Type::Trigger}}, PresetParameter},
    Primitive{PrimitiveKind::PresetBool, "Preset Bool", "PBool",
              {{"Preset Bus", Type::Preset}, {"Val", Type::Boolean}, {"Name", Type::String}, {"Disp", Type::String},
               {"Label", Type::String}, {"Auto", Type::Boolean}, {"Preset", Type::Boolean}, {"Def", Type::Boolean},
               {"Rec", Type::Boolean}, {"Order", Type::Int}},
              {{"Val", Type::Boolean}, {"Before", Type::Trigger}, {"After", Type::Trigger}}, PresetParameter},
    Primitive{PrimitiveKind::PresetString, "Preset String", "PString",
              {{"Preset Bus", Type::Preset}, {"Val", Type::String}, {"Name", Type::String}, {"Def", Type::String},
               {"Order", Type::Int}, {"Preset", Type::Boolean}},
              {{"Val", Type::String}, {"Before", Type::Trigger}, {"After", Type::Trigger}}, PresetParameter},
    Primitive{PrimitiveKind::PresetArray, "Preset Array", "PArray",
              {{"Preset Bus", Type::Preset}, {"N", Type::Int}, {"Index", Type::Int}, {"Value", Type::Float},
               {"Name", Type::String}, {"Disp", Type::String}, {"Label", Type::String}, {"Auto", Type::Boolean},
               {"Preset", Type::Boolean}, {"Def", Type::Float}, {"Array", Type::FloatArray}, {"Reset", Type::Trigger},
               {"Rec", Type::Boolean}, {"Order", Type::Int}},
              {{"Array", Type::FloatArray}, {"Value", Type::Float}, {"Index", Type::Int},
               {"Before", Type::Trigger}, {"After", Type::Trigger}}, PresetParameter},
    Primitive{PrimitiveKind::PresetLoad, "Preset Load", "Preset Load",
              {{"Manager", Type::Preset}, {"Filename", Type::String}, {"Curr", Type::Boolean}, {"Fx ID", Type::String},
               {"Min Ver", Type::Int}, {"Max Ver", Type::Int}, {"Load", Type::Trigger}},
              {{"Name(s)", Type::String}, {"Success", Type::Trigger}, {"Error", Type::Trigger},
               {"Fx Ver", Type::Int}, {"Prog", Type::Int}}, PresetUtility},
    Primitive{PrimitiveKind::PresetSave, "Preset Save", "Preset Save",
              {{"Manager", Type::Preset}, {"Filename", Type::String}, {"Name(s)", Type::String}, {"Curr", Type::Boolean},
               {"Fx ID", Type::String}, {"Fx Ver", Type::Int}, {"Save", Type::Trigger}},
              {{"Success", Type::Trigger}, {"Error", Type::Trigger}}, PresetUtility},
    Primitive{PrimitiveKind::PresetToText, "Preset To Text", "ToText",
              {{"Manager", Type::Preset}, {"Name(s)", Type::String}, {"Curr", Type::Boolean}, {"Save", Type::Trigger}},
              {{"Data", Type::String}, {"Done", Type::Trigger}, {"Error", Type::Trigger}}, PresetUtility},
    Primitive{PrimitiveKind::PresetFromText, "Preset From Text", "FromText",
              {{"Manager", Type::Preset}, {"Data", Type::String}, {"Curr", Type::Boolean}, {"Load", Type::Trigger}},
              {{"Name(s)", Type::String}, {"Success", Type::Trigger}, {"Error", Type::Trigger}}, PresetUtility},
    Primitive{PrimitiveKind::PresetShowOrder, "Preset Show Order", "ShowOrder",
              {{"Grouped", Type::Boolean}, {"Get", Type::Trigger}}, {{"Presets", Type::String}}, PresetUtility},
    Primitive{PrimitiveKind::PresetBypass, "Preset Bypass", "PBypass",
              {{"Preset Bus", Type::Preset}, {"Val", Type::Boolean}, {"Name", Type::String}, {"Disp", Type::String},
               {"Label", Type::String}, {"Auto", Type::Boolean}, {"Preset", Type::Boolean}, {"Def", Type::Boolean},
               {"Rec", Type::Boolean}, {"Order", Type::Int}},
              {{"Val", Type::Boolean}, {"Before", Type::Trigger}, {"After", Type::Trigger}}, PresetParameter},
    Primitive{PrimitiveKind::VstInput, "VST Input", "VST In", {{"Channel", Type::Int}}, {{"Mono", Type::Mono}}, PluginHost},
    Primitive{PrimitiveKind::VstOutput, "Mono Out", "Mono Out", {{"Channel", Type::Int}, {"Mono", Type::Mono}}, {}, PluginHost},
    Primitive{PrimitiveKind::VstPluginInfo, "VST Plugin Info", "VST",
              {{"Type", Type::String}, {"Name", Type::String}, {"Vendor", Type::String}, {"Ver", Type::Int},
               {"ID", Type::String}, {"GUID", Type::String}, {"Email", Type::String}, {"URL", Type::String},
               {"Resizable", Type::Boolean}, {"Tables", Type::Boolean}, {"Embed DLLs", Type::Boolean},
               {"Folder", Type::String}, {"VST2", Type::Boolean}, {"VST3", Type::Boolean}}, {}, PluginHost},
    Primitive{PrimitiveKind::Vst3AuxChannels, "VST3 Aux Channels", "VST3 Aux",
              {{"Ins", Type::Int}, {"Outs", Type::Int}}, {{"In States", Type::IntArray}, {"Out States", Type::IntArray}}, PluginHost},
    Primitive{PrimitiveKind::DelayCompensation, "Delay Compensation", "Comp", {{"Delay", Type::Int}}, {}, PluginHost},
    Primitive{PrimitiveKind::VstEditorOpen, "VST Editor Open", "Editor Open", {}, {{"Open", Type::Boolean}}, PluginHost},
    Primitive{PrimitiveKind::IsPlaying, "Is Playing", "Playing",
              {}, {{"Playing", Type::Boolean}, {"Stop", Type::Trigger}}, PluginHost},
    Primitive{PrimitiveKind::OfflineMode, "Offline Mode", "Offline", {}, {{"Offline", Type::Boolean}}, PluginHost},
    Primitive{PrimitiveKind::Tempo, "Tempo", "Tempo", {}, {{"Tempo", Type::Float}}, PluginHost},
    Primitive{PrimitiveKind::TimeSignature, "Time Signature", "TimeSig",
              {}, {{"Num", Type::Float}, {"Den", Type::Float}}, PluginHost},
    Primitive{PrimitiveKind::StartFolder, "Start Folder", "Folder",
              {{"Test", Type::String}}, {{"Folder", Type::String}}, PluginHost},
    Primitive{PrimitiveKind::VstHostInfo, "VST Host Info", "VST Host",
              {{"Vendor", Type::String}, {"Product", Type::String}, {"Version", Type::String}},
              {{"Vendor", Type::String}, {"Product", Type::String}, {"Version", Type::String}}, PluginHost},
    Primitive{PrimitiveKind::MidiIn, "MIDI In", "MIDI In", {}, {{"MIDI", Type::MIDI}}, MidiDevice},
    Primitive{PrimitiveKind::MidiOut, "MIDI Out", "MIDI Out", {{"MIDI", Type::MIDI}}, {}, MidiDevice},
    Primitive{PrimitiveKind::MidiInDevices, "MIDI In Devices", "MidiIns", {},
              {{"N", Type::Int}, {"Names", Type::StringArray}, {"Avail", Type::IntArray}}, MidiDevice},
    Primitive{PrimitiveKind::MidiOutDevices, "MIDI Out Devices", "MidiOuts", {},
              {{"N", Type::Int}, {"Names", Type::StringArray}, {"Avail", Type::IntArray}}, MidiDevice},
    Primitive{PrimitiveKind::MidiInSelect, "MIDI In Select", "MIDI In Sel", {{"Device", Type::Int}},
              {{"MIDI", Type::MIDI}, {"Selected", Type::Int}, {"Unavailable", Type::Trigger}}, MidiDevice},
    Primitive{PrimitiveKind::MidiOutSelect, "MIDI Out Select", "MIDI Out Sel", {{"MIDI", Type::MIDI}, {"Device", Type::Int}},
              {{"Selected", Type::Int}, {"Unavailable", Type::Trigger}}, MidiDevice},
    Primitive{PrimitiveKind::MidiAftertouch, "MIDI Aftertouch", "Aftertouch",
              {{"MIDI", Type::MIDI}}, {{"Pressure", Type::Int}}, MidiOperation},
    Primitive{PrimitiveKind::MidiBuffer, "Midi Buffer", "Buffer",
              {{"MIDI", Type::MIDI}, {"Flush", Type::Trigger}, {"Clear", Type::Trigger}},
              {{"MIDI", Type::MIDI}, {"Flushed", Type::Trigger}}, MidiOperation},
    Primitive{PrimitiveKind::MidiChannelFilter, "Midi Channel Filter", "Channel",
              {{"MIDI", Type::MIDI}, {"Chan", Type::Int}},
              {{"Match", Type::MIDI}, {"Remaining", Type::MIDI}}, MidiOperation},
    Primitive{PrimitiveKind::MidiCC, "MIDI Control Change", "Midi CC",
              {{"MIDI", Type::MIDI}, {"CC", Type::Int}},
              {{"Value", Type::Int}, {"Changed", Type::Trigger}}, MidiOperation},
    Primitive{PrimitiveKind::MidiEvent, "Midi Event", "MidiEvent",
              {{"St", Type::Int}, {"Chn", Type::Int}, {"D1", Type::Int}, {"D2", Type::Int}, {"Send", Type::Trigger}},
              {{"MIDI", Type::MIDI}}, MidiOperation},
    Primitive{PrimitiveKind::MidiNote, "Midi Note", "MidiNote",
              {{"MIDI", Type::MIDI}}, {{"Pitch", Type::Int}, {"Active", Type::Int}, {"Freq", Type::Float}}, MidiOperation},
    Primitive{PrimitiveKind::MidiNoteFilter, "Midi Note Filter", "Note",
              {{"MIDI", Type::MIDI}, {"Note", Type::Int}},
              {{"Match", Type::MIDI}, {"Remaining", Type::MIDI}}, MidiOperation},
    Primitive{PrimitiveKind::MidiPitchBend, "MIDI Pitch Bend", "Pitch Bend",
              {{"MIDI", Type::MIDI}}, {{"Bend", Type::Int}}, MidiOperation},
    Primitive{PrimitiveKind::MidiSplit, "MIDI Splitter", "MidiSplit",
              {{"MIDI", Type::MIDI}},
              {{"Status", Type::Int}, {"Channel", Type::Int}, {"Data 1", Type::Int}, {"Data 2", Type::Int},
               {"Changed", Type::Trigger}}, MidiOperation},
    Primitive{PrimitiveKind::MidiStatusFilter, "Midi Status Filter", "Status",
              {{"MIDI", Type::MIDI}, {"Status", Type::Int}},
              {{"Match", Type::MIDI}, {"Remaining", Type::MIDI}}, MidiOperation},
    Primitive{PrimitiveKind::MidiToNotes, "Midi to Notes", "Notes",
              {{"MIDI", Type::MIDI}, {"NoRpt", Type::Boolean}, {"NoSus", Type::Boolean}},
              {{"Notes", Type::Notes}}, MidiOperation},
    Primitive{PrimitiveKind::MidiNoteEvent, "Note Event", "NoteEvent",
              {{"Chn", Type::Int}, {"Note", Type::Int}, {"Vel", Type::Int}, {"Dur (ms)", Type::Int}, {"Send", Type::Trigger}},
              {{"MIDI", Type::MIDI}}, MidiOperation},
    Primitive{PrimitiveKind::MidiStringToSysex, "String to Sysex", "StrSysex",
              {{"Sysex", Type::String}, {"Send", Type::Trigger}}, {{"MIDI", Type::MIDI}}, MidiOperation},
    Primitive{PrimitiveKind::MidiSysexToString, "Sysex to String", "SysexStr",
              {{"Sysex", Type::MIDI}}, {{"String", Type::String}, {"Received", Type::Trigger}}, MidiOperation},
    Primitive{PrimitiveKind::MidiCCStream, "MIDI Control Change (Stream)", "Midi CC",
              {{"MIDI", Type::MIDI}, {"CC", Type::Int}, {"Min", Type::Float}, {"Max", Type::Float},
               {"Default", Type::Float}}, {{"Value", Type::Poly}}, MidiStream},
    Primitive{PrimitiveKind::MidiAftertouchStream, "MIDI Aftertouch (Stream)", "Aftertouch",
              {{"MIDI", Type::MIDI}}, {{"Pressure", Type::Mono}}, MidiStream},
    Primitive{PrimitiveKind::MidiPitchBendStream, "MIDI Pitch Bend (Stream)", "Pitch Bend",
              {{"MIDI", Type::MIDI}}, {{"Bend", Type::Mono}}, MidiStream},
    Primitive{PrimitiveKind::Time, "Time", "Time", {{"Get", Type::Trigger}},
              {{"Weekday", Type::Int}, {"Day", Type::Int}, {"Month", Type::Int}, {"Year", Type::Int},
               {"Hour", Type::Int}, {"Minute", Type::Int}, {"Second", Type::Int}, {"Millisecond", Type::Int}}, SampleTime},
    Primitive{PrimitiveKind::CustomTimer, "Timer", "Timer",
              {{"Duration (ms)", Type::Int}, {"Start", Type::Trigger}, {"Stop", Type::Trigger}},
              {{"Elapsed", Type::Trigger}}, Timer},
    Primitive{PrimitiveKind::BasicCounter, "Counter", "Counter",
              {{"Inc", Type::Trigger}, {"Reset", Type::Trigger}}, {{"", Type::Int}}, Counter},
    Primitive{PrimitiveKind::TriggerCounter, "Trigger Counter", "#", {{"Pulse", Type::Trigger}}, {{"Count", Type::Int}}, Counter},
    Primitive{PrimitiveKind::AboutToClose, "About To Close", "Closing",
              {{"Hold", Type::Boolean}, {"Done", Type::Trigger}}, {{"Trigger", Type::Trigger}}, ProcessTrigger},
    Primitive{PrimitiveKind::AfterLoad, "After Load", "AfterLoad", {}, {{"Trigger", Type::Trigger}}, ProcessTrigger},
    Primitive{PrimitiveKind::AfterDuplicate, "After Duplicate", "AfterDup", {}, {{"Trigger", Type::Trigger}}, ProcessTrigger},
    Primitive{PrimitiveKind::CounterAdvanced, "Counter Advanced", "CounterUD",
              {{"Step", Type::Trigger}, {"Reset", Type::Trigger}, {"Max", Type::Int}, {"Min", Type::Int},
               {"Type", Type::Int}, {"Start", Type::Int}},
              {{"Current Value", Type::Int}, {"Reached Max", Type::Trigger}, {"Reached Min", Type::Trigger}},
              ProcessTrigger},
    Primitive{PrimitiveKind::TriggerBlocker, "Trigger Blocker", "⊣",
              {{"Value", {}}}, {{"Value", {}}}, Blocker},
    Primitive{PrimitiveKind::TriggerSwitch, "Trigger Switch", "↯",
              {{"Signal", {}}, {"Open", Type::Boolean}}, {{"Signal", {}}}, Switch},
    Primitive{PrimitiveKind::TriggerDiv, "Trigger Div", "Div",
              {{"Trigger", Type::Trigger}, {"Divisor", Type::Int}}, {{"Trigger", Type::Trigger}}, ProcessTrigger},
    Primitive{PrimitiveKind::TriggerDelta, "Trigger Delta", "Delta",
              {{"Trigger", Type::Trigger}}, {{"Delta (ms)", Type::Float}}, ProcessTrigger},
    Primitive{PrimitiveKind::TriggerThread, "Trigger Thread", "TThread",
              {{"Trigger", Type::Trigger}}, {{"Trigger", Type::Trigger}}, ProcessTrigger},
    Primitive{PrimitiveKind::Indexer, "Indexer", "Indexer", {{"Add trigger", Type::Trigger}},
              {{"Index", Type::Int}}, ProcessTrigger},
    Primitive{PrimitiveKind::Loop, "Loop", "Loop",
              {{"Start", {}}, {"Step", {}}, {"End", {}}, {"NoLim", Type::Boolean}, {"Run", Type::Trigger}},
              {{"Value", {}}, {"Iteration", Type::Int}, {"Step", Type::Trigger}, {"Finished", Type::Trigger}},
              ProcessTrigger},
    Primitive{PrimitiveKind::ModuleGui, "MGUI", "V",
              {{"View", Type::View}, {"Mouse", Type::Boolean}, {"Border", Type::Boolean}}, {{"View", Type::View}}, ModuleGui},
    Primitive{PrimitiveKind::ModulePropertiesGui, "Module Properties GUI", "Props",
              {{"View", Type::View}, {"Mouse", Type::Boolean}}, {{"View", Type::View}}, ModuleGui},
    Primitive{PrimitiveKind::Window, "Window", "V",
              {{"Open", Type::Boolean}, {"Width (px)", Type::Int}, {"Height (px)", Type::Int},
               {"Type", Type::Int}, {"Title", Type::String}},
              {{"View", Type::View}, {"Open", Type::Boolean}}, Window},
    Primitive{PrimitiveKind::ViewSection, "View Section", "V",
              {{"View", Type::View}, {"X", Type::Float}, {"Y", Type::Float},
               {"W", Type::Float}, {"H", Type::Float}}, {{"View", Type::View}}, TransformView},
    Primitive{PrimitiveKind::ViewScale, "View Scale", "V",
              {{"View", Type::View}, {"Ratio", Type::Float}}, {{"View", Type::View}}, TransformView},
    Primitive{PrimitiveKind::ViewRotate, "View Rotate", "V",
              {{"View", Type::View}, {"X", Type::Float}, {"Y", Type::Float}, {"Angle", Type::Float}},
              {{"View", Type::View}}, TransformView},
    Primitive{PrimitiveKind::ViewSmooth, "View Smooth", "V",
              {{"View", Type::View}, {"AntiAlias", Type::Boolean}, {"ClearType", Type::Boolean}},
              {{"View", Type::View}}, TransformView},
    Primitive{PrimitiveKind::ViewSize, "View Size", "V",
              {{"View", Type::View}}, {{"W", Type::Float}, {"H", Type::Float}}, ViewAttributes},
    Primitive{PrimitiveKind::ViewArea, "View Area", "V",
              {{"View", Type::View}}, {{"Area", Type::Area}}, ViewAttributes},
    Primitive{PrimitiveKind::Clip, "Clip", "V",
              {{"View", Type::View}, {"Area", Type::Area}, {"Mode", Type::Int}}, {{"View", Type::View}}, TransformView},
    Primitive{PrimitiveKind::GuiArea, "GUI Area", "V",
              {{"View", Type::View}}, {{"Area", Type::Area}}, ViewAttributes},
    Primitive{PrimitiveKind::SetGuiArea, "Set GUI Area", "V",
              {{"View", Type::View}, {"Area", Type::Area}, {"Set", Type::Trigger}}, {{"View", Type::View}}, SetGuiArea},
    Primitive{PrimitiveKind::ViewLoop, "View Loop", "V",
              {{"View", Type::View}, {"Start", {}}, {"Step", {}}, {"End", {}}, {"NoLim", Type::Boolean}},
              {{"View", Type::View}, {"Value", {}}, {"Iteration", Type::Int},
               {"Step", Type::Trigger}, {"Finished", Type::Trigger}}, ViewLoop},
    Primitive{PrimitiveKind::ViewArrayLoop, "View Array Loop", "V",
              {{"View", Type::View}, {"Array", {}}, {"Rev", Type::Boolean}, {"NoLim", Type::Boolean}},
              {{"View", Type::View}, {"Value", {}}, {"Index", Type::Int},
               {"Step", Type::Trigger}, {"Finished", Type::Trigger}}, ViewLoop},
    Primitive{PrimitiveKind::ViewCache, "View Cache", "V",
              {{"View", Type::View}, {"Clear", Type::Trigger}}, {{"View", Type::View}}, ViewCache},
    Primitive{PrimitiveKind::ViewShadow, "View Shadow", "V",
              {{"View", Type::View}, {"Distance", Type::Point}, {"Blur", Type::Point},
               {"Alpha", Type::Float}, {"Color", Type::Color}, {"ColMix", Type::String},
               {"ShOnly", Type::Boolean}}, {{"View", Type::View}}, ViewEffect},
    Primitive{PrimitiveKind::ViewBlur, "View Blur", "V",
              {{"View", Type::View}, {"Size", Type::Point}, {"Sigma", Type::Point},
               {"Chans", Type::String}}, {{"View", Type::View}}, ViewEffect},
    Primitive{PrimitiveKind::ViewGlow, "View Glow", "V",
              {{"View", Type::View}, {"Blur", Type::Point}, {"Color", Type::Color},
               {"Strength", Type::Float}, {"Glow Only", Type::Boolean}}, {{"View", Type::View}}, ViewEffect},
    Primitive{PrimitiveKind::ViewShader, "View Shader", "V",
              {{"View", Type::View}, {"Shader", Type::String}, {"Time", Type::Float},
               {"Amount", Type::Float}}, {{"View", Type::View}}, ViewEffect},
    Primitive{PrimitiveKind::ViewBloom, "View Bloom", "V",
              {{"View", Type::View}, {"Blur", Type::Point}, {"Threshold", Type::Float},
               {"Strength", Type::Float}}, {{"View", Type::View}}, ViewEffect},
    Primitive{PrimitiveKind::GridToPixel, "Grid to Pixel", "Pixel",
              {{"Grid", Type::Float}}, {{"Pixel", Type::Float}}, PanelUtility},
    Primitive{PrimitiveKind::GridToPixelArea, "Grid to Pixel (Area)", "Pixel",
              {{"Grid", Type::Area}}, {{"Pixel", Type::Area}}, PanelUtility},
    Primitive{PrimitiveKind::PixelToGrid, "Pixel to Grid", "Grid",
              {{"Pixel", Type::Float}}, {{"Grid", Type::Float}}, PanelUtility},
    Primitive{PrimitiveKind::PixelToGridArea, "Pixel to Grid (Area)", "Grid",
              {{"Pixel", Type::Area}}, {{"Grid", Type::Area}}, PanelUtility},
    Primitive{PrimitiveKind::TooltipHelp, "Tooltip Help", "Help", {{"Text", Type::String}}, {}, PanelUtility},
    Primitive{PrimitiveKind::RedrawLimiter, "Redraw Limiter", "Limit",
              {{"In", Type::Trigger}}, {{"Out", Type::Trigger}}, PanelUtility},
    Primitive{PrimitiveKind::RedrawsSuspended, "Suspend Redraws", "Suspended",
              {}, {{"Suspended", Type::Boolean}}, PanelUtility},
    Primitive{PrimitiveKind::PanelBackgroundColor, "Background Color", "Background",
              {{"Color", Type::Color}}, {}, PresentationControl},
    Primitive{PrimitiveKind::ExeQuit, "Exe Quit", "Quit",
              {{"Quit", Type::Trigger}}, {}, PresentationControl},
    Primitive{PrimitiveKind::FocusMode, "Focus Mode", "Focus",
              {{"Exit", Type::Trigger}}, {}, PresentationControl},
    Primitive{PrimitiveKind::FullScreen, "Full Screen", "FullScreen",
              {{"Toggle", Type::Trigger}}, {{"Full Screen", Type::Boolean}}, PresentationControl},
    Primitive{PrimitiveKind::PanelZoom, "Zoom", "Zoom",
              {{"Scale", Type::Float}, {"Set", Type::Trigger}}, {{"Scale", Type::Float}}, PresentationControl},
    Primitive{PrimitiveKind::KeyDown, "Key Down", "KeyDown",
              {{"Enable", Type::Boolean}, {"Key Codes", Type::IntArray}},
              {{"Code", Type::Int}, {"Repeat", Type::Int}, {"Flags", Type::Int}, {"Char", Type::String}}, KeyboardInput},
    Primitive{PrimitiveKind::KeyUp, "Key Up", "KeyUp",
              {{"Enable", Type::Boolean}, {"Key Codes", Type::IntArray}},
              {{"Code", Type::Int}, {"Repeat", Type::Int}, {"Flags", Type::Int}, {"Char", Type::String}}, KeyboardInput},
    Primitive{PrimitiveKind::IsKeyPressed, "Is Key Pressed", "Key?",
              {{"Key", Type::String}, {"Check", Type::Trigger}}, {{"Pressed", Type::Boolean}}, KeyboardInput},
    Primitive{PrimitiveKind::Redraw, "Redraw", "V",
              {{"View", Type::View}, {"DoIt", Type::Trigger}}, {{"View", Type::View}}, Redraw},
    Primitive{PrimitiveKind::RedrawArea, "Redraw Area", "V",
              {{"View", Type::View}, {"Area", Type::Area}, {"DoIt", Type::Trigger}}, {{"View", Type::View}}, Redraw},
    Primitive{PrimitiveKind::DropList, "DropList", "▾",
              {{"View", Type::View}, {"X", Type::Float}, {"Y", Type::Float}, {"List", Type::String},
               {"Open", Type::Trigger}, {"Check", Type::Int}, {"Style", Type::String}},
              {{"Selected String", Type::String}, {"Selected Index", Type::Int}, {"Number of Options", Type::Int},
               {"Canceled", Type::Trigger}}, DropList},
    Primitive{PrimitiveKind::EditControl, "Edit Control", "Aa",
              {{"View", Type::View}, {"Area", Type::Area}, {"Font", Type::Font}, {"Text", Type::Color},
               {"Back", Type::Color}, {"Set", Type::String}, {"Trig", Type::Boolean}, {"Start", Type::Trigger},
               {"Multi", Type::Boolean}, {"Pwd", Type::Boolean}},
              {{"View", Type::View}, {"Current Text", Type::String}, {"Is Editing", Type::Boolean}}, EditControl},
    Primitive{PrimitiveKind::PopupList, "Popup List", "▾",
              {{"View", Type::View}, {"Area", Type::Area}, {"Font", Type::Font}, {"Text", Type::Color},
               {"Back", Type::Color}, {"List", Type::String}, {"Rows", Type::Int}, {"Set", Type::Int}},
              {{"View", Type::View}, {"Selected String", Type::String}, {"Selected Index", Type::Int}}, PopupList},
    Primitive{PrimitiveKind::DropFiles, "Drop Files", "↓",
              {{"View", Type::View}, {"Area", Type::Area}, {"Ext", Type::String}, {"Dir", Type::Boolean}, {"Max", Type::Int}},
              {{"Paths", Type::StringArray}, {"Mouse X", Type::Float}, {"Mouse Y", Type::Float},
               {"Dragging Over", Type::Boolean}, {"File(s) Dropped", Type::Trigger}}, DropFiles},
    Primitive{PrimitiveKind::MouseArea, "Rectangular Mouse Area", "M", {{"View", Type::View}, {"Area", Type::Area}},
              {{"Mouse Messages", Type::MouseMessage}}, MouseArea},
    Primitive{PrimitiveKind::PolygonalMouseArea, "Polygonal Mouse Area", "M", {{"View", Type::View}, {"Pts", Type::PointArray}},
              {{"Mouse Messages", Type::MouseMessage}}, MouseArea},
    Primitive{PrimitiveKind::MouseInfo, "Mouse Info", "M", {{"Mouse Messages", Type::MouseMessage},
              {"Cursor", Type::String}, {"Tip", Type::String}}, {}, MouseInfo},
    Primitive{PrimitiveKind::ShowCursor, "Show Cursor", "M",
              {{"Show", Type::Boolean}, {"Hold", Type::Boolean}}, {}, ShowCursor},
    Primitive{PrimitiveKind::MouseCapture, "Mouse Capture", "Capture",
              {{"Mouse Messages", Type::MouseMessage}, {"Start", Type::Trigger}, {"Stop", Type::Trigger}},
              {{"Mouse Messages", Type::MouseMessage}}, MouseCaptureControl},
    Primitive{PrimitiveKind::MouseDrag, "Mouse Drag", "M", {{"Mouse Messages", Type::MouseMessage},
              {"Cursor", Type::String}, {"Reset", Type::Boolean}},
              {{"Mouse Down", Type::Point}, {"Position", Type::Point}, {"Mouse Up", Type::Point},
               {"Dragging", Type::Boolean}}, MouseDrag},
    Primitive{PrimitiveKind::MouseDragAccumulate, "Mouse Drag Accumulate", "M", {{"Mouse Messages", Type::MouseMessage},
              {"Cursor", Type::String}, {"Reset", Type::Boolean}, {"Scale", Type::Point}, {"Set", Type::Point}},
              {{"Mouse Down", Type::Point}, {"Position", Type::Point}, {"Mouse Up", Type::Point},
               {"Dragging", Type::Boolean}}, MouseDrag},
    Primitive{PrimitiveKind::DragFiles, "Drag Files", "↑",
              {{"Mouse Messages", Type::MouseMessage}, {"Files", Type::StringArray}, {"Mode", Type::String}},
              {{"Dragging", Type::Boolean}, {"Dropped Successfully", Type::Boolean}}, DragFiles},
    mouseParser(PrimitiveKind::MouseLDbl, "Mouse LDbl", "Pos", Type::Point),
    mouseParser(PrimitiveKind::MouseLDown, "Mouse LDown", "Pos", Type::Point),
    mouseParser(PrimitiveKind::MouseLUp, "Mouse LUp", "Pos", Type::Point),
    mouseParser(PrimitiveKind::MouseRDbl, "Mouse RDbl", "Pos", Type::Point),
    mouseParser(PrimitiveKind::MouseRDown, "Mouse RDown", "Pos", Type::Point),
    mouseParser(PrimitiveKind::MouseRUp, "Mouse RUp", "Pos", Type::Point),
    mouseParser(PrimitiveKind::MouseWheel, "Mouse Wheel", "Delta", Type::Float),
    Primitive{PrimitiveKind::MouseMove, "Mouse Move", "M", {{"View", Type::View}}, {{"Pos", Type::Point}}, MouseMove},
    Primitive{PrimitiveKind::Area, "Float to Area", "F2A",
              {{"X", Type::Float}, {"Y", Type::Float}, {"Width", Type::Float}, {"Height", Type::Float}},
              {{"Area", Type::Area}}, AreaGeometry},
    Primitive{PrimitiveKind::AreaToFloat, "Area to Float", "A2F", {{"Area", Type::Area}},
              {{"X", Type::Float}, {"Y", Type::Float}, {"Width", Type::Float}, {"Height", Type::Float}}, AreaGeometry},
    Primitive{PrimitiveKind::AreaAroundCenterPoint, "Area Around Center Point", "C2A",
              {{"Center", Type::Point}, {"Width", Type::Float}, {"Height", Type::Float}},
              {{"Area", Type::Area}}, AreaGeometry},
    Primitive{PrimitiveKind::AreaFromPoints, "Area From Points", "P2A", {{"P1", Type::Point}, {"P2", Type::Point}},
              {{"Area", Type::Area}}, AreaGeometry},
    Primitive{PrimitiveKind::AreaCenterPoint, "Area Center Point", "Center", {{"Area", Type::Area}},
              {{"Center", Type::Point}}, AreaGeometry},
    Primitive{PrimitiveKind::AlignArea, "Align Area", "Align",
              {{"Ref", Type::Area}, {"Src", Type::Area}, {"Horz", Type::String}, {"Vert", Type::String}},
              {{"Area", Type::Area}}, AreaGeometry},
    Primitive{PrimitiveKind::AreaToPoints, "Area to Points", "A2P", {{"Area", Type::Area}},
              {{"TL", Type::Point}, {"BR", Type::Point}}, AreaGeometry},
    Primitive{PrimitiveKind::AreaUnion, "Area Union", "∪", {{"A", Type::Area}, {"B", Type::Area}},
              {{"Area", Type::Area}}, AreaGeometry},
    Primitive{PrimitiveKind::PointInsideArea, "Point Inside Area", "⊡", {{"Area", Type::Area}, {"Point", Type::Point}},
              {{"Inside", Type::Boolean}}, AreaGeometry},
    Primitive{PrimitiveKind::PointInsidePolygon, "Point Inside Polygon", "⊡",
              {{"Polygon", Type::PointArray}, {"Point", Type::Point}}, {{"Inside", Type::Boolean}}, AreaGeometry},
    Primitive{PrimitiveKind::Color, "Color", "C", {{"Color", Type::Color}}, {{"Color", Type::Color}}, ValueBox},
    Primitive{PrimitiveKind::ColorToArgb, "Color to ARGB", "ARGB", {{"Color", Type::Color}},
              {{"Alpha", Type::Int}, {"Red", Type::Int}, {"Green", Type::Int}, {"Blue", Type::Int}}, SplitColor},
    Primitive{PrimitiveKind::ColorToAhsv, "Color to AHSV", "AHSV", {{"Color", Type::Color}},
              {{"Alpha", Type::Int}, {"Hue", Type::Int}, {"Sat", Type::Int}, {"Val", Type::Int}}, SplitColor},
    Primitive{PrimitiveKind::ArgbToColor, "ARGB to Color", "ARGB",
              {{"Alpha", Type::Int}, {"Red", Type::Int}, {"Green", Type::Int}, {"Blue", Type::Int}},
              {{"Color", Type::Color}}, MakeColor},
    Primitive{PrimitiveKind::AhsvToColor, "AHSV to Color", "AHSV",
              {{"Alpha", Type::Int}, {"Hue", Type::Int}, {"Sat", Type::Int}, {"Val", Type::Int}},
              {{"Color", Type::Color}}, MakeColor},
    Primitive{PrimitiveKind::ColorMatrix, "Color Matrix", "CM",
              {{"Data", Type::String}, {"Parse", Type::Trigger}},
              {{"Matrix", Type::ColorMatrix}, {"OK", Type::Boolean}}, ParseColorMatrix},
    Primitive{PrimitiveKind::ColorMatrixSet, "Color Matrix Set", "Ⅱ",
              {{"Matrix", Type::ColorMatrix}, {"Row", Type::Int}, {"Col", Type::Int}, {"Value", Type::Float}},
              {{"Modified matrix", Type::ColorMatrix}}, ColorMatrixSet},
    Primitive{PrimitiveKind::ApplyColorMatrix, "Apply Color Matrix", "Ⅱ",
              {{"Color", Type::Color}, {"Matrix", Type::ColorMatrix}}, {{"Color", Type::Color}}, ApplyColorMatrix},
    Primitive{PrimitiveKind::Pen, "Pen", "▯",
              {{"Color", Type::Color}, {"Width", Type::Float}, {"Style", Type::String}},
              {{"Paint Tool", Type::PaintTool}}, MakePen},
    Primitive{PrimitiveKind::PenAttributes, "Pen Attributes", "PenAttr",
              {{"", Type::PaintTool}}, {{"Col", Type::Color}, {"Thk", Type::Float}, {"Style", Type::String}}, PenInfo},
    Primitive{PrimitiveKind::Solid, "Solid", "▯",
              {{"Color", Type::Color}}, {{"Paint Tool", Type::PaintTool}}, MakeSolid},
    Primitive{PrimitiveKind::LinearGradientBrush, "Linear Gradient Brush", "LinGrad",
              {{"Col1", Type::Color}, {"Col2", Type::Color}, {"Area", Type::Area}, {"Angle", Type::Float},
               {"Wrap", Type::String}}, {{"Paint Tool", Type::PaintTool}}, MakeBrush},
    Primitive{PrimitiveKind::RadialGradientBrush, "Radial Gradient Brush", "RadGrad",
              {{"Col1", Type::Color}, {"Col2", Type::Color}, {"Area", Type::Area}, {"Shape", Type::String},
               {"Ctr X", Type::Float}, {"Ctr Y", Type::Float}, {"H-Focus", Type::Float}, {"V-Focus", Type::Float}},
              {{"Paint Tool", Type::PaintTool}}, MakeBrush},
    Primitive{PrimitiveKind::HatchBrush, "Hatch Brush", "Hatch",
              {{"Fg", Type::Color}, {"Bg", Type::Color}, {"Style", Type::String}},
              {{"Paint Tool", Type::PaintTool}}, MakeBrush},
    Primitive{PrimitiveKind::TextureBrush, "Texture Brush", "Texture",
              {{"Bitmap", Type::Bitmap}, {"Wrap", Type::String}}, {{"Paint Tool", Type::PaintTool}}, MakeBrush},
    Primitive{PrimitiveKind::Arc, "Arc", "◠",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool},
               {"StartA", Type::Float}, {"SweepA", Type::Float}}, {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::Ellipse, "Ellipse", "○",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool}},
              {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::Line, "Line", "╱",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool}},
              {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::Pie, "Pie", "◔",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool},
               {"StartA", Type::Float}, {"SweepA", Type::Float}}, {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::Rect, "Rect", "▭",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool},
               {"Corner", Type::Float}}, {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::RoundRectangle, "Round Rectangle", "RRect",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool},
               {"Corner", Type::Float}}, {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::Text, "Text", "T",
              {{"View", Type::View}, {"Area", Type::Area}, {"Color", Type::Color},
               {"Text", Type::String}, {"Font", Type::Font}, {"SF", Type::StringFormat}},
              {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::MeasureText, "Measure Text", "A",
              {{"Text", Type::String}, {"MaxArea", Type::Area}, {"Font", Type::Font}, {"SF", Type::StringFormat}},
              {{"Area", Type::Area}}, MeasureText},
    Primitive{PrimitiveKind::BitmapArea, "Bitmap Area", "□",
              {{"View", Type::View}, {"Bmp", Type::Bitmap}, {"Src", Type::Area},
               {"Dest", Type::Area}, {"Matrix", Type::ColorMatrix}}, {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::BitmapDraw, "Bitmap Draw", "□",
              {{"View", Type::View}, {"Area", Type::Area}, {"Bitmap", Type::Bitmap}, {"Alpha", Type::Int}},
              {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::BitmapDrawTransform, "Bitmap Draw Transform", "□",
              {{"View", Type::View}, {"Area", Type::Area}, {"Bitmap", Type::Bitmap}, {"CM", Type::ColorMatrix}},
              {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::GraphDots, "GraphDots", "⠪",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool}, {"Data", Type::FloatArray},
               {"Top Y", Type::Float}, {"Bottom Y", Type::Float}, {"Log X", Type::Boolean}, {"Radius", Type::Float}},
              {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::LineGraph, "LineGraph", "⌁",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool}, {"Data", Type::FloatArray},
               {"Top Y", Type::Float}, {"Bottom Y", Type::Float}, {"Log X", Type::Boolean}, {"Curve", Type::Boolean}},
              {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::Dots, "Dots", "⠕",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool}, {"Data", Type::PointArray},
               {"Left X", Type::Float}, {"Right X", Type::Float}, {"Top Y", Type::Float}, {"Bottom Y", Type::Float},
               {"Radius", Type::Float}}, {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::DrawPointLines, "Draw Point Lines", "⌁",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool}, {"Data", Type::PointArray},
               {"Left X", Type::Float}, {"Right X", Type::Float}, {"Top Y", Type::Float}, {"Bottom Y", Type::Float},
               {"Curve", Type::Boolean}}, {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::DrawPolygon, "Draw Polygon", "⬠",
              {{"View", Type::View}, {"Area", Type::Area}, {"Tool", Type::PaintTool}, {"Data", Type::PointArray},
               {"Left X", Type::Float}, {"Right X", Type::Float}, {"Top Y", Type::Float}, {"Bottom Y", Type::Float},
               {"Curve", Type::Boolean}}, {{"View", Type::View}}, DrawView},
    Primitive{PrimitiveKind::Font, "Font", "ƒ",
              {{"Face", Type::String}, {"Size", Type::Float}, {"Style", Type::String}, {"AltF", Type::String}},
              {{"Font", Type::Font}}, MakeFont},
    Primitive{PrimitiveKind::FontEmbed, "Font Embed", "FontEmbed",
              {{"Files", Type::StringArray}, {"Load", Type::Trigger}}, {{"", Type::Boolean}}, EmbedFonts},
    Primitive{PrimitiveKind::FontAttr, "FontAttr", "ƒ", {{"Font", Type::Font}},
              {{"Face", Type::String}, {"Size", Type::Float}, {"Style", Type::String}}, FontAttributes},
    Primitive{PrimitiveKind::StringFormat, "String Format", "¶",
              {{"Style", Type::String}, {"Horz", Type::String}, {"Vert", Type::String}},
              {{"String Format", Type::StringFormat}}, MakeStringFormat},
    Primitive{PrimitiveKind::StrFmtAttr, "StrFmtAttr", "¶", {{"SF", Type::StringFormat}},
              {{"Style", Type::String}, {"Horz", Type::String}, {"Vert", Type::String}}, StringFormatAttributes},
    Primitive{PrimitiveKind::Bitmap, "Bitmap", "□",
              {{"Path", Type::String}, {"Load", Type::Trigger}, {"Clear", Type::Trigger}},
              {{"Bitmap", Type::Bitmap}, {"Width", Type::Int}, {"Height", Type::Int}}, LoadBitmap},
    Primitive{PrimitiveKind::BitmapArrayFromFile, "Bitmap Array from File", "□[]",
              {{"Path", Type::String}, {"Index", Type::Int}, {"Load", Type::Trigger}, {"Clear", Type::Trigger}, {"Enc", Type::String}},
              {{"Bitmap Array", Type::BitmapArray}, {"Load complete", Type::Trigger}}, LoadBitmapArray},
    Primitive{PrimitiveKind::BitmapResize, "Bitmap Resize", "□",
              {{"Bitmap", Type::Bitmap}, {"Scale", Type::Float}, {"Resize", Type::Trigger}},
              {{"Bitmap", Type::Bitmap}, {"Resized", Type::Trigger}}, BitmapOperation},
    Primitive{PrimitiveKind::BitmapSampleAndHold, "Bitmap Sample and Hold", "□",
              {{"Bitmap", Type::Bitmap}, {"Sample", Type::Trigger}, {"Enc", Type::String}},
              {{"Bitmap", Type::Bitmap}, {"Sampled", Type::Trigger}}, BitmapOperation},
    Primitive{PrimitiveKind::BitmapSize, "Bitmap Size", "□",
              {{"Bitmap", Type::Bitmap}}, {{"W", Type::Int}, {"H", Type::Int}}, BitmapMetadata},
    Primitive{PrimitiveKind::BitmapSave, "Bitmap Save", "□",
              {{"Bitmap", Type::Bitmap}, {"Path", Type::String}, {"Save", Type::Trigger}, {"Enc", Type::String}},
              {{"Saved", Type::Trigger}}, BitmapOperation},
    Primitive{PrimitiveKind::BitmapCreate, "Create Bitmap", "□",
              {{"W", Type::Int}, {"H", Type::Int}, {"Step", Type::Float}, {"Create", Type::Trigger}, {"Redraw", Type::Trigger}},
              {{"Bitmap", Type::Bitmap}, {"View", Type::View}, {"Created", Type::Trigger}}, BitmapCanvas},
    Primitive{PrimitiveKind::BitmapArrayFromBitmap, "Bitmap Array from Bitmap", "□[]",
              {{"Bitmap", Type::Bitmap}, {"Index", Type::Int}, {"Set", Type::Trigger}, {"Enc", Type::String}, {"Clear", Type::Trigger}},
              {{"Bitmap Array", Type::BitmapArray}, {"Set complete", Type::Trigger}}, LoadBitmapArray},
    Primitive{PrimitiveKind::BitmapBounds, "Bitmap Bounds", "□",
              {{"Bitmap", Type::Bitmap}}, {{"Area", Type::Area}}, BitmapMetadata},
    Primitive{PrimitiveKind::BitmapGetPixel, "Get Pixel", "□",
              {{"Bitmap", Type::Bitmap}, {"X", Type::Int}, {"Y", Type::Int}, {"Get", Type::Trigger}},
              {{"Color", Type::Color}}, BitmapOperation},
    Primitive{PrimitiveKind::BitmapGetAt, "Bitmap Get At", "□",
              {{"Bitmap Array", Type::BitmapArray}, {"Index", Type::Int}, {"Get", Type::Trigger}},
              {{"Bitmap", Type::Bitmap}, {"Done", Type::Trigger}}, BitmapOperation},
    Primitive{PrimitiveKind::BitmapSetPixel, "Set Pixel", "□",
              {{"Bitmap", Type::Bitmap}, {"X", Type::Int}, {"Y", Type::Int}, {"Color", Type::Color},
               {"Set", Type::Trigger}}, {}, BitmapOperation},
    Primitive{PrimitiveKind::DrawToBitmap, "Draw to Bitmap", "□",
              {{"Bitmap", Type::Bitmap}, {"Step", Type::Float}, {"Draw", Type::Trigger}},
              {{"Bitmap", Type::Bitmap}, {"View", Type::View}, {"Drawn", Type::Trigger}}, BitmapCanvas},
    Primitive{PrimitiveKind::Svg, "SVG", "□",
              {{"Path", Type::String}, {"Width (px)", Type::Float}, {"Height (px)", Type::Float},
               {"X-Mode", Type::String}, {"Y-Mode", Type::String}, {"Load", Type::Trigger}},
              {{"Bitmap", Type::Bitmap}, {"Success", Type::Trigger}, {"Error", Type::Trigger}}, BitmapOperation},
    Primitive{PrimitiveKind::BusCreate, "Bus Create", "B", {{"Names", Type::String}}, {{"Bus", Type::Bus}}, CreateBus},
    Primitive{PrimitiveKind::BusExtract, "Bus Extract", "B",
              {{"Names", Type::String}, {"Bus", Type::Bus}}, {}, ExtractBus},
    Primitive{PrimitiveKind::Module, "Module", "▣", {}, {}, Module},
    Primitive{PrimitiveKind::ModuleInput, "Module Input", "→",
              {}, {{"Signal", {}}}, ModuleInput},
    Primitive{PrimitiveKind::ModuleOutput, "Module Output", "→",
              {{"Signal", {}}}, {}, ModuleOutput},
    Primitive{PrimitiveKind::ModuleInputProperty, "Module Input (Property)", "P →",
              {}, {{"Signal", {}}}, PropertyInput},
    Primitive{PrimitiveKind::ModuleOutputProperty, "Module Output (Property)", "→ P",
              {{"Signal", {}}}, {}, PropertyOutput},
    Primitive{PrimitiveKind::WirelessOutput, "Wireless Output", "↗",
              {{"Signal", {}}}, {}, SendWireless},
    Primitive{PrimitiveKind::ModuleWirelessOutput, "Module Wireless Output", "↗",
              {{"Signal", {}}}, {}, SendWireless},
    Primitive{PrimitiveKind::ModuleWirelessLink, "Module Wireless Output (Dynamic)", "WL-Out",
              {{"Signal", {}}, {"Name", Type::String}}, {}, SendWireless},
    Primitive{PrimitiveKind::WirelessInput, "Wireless Input", "↘",
              {}, {{"Signal", {}}}, ReceiveWireless},
    Primitive{PrimitiveKind::WirelessLink, "WL-Link", "↗",
              {{"Signal", {}}, {"Name", Type::String}}, {}, SendWireless},
    Primitive{PrimitiveKind::WirelessReceiver, "Wireless", "↘",
              {{"Name", Type::String}}, {{"Signal", {}}}, ReceiveWireless},
};

Value defaultValue(Type type)
{
    switch (type) {
    case Type::Int: return std::int32_t{0};
    case Type::Float: return 0.0F;
    case Type::String: return std::string{};
    case Type::Boolean: return false;
    case Type::Point: return green::Point{};
    case Type::IntArray: return green::IntArray{};
    case Type::FloatArray: return green::FloatArray{};
    case Type::StringArray: return green::StringArray{};
    case Type::PointArray: return green::PointArray{};
    case Type::View: return yellow::View{};
    case Type::Area: return yellow::Area{};
    case Type::MouseMessage: return yellow::MouseMessage{};
    case Type::Color: return yellow::Color{};
    case Type::ColorMatrix: return yellow::ColorMatrix{};
    case Type::PaintTool: return yellow::PaintTool{};
    case Type::Font: return yellow::Font{};
    case Type::StringFormat: return yellow::StringFormat{};
    case Type::Bitmap: return yellow::Bitmap{};
    case Type::BitmapArray: return yellow::BitmapArray{};
    case Type::Bus: return green::Bus{};
    case Type::Mem: return green::Memory{};
    case Type::MemArray: return green::MemoryArray{};
    case Type::MIDI: return green::MidiMessage{};
    case Type::Notes: return green::Notes{};
    case Type::Voices: return green::Voices{};
    case Type::Preset: return green::Preset{};
    case Type::RubyValue: return green::RubyValue{};
    default: throw std::invalid_argument("No default value for this connector type");
    }
}

std::optional<Value> initialValue(const Primitive& spec)
{
    if (spec.kind == PrimitiveKind::Color) {
        return yellow::Color{0xFF40C8C3};
    }
    if (spec.kind == PrimitiveKind::Settings) return std::int32_t{0};
    if (spec.kind == PrimitiveKind::StreamToGreen) return {};
    if (spec.behavior == ValueBox || spec.behavior == Counter || spec.behavior == SampleMono ||
        spec.behavior == IndexSelector ||
        spec.kind == PrimitiveKind::RandomNumber || spec.kind == PrimitiveKind::FloatSlide ||
        spec.behavior == LoadBitmap || spec.behavior == LoadBitmapArray || spec.behavior == BitmapCanvas ||
        (spec.behavior == BitmapOperation && !spec.outputs.empty() && spec.outputs.front().type != Type::Trigger)) {
        return defaultValue(spec.outputs.front().type.value());
    }
    return std::nullopt;
}

std::size_t expandablePortCount(auto&& links, int SchematicLink::*index, std::size_t first = 0)
{
    std::size_t count = first + 1;
    for (const auto link : links) {
        count = std::max(count, static_cast<std::size_t>(link.*index) + 2);
    }
    return count;
}

std::size_t expandableOutputCount(const SchematicNode& entry, auto&& links)
{
    auto count = expandablePortCount(links, &SchematicLink::output);
    if (entry.kind == PrimitiveKind::Multiplexer && entry.flowStoneRoutingBranches)
        count = std::max(count, std::size_t(*entry.flowStoneRoutingBranches) + 1);
    return count;
}

double comparisonNumber(const Value& value)
{
    if (const auto* integer = std::get_if<std::int32_t>(&value)) {
        return *integer;
    }
    return std::get<float>(value);
}

bool isNumericComparisonType(Type type)
{
    return type == Type::Int || type == Type::Float;
}

bool isNumericComparisonArray(Type type)
{
    return type == Type::IntArray || type == Type::FloatArray;
}

std::partial_ordering compareMixedNumericArrays(const auto& left, const auto& right)
{
    return std::lexicographical_compare_three_way(left.begin(), left.end(), right.begin(), right.end(),
        [](double a, double b) { return a <=> b; });
}

std::partial_ordering comparisonOrder(const Value& left, const Value& right)
{
    using green::FloatArray;
    using green::IntArray;
    using green::StringArray;
    switch (green::typeOf(left)) {
    case Type::IntArray: {
        const auto& values = std::get<IntArray>(left);
        if (const auto* floats = std::get_if<FloatArray>(&right)) return compareMixedNumericArrays(values, *floats);
        return values <=> std::get<IntArray>(right);
    }
    case Type::FloatArray: {
        const auto& values = std::get<FloatArray>(left);
        if (const auto* ints = std::get_if<IntArray>(&right)) return compareMixedNumericArrays(values, *ints);
        return values <=> std::get<FloatArray>(right);
    }
    case Type::StringArray: return std::get<StringArray>(left) <=> std::get<StringArray>(right);
    case Type::String: return std::get<std::string>(left) <=> std::get<std::string>(right);
    default: return comparisonNumber(left) <=> comparisonNumber(right);
    }
}

Type arithmeticResult(Type left, Type right)
{
    if (left == right) {
        return left;
    }
    if ((left == Type::Int && right == Type::Float) || (left == Type::Float && right == Type::Int)) {
        return Type::Float;
    }
    if (isArray(right)) {
        std::swap(left, right);
    }
    if ((left == Type::IntArray || left == Type::FloatArray) &&
        (right == Type::Int || right == Type::Float)) {
        return left == Type::FloatArray || right == Type::Float ? Type::FloatArray : Type::IntArray;
    }
    if ((left == Type::PointArray && right == Type::Point) ||
        (left == Type::StringArray && right == Type::String)) {
        return left;
    }
    throw std::invalid_argument("These connector types cannot be combined by this operator");
}

}

bool Primitive::hasTemplateConnectors() const
{
    const auto isTemplate = [](const Port& port) { return !port.type; };
    return std::ranges::any_of(outputs, isTemplate) || std::ranges::any_of(inputs, isTemplate);
}

std::span<const Primitive> primitives()
{
    return catalog;
}

const Primitive& primitive(PrimitiveKind kind)
{
    const auto found = std::ranges::find(catalog, kind, &Primitive::kind);
    if (found == catalog.end()) {
        throw std::invalid_argument("Unknown primitive");
    }
    return *found;
}

Schematic::Schematic(BitmapLoader bitmapLoader, BitmapBackend bitmapBackend, TextMeasurer textMeasurer,
                     TextFileBackend textFiles, SystemInfoBackend systemInfo)
    : Schematic(std::move(bitmapLoader), nullptr, 0)
{
    bitmapBackend_ = std::move(bitmapBackend);
    textMeasurer_ = std::move(textMeasurer);
    textFiles_ = std::move(textFiles);
    systemInfo_ = std::move(systemInfo);
    rubyClockStart_ = sequenceNow();
}

Schematic::Schematic(BitmapLoader bitmapLoader, Schematic* parent, int moduleId)
    : runtime_(std::make_unique<green::Runtime>())
    , bitmapLoader_(std::move(bitmapLoader))
    , parent_(parent)
    , path_(parent ? parent->path_ : std::vector<int>{})
    , location_(parent ? nullptr : std::make_shared<SchematicLocation>())
{
    if (parent_) {
        path_.push_back(moduleId);
    }
}

bool Schematic::canPlace(PrimitiveKind kind) const noexcept
{
    return parent_ || (kind != PrimitiveKind::ModuleGui && kind != PrimitiveKind::ModulePropertiesGui &&
                       kind != PrimitiveKind::TooltipHelp && !isModuleEndpoint(kind) && !isPropertyEndpoint(kind) &&
                       !isModuleWirelessOutput(kind));
}

Schematic* Schematic::module(int id)
{
    const auto found = modules_.find(id);
    return found == modules_.end() ? nullptr : found->second.get();
}

const Schematic* Schematic::module(int id) const
{
    const auto found = modules_.find(id);
    return found == modules_.end() ? nullptr : found->second.get();
}

void Schematic::setModuleName(int id, std::string name)
{
    auto& entry = node(id);
    if (entry.kind != PrimitiveKind::Module) {
        throw std::invalid_argument("Choose a Module");
    }
    entry.moduleName = name.empty() ? "Module" : std::move(name);
}

int Schematic::addNode(PrimitiveKind kind, double x, double y, std::optional<Value> value)
{
    if (!std::isfinite(x) || !std::isfinite(y)) {
        throw std::invalid_argument("Node position must be finite");
    }
    if (!canPlace(kind)) {
        throw std::invalid_argument(std::string(primitive(kind).name) + " can only be placed inside a module");
    }
    const auto& spec = primitive(kind);
    auto initial = initialValue(spec);
    if (value) {
        if (!initial || green::typeOf(*initial) != green::typeOf(*value)) {
            throw std::invalid_argument("Initial value does not match the primitive type");
        }
        if (kind == PrimitiveKind::Settings && !validSystemSetting(std::get<std::int32_t>(*value))) {
            throw std::invalid_argument("Choose a valid system setting");
        }
        if (auto* number = std::get_if<float>(&*value)) {
            *number = green::normalizeFloat(*number);
        }
        initial = std::move(value);
    }
    const int id = nextId_++;
    nodes_.push_back({id, kind, x, y, std::move(initial),
                      std::vector<std::uint64_t>(spec.inputs.size()),
                      std::vector<std::uint64_t>(spec.outputs.size())});
    try {
        if (kind == PrimitiveKind::Module) {
            nodes_.back().moduleName = "Module";
            modules_.emplace(id, std::unique_ptr<Schematic>(new Schematic(bitmapLoader_, this, id)));
        } else if (kind == PrimitiveKind::Ruby) {
            nodes_.back().rubyCode = rubyCode(std::string(rubyStarter), std::string(rubyStarter));
        } else if (isDspEditor(kind)) {
            static const std::string dspStarter = "streamin input;\nstreamout output;\n\noutput = input;";
            static const std::string asmStarter =
                "streamin input;\nstreamout output;\n\nmovaps xmm0, input;\nmovaps output, xmm0;";
            const auto& starter = kind == PrimitiveKind::Assembler ? asmStarter : dspStarter;
            static const auto dspProgram = stream::compileDspCode(dspStarter);
            static const auto asmProgram = stream::compileDspCode(asmStarter, stream::DspLanguage::Assembler);
            const auto& program = kind == PrimitiveKind::Assembler ? asmProgram : dspProgram;
            nodes_.back().dspCode.emplace(DspCodeState{starter, starter, program});
            nodes_.back().inputTriggers.resize(program.inputs().size());
            nodes_.back().outputTriggers.resize(program.outputs().size());
        } else if (kind == PrimitiveKind::AsioIn || kind == PrimitiveKind::AsioOut) {
            auto& entry = nodes_.back();
            const bool input = kind == PrimitiveKind::AsioIn;
            const auto& state = rootGraph().audioControls_;
            const auto selected = input ? state.setup.input : state.setup.output;
            const auto usable = [input](const AudioDeviceInfo& device) {
                return device.api == AudioApi::Asio && (input ? device.inputs : device.outputs) > 0;
            };
            auto device = std::ranges::find_if(state.devices, [&](const AudioDeviceInfo& info) {
                return usable(info) && info.id == selected;
            });
            if (device == state.devices.end()) device = std::ranges::find_if(state.devices, usable);
            if (device != state.devices.end()) {
                const auto channels = std::min(input ? device->inputs : device->outputs, maxAsioChannels);
                entry.channelNames.reserve(channels);
                for (int channel = 1; channel <= channels; ++channel) {
                    entry.channelNames.push_back(std::to_string(channel));
                }
                (input ? entry.outputTriggers : entry.inputTriggers).resize(channels);
            }
        } else if (kind == PrimitiveKind::Tick) {
            nodes_.back().tick.emplace();
        } else if (kind == PrimitiveKind::Time) {
            nodes_.back().localTime = sampleLocalTime();
        } else if (isModuleEndpoint(kind)) {
            const bool input = kind == PrimitiveKind::ModuleInput;
            nodes_.back().label = nextModulePortName(input, id);
        }
        ports_.emplace(id, makePorts(*runtime_, nodes_.back(), resolveTypes(nodes_.back(), {}, {})));
        if (isModuleEndpoint(kind)) {
            updateModulePorts(kind == PrimitiveKind::ModuleInput);
        }
    } catch (...) {
        modules_.erase(id);
        ports_.erase(id);
        nodes_.pop_back();
        throw;
    }
    if (kind == PrimitiveKind::AudioOut || kind == PrimitiveKind::DirectSoundOut || kind == PrimitiveKind::AsioOut) {
        ++rootGraph().audioTopology_;
    }
    return id;
}

void Schematic::moveNode(int id, double x, double y)
{
    if (std::isfinite(x) && std::isfinite(y)) {
        auto& moved = node(id);
        const auto previous = std::pair{moved.x, moved.y};
        moved.x = x;
        moved.y = y;
        if (isModuleEndpoint(moved.kind)) {
            try {
                updateModulePorts(moved.kind == PrimitiveKind::ModuleInput);
            } catch (...) {
                std::tie(moved.x, moved.y) = previous;
                throw;
            }
        }
    }
}

void Schematic::removeNode(int id)
{
    const auto found = std::ranges::find(nodes_, id, &SchematicNode::id);
    if (found == nodes_.end()) return;
    if (isModuleEndpoint(found->kind)) {
        updateModulePorts(found->kind == PrimitiveKind::ModuleInput, id);
    }
    eraseNodes(std::span{&id, 1});
}

void Schematic::eraseNodes(std::span<const int> ids)
{
    const std::set<int> removed(ids.begin(), ids.end());
    bool removingProperties = false;
    if (!std::erase_if(nodes_, [&](const auto& entry) {
            if (!removed.contains(entry.id)) return false;
            removingProperties |= entry.kind == PrimitiveKind::ModulePropertiesGui;
            clearRubyBranch(entry);
            return true;
        })) return;

    if (removingProperties && std::ranges::none_of(nodes_, [](const auto& entry) {
            return entry.kind == PrimitiveKind::ModulePropertiesGui;
        })) {
        parent_->node(path_.back()).propertiesVisibility = PropertiesVisibility::Closed;
    }
    for (int id : removed) {
        modules_.erase(id);
        wirelessRoutes_.erase(id);
    }
    const auto incident = [&](SchematicLink link) {
        return removed.contains(link.source) || removed.contains(link.target);
    };
    std::erase_if(links_, [&](auto link) {
        if (removed.contains(link.target)) return true;
        if (!removed.contains(link.source)) return false;
        node(link.target).memoryCache = {};
        return true;
    });
    std::erase_if(linkTriggers_, [&](const auto& entry) { return incident(entry.first); });
    ++rootGraph().drawingTopology_;
    rebuildAfterRemoval();
    refreshConnections();
    rootGraph().primeStateMachines();
}

void Schematic::validateLink(SchematicLink link) const
{
    const auto& inputs = ports_.at(link.target).inputs;
    if (link.input < 0 || static_cast<std::size_t>(link.input) >= inputs.size()) {
        throw std::invalid_argument("Choose an input connector");
    }
    const auto& outputs = ports_.at(link.source).outputs;
    if (link.output < 0 || static_cast<std::size_t>(link.output) >= outputs.size()) {
        throw std::invalid_argument("Choose an output connector");
    }
    if (wouldCreateFeedback(link)) {
        throw std::invalid_argument("This link would create a feedback loop");
    }
    if (linkTriggers_.contains(link)) {
        throw std::invalid_argument("Green link already exists");
    }
}

void Schematic::checkConnection(SchematicLink link)
{
    validateLink(link);
    links_.push_back(link);
    try {
        rebuildConnectedRuntimes(false);
    } catch (...) {
        links_.pop_back();
        throw;
    }
    links_.pop_back();
}

bool Schematic::canSuggestLink(SchematicLink link) const
{
    try {
        validateLink(link);
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

void Schematic::connect(SchematicLink link, LinkNotification notification)
{
    validateLink(link);
    const auto [count, inserted] = linkTriggers_.emplace(link, 0);
    if (!inserted) {
        throw std::invalid_argument("Green link already exists");
    }
    try {
        links_.push_back(link);
    } catch (...) {
        linkTriggers_.erase(count);
        throw;
    }
    try {
        rebuildConnectedRuntimes();
    } catch (...) {
        links_.pop_back();
        linkTriggers_.erase(count);
        throw;
    }
    ++rootGraph().drawingTopology_;
    const auto sourceKind = node(link.source).kind;
    auto& destination = node(link.target);
    destination.memoryCache = {};
    // Commands wait for a pulse when connecting a wire would deliver an initial value.
    const auto sourceType = outputType(link.source, link.output);
    const auto destinationBehavior = primitive(destination.kind).behavior;
    const bool command = (destinationBehavior == FileUtility || destinationBehavior == SerialDevice ||
                          destinationBehavior == PresentationControl || destinationBehavior == KeyboardInput ||
                          destinationBehavior == PersistSettings || destinationBehavior == StateMachineLogic ||
                          destinationBehavior == SystemAction || destinationBehavior == HttpTransfer ||
                          destinationBehavior == NetworkTransfer || destinationBehavior == ParseSfz ||
                          destinationBehavior == MouseCaptureControl || destinationBehavior == MediaSource ||
                          destinationBehavior == ParseColorMatrix || destinationBehavior == EmbedFonts || destinationBehavior == RubyCode ||
                          destination.kind == PrimitiveKind::BasicCounter ||
                          destination.kind == PrimitiveKind::RedrawLimiter) &&
        inputType(link.target, link.input) == Type::Trigger;
    const bool eventOnly = sourceKind == PrimitiveKind::AfterLoad || sourceKind == PrimitiveKind::AfterDuplicate ||
        sourceKind == PrimitiveKind::AboutToClose || (sourceKind == PrimitiveKind::IsPlaying && link.output == 1) ||
        (sourceKind == PrimitiveKind::Ruby && sourceType == Type::Trigger);
    if (!command && !eventOnly && notification == LinkNotification::Deliver &&
        (!sourceType || !green::deliversDiscreteEvents(*sourceType))) {
        const auto source = ports_.at(link.source).outputs[link.output];
        const auto target = ports_.at(link.target).inputs[link.input];
        if (source && target) {
            runtime_->fireConnection(*source, *target);
        }
    }
    // A receiver's first local connection already delivered its initial value.
    const bool firstReceiverLink = primitive(sourceKind).behavior == ReceiveWireless &&
        std::ranges::count(links_, link.source, &SchematicLink::source) == 1;
    refreshConnections(notification == LinkNotification::Deliver,
                       firstReceiverLink ? std::optional{link.source} : std::nullopt);
}

void Schematic::eraseLinks(const std::function<bool(SchematicLink)>& matches)
{
    if (std::erase_if(links_, [&](auto link) {
        if (!matches(link)) return false;
        node(link.target).memoryCache = {};
        return true;
    })) {
        std::erase_if(linkTriggers_, [&](const auto& entry) { return matches(entry.first); });
        ++rootGraph().drawingTopology_;
        rebuildAfterRemoval();
        refreshConnections();
        rootGraph().primeStateMachines();
    }
}

void Schematic::disconnectInput(int id, int input)
{
    eraseLinks([=](auto link) { return link.target == id && link.input == input; });
}

void Schematic::disconnectLink(SchematicLink link)
{
    eraseLinks([link](auto candidate) { return candidate == link; });
}

void Schematic::disconnectNode(int id)
{
    eraseLinks([=](auto link) { return link.target == id || link.source == id; });
}

namespace {

std::optional<Type> noteInputForOutput(PrimitiveKind kind, Type output)
{
    if (kind == PrimitiveKind::IntToNoteName) {
        if (output == Type::String) return Type::Int;
        if (output == Type::StringArray) return Type::IntArray;
    } else {
        if (output == Type::Int) return Type::String;
        if (output == Type::IntArray) return Type::StringArray;
    }
    return {};
}

}

bool Schematic::supportsTemplateType(PrimitiveKind kind, Type type)
{
    if (kind == PrimitiveKind::StreamToGreen) {
        return type == Type::Float || type == Type::Boolean;
    }
    if (isPropertyEndpoint(kind)) {
        return type == Type::Trigger || (green::isValueType(type) && type != Type::View && type != Type::Bus);
    }
    if (isModuleEndpoint(kind)) {
        return type == Type::Trigger || green::isValueType(type) || green::isDspType(type);
    }
    if (kind == PrimitiveKind::Matrix) {
        return type == Type::Trigger || (green::isDspType(type) && type != Type::PolyInt) ||
               (green::isValueType(type) && type != Type::View && type != Type::Bus);
    }
    const auto& spec = primitive(kind);
    if (spec.behavior == SendWireless || spec.behavior == ReceiveWireless) {
        return type != Type::Bus && (type == Type::Trigger || green::isValueType(type));
    }
    if (spec.behavior == Blocker || spec.behavior == Switch) {
        // Views are routed through front panels.
        return (green::isValueType(type) && type != Type::View && type != Type::Bus) ||
               (spec.behavior == Switch && type == Type::Trigger);
    }
    if (spec.behavior == GreenLogic) {
        if (isIndexedRouter(kind)) {
            return type == Type::Trigger || green::isValueType(type) || green::isDspType(type);
        }
        if (isBitwiseBinary(kind)) {
            return type == Type::Int || type == Type::Boolean;
        }
        return (type >= Type::Float && type <= Type::PointArray && type != Type::Trigger) ||
               type == Type::Area || type == Type::Color;
    }
    const bool scalarNumber = type == Type::Int || type == Type::Float;
    if (kind == PrimitiveKind::ViewLoop || kind == PrimitiveKind::Loop) {
        return scalarNumber;
    }
    if (kind == PrimitiveKind::ArrayBuilder) {
        return arrayForElement(type).has_value();
    }
    if (hasGenericArrayInput(kind) || kind == PrimitiveKind::SortArray) return isArray(type);
    if (kind == PrimitiveKind::ArrayLength) return type == Type::String || isArray(type);
    if (kind == PrimitiveKind::ViewArrayLoop || spec.behavior == GetArrayElement ||
        spec.behavior == AppendArrays || spec.behavior == TransformArray || spec.behavior == StoreArray) {
        return isArray(type);
    }
    if (supportsStreamMath(spec) && numericStreamFamily(type)) {
        return true;
    }
    if (spec.behavior == NoteConversion) {
        return kind == PrimitiveKind::IntToNoteName ? (type == Type::Int || type == Type::IntArray)
                                                    : (type == Type::String || type == Type::StringArray);
    }
    if (spec.pitchConversion || kind == PrimitiveKind::FromDecibel || kind == PrimitiveKind::ToDecibel ||
        kind == PrimitiveKind::Hypotenuse || kind == PrimitiveKind::Reciprocal || kind == PrimitiveKind::Sign ||
        kind == PrimitiveKind::Inverse) {
        return type == Type::Float || type == Type::FloatArray ||
               ((kind == PrimitiveKind::Sign || kind == PrimitiveKind::Inverse) &&
                (type == Type::Int || type == Type::IntArray));
    }
    if (spec.behavior == Compare) {
        return scalarNumber || type == Type::String || isNumericComparisonArray(type) || type == Type::StringArray;
    }
    if (spec.operation == ArithmeticOperation::Modulus) {
        return scalarNumber;
    }
    if (spec.behavior == Binary || spec.behavior == UnaryMath) {
        return !spec.inputs.front().type &&
               (scalarNumber || type == Type::Point || type == Type::IntArray ||
                type == Type::FloatArray || type == Type::PointArray ||
                (spec.operation == ArithmeticOperation::Add && (type == Type::String || type == Type::StringArray)));
    }
    return false;
}

void Schematic::checkTemplateType(int id, std::optional<Type> type)
{
    auto& entry = node(id);
    if (!primitive(entry.kind).hasTemplateConnectors() || (type && !supportsTemplateType(entry.kind, *type))) {
        throw std::invalid_argument("This primitive does not support that connector type");
    }
    if (type == entry.typeOverride) {
        return;
    }
    const auto previous = std::exchange(entry.typeOverride, type);
    try {
        rebuildConnectedRuntimes(false);
    } catch (...) {
        entry.typeOverride = previous;
        throw;
    }
    entry.typeOverride = previous;
}

void Schematic::setTemplateType(int id, std::optional<Type> type)
{
    auto& entry = node(id);
    if (isModuleEndpoint(entry.kind)) {
        setModulePortType(id, type);
        return;
    }
    if (!primitive(entry.kind).hasTemplateConnectors() || (type && !supportsTemplateType(entry.kind, *type))) {
        throw std::invalid_argument("This primitive does not support that connector type");
    }
    if (entry.typeOverride == type) {
        return;
    }
    const auto previous = entry.typeOverride;
    entry.typeOverride = type;
    try {
        rebuildConnectedRuntimes();
    } catch (...) {
        entry.typeOverride = previous;
        throw;
    }
    refreshConnections();
}

std::optional<Type> Schematic::inputType(int id, int input) const
{
    return ports_.at(id).types.inputs.at(input);
}

std::optional<Type> Schematic::outputType(int id, int output) const
{
    return ports_.at(id).types.outputs.at(output);
}

bool Schematic::editable(int id) const
{
    const auto& entry = node(id);
    return primitive(entry.kind).behavior == ValueBox &&
           std::ranges::none_of(links_, [this, id](auto link) {
               if (link.target != id) {
                   return false;
               }
               const auto type = outputType(link.source, link.output);
               return type && *type != Type::Trigger;
           });
}

Value Schematic::readValue(int id, int output) const
{
    const auto& entry = node(id);
    const auto& spec = primitive(entry.kind);
    const auto type = outputType(id, output);
    if (!type || !green::isValueType(*type)) {
        throw std::invalid_argument("This connector has no value type");
    }
    if (entry.codeSourcePort(true) == output) {
        return entry.dspCode ? entry.dspCode->source : entry.rubyCode->source;
    }
    switch (spec.behavior) {
    case RubyCode:
        if (const auto& value = entry.rubyCode->outputs.at(output)) return *value;
        return defaultValue(*type);
    case ReceiveWireless:
        if (auto value = readWireless(id, *type)) {
            return std::move(*value);
        }
        return defaultValue(*type);
    case UnaryMath:
        if (spec.pitchConversion) {
            return green::calculate(*spec.pitchConversion, readInput(id, 0), static_cast<float>(sampleRate()));
        }
        return green::calculate(spec.unaryOperation.value(), readInput(id, 0));
    case NoteConversion:
        return readNoteConversion(entry);
    case GeneralProcessing:
        return readGeneralProcessing(entry, output);
    case WaveFile:
        return readWaveFile(entry, output);
    case Compare:
        return compareInputs(entry);
    case GreenLogic:
        return readGreenLogic(entry, *type, output);
    case MatrixRouting:
        return readMatrix(entry, *type, output);
    case BuildArray:
        return withGreenArray(*type, [&]<class Array>() -> Value {
            Array result;
            const auto count = entry.inputTriggers.size() - 1;
            result.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                result.push_back(std::get<typename Array::value_type>(readInput(id, static_cast<int>(i))));
            }
            return result;
        });
    case ProcessArray:
        return readArrayTool(entry, output);
    case StoreArray:
        return readStoredArray(entry, output, *type);
    case GetArrayElement:
    case TransformArray:
        return entry.localValue ? *entry.localValue : defaultValue(*type);
    case AppendArrays:
        return withGreenArray(*type, [&]<class Array>() -> Value {
            auto first = std::get<Array>(readInput(id, 0));
            auto second = std::get<Array>(readInput(id, 1));
            first.insert(first.end(), std::make_move_iterator(second.begin()), std::make_move_iterator(second.end()));
            return first;
        });
    case MakePoint:
        return green::Point{std::get<float>(readInput(id, 0)), std::get<float>(readInput(id, 1))};
    case SplitPoint: {
        const auto point = std::get<green::Point>(readInput(id, 0));
        return output == 0 ? point.x : point.y;
    }
    case IndexSelector:
        return readIndexSelector(entry, output);
    case Window:
        if (output == 0) {
            return windowView(entry);
        }
        return entry.window && entry.window->visible;
    case ModuleGui: {
        return panelView(entry, moduleParentView(entry));
    }
    case ViewLoop:
        if (output == 0) {
            return readViewLoop(entry, output, readInput(id, 0));
        }
        return readViewLoop(entry, output, defaultValue(*type));
    case ViewCache:
        return readDrawBoundary(entry, yellow::DrawBoundaryKind::Cache);
    case ViewEffect:
        return readDrawBoundary(entry, yellow::DrawBoundaryKind::Effect);
    case TransformView:
        return transformView(entry);
    case ViewAttributes:
        return readViewAttribute(entry, output);
    case PanelUtility:
        return readPanelUtility(entry);
    case PresentationControl:
        return readPresentationControl(entry.kind);
    case KeyboardInput:
        if (entry.kind == PrimitiveKind::IsKeyPressed) {
            return isKeyPressed(std::get<std::string>(readInput(id, 0)));
        }
        return entry.capturedSignals.empty() ? defaultValue(*type) : entry.capturedSignals.at(output);
    case SetGuiArea:
    case Redraw:
    case DrawView:
        return readInput(id, 0);
    case DropList:
        return readDropList(entry, output);
    case EditControl:
        return readEditControl(entry, output);
    case PopupList:
        return readPopupList(entry, output);
    case DropFiles:
        return readDropFiles(entry, output);
    case DragFiles:
        return readDragFiles(entry, output);
    case MouseCaptureControl:
    case MouseArea:
        return entry.lastMouseMessage;
    case ParseMouseMessage:
        if (entry.kind == PrimitiveKind::MouseWheel) {
            return static_cast<float>(entry.lastMouseMessage.wheelDelta.y);
        }
        return entry.lastMouseMessage.position;
    case MouseMove:
        return entry.lastMouseMessage.position;
    case MouseDrag:
        return readMouseDrag(entry, output);
    case AreaGeometry:
        return readAreaGeometry(entry, output);
    case ProcessString:
        return readStringOperation(entry, output);
    case FileUtility:
        return readFileUtility(entry, output);
    case SystemAction:
        return entry.capturedSignals.empty() ? Value{std::string{}} : entry.capturedSignals.front();
    case MediaSource:
        return readMedia(entry, output);
    case NetworkTransfer:
        return readNetwork(entry, output);
    case ParseSfz:
        return readSfz(entry, output);
    case HttpTransfer:
        return readHttpResponse(entry, output);
    case ParseColorMatrix:
        return readParsedColorMatrix(entry, output);
    case PresetUtility:
        if (entry.kind == PrimitiveKind::PresetLoad && output >= 3) return entry.legacyPresetInfo.at(output - 3);
        [[fallthrough]];
    case TextFile:
        return entry.localValue ? *entry.localValue : Value{std::string{}};
    case SystemInfo:
        return readSystemInfo(entry, output);
    case StateMachineLogic:
        return readStateMachine(entry, output);
    case SystemSettings:
        return readSystemSetting(entry);
    case PersistSettings:
        return entry.localValue.value_or(Value{std::string{}});
    case MemoryOperation:
        return readMemory(entry, output);
    case SerialDevice:
        return readSerialDevice(entry, output);
    case MidiDevice:
        return readMidiDevice(entry, output);
    case VoiceRouting:
        return readVoiceRouting(entry);
    case ManagePresets:
        return readPresetManager(entry, output);
    case PresetParameter:
        return readPresetParameter(entry, output);
    case PluginHost:
        return readPluginHost(entry, output);
    case NotesFilter:
    case SequencerTrack:
    case HostMidi:
        return entry.midi && entry.midi->values.at(output) ? *entry.midi->values.at(output) : defaultValue(*type);
    case MidiOperation:
        if (entry.midi && entry.midi->values.at(output)) return *entry.midi->values.at(output);
        if (entry.kind == PrimitiveKind::MidiPitchBend) return std::int32_t{8192};
        return defaultValue(*type);
    case SampleTime:
        if (!entry.localTime) entry.localTime = sampleLocalTime();
        return entry.localTime->at(output);
    case MeasureText:
        return measureText(id);
    case SplitColor: {
        const auto color = std::get<yellow::Color>(readInput(id, 0));
        const auto channels = entry.kind == PrimitiveKind::ColorToArgb
            ? yellow::colorToArgb(color) : yellow::colorToAhsv(color);
        return channels.at(output);
    }
    case MakeColor: {
        const auto channel = [&](int input) { return std::get<std::int32_t>(readInput(id, input)); };
        const yellow::ColorChannels channels{channel(0), channel(1), channel(2), channel(3)};
        return entry.kind == PrimitiveKind::ArgbToColor
            ? yellow::colorFromArgb(channels) : yellow::colorFromAhsv(channels);
    }
    case ColorMatrixSet: {
        auto matrix = std::get<yellow::ColorMatrix>(readInput(id, 0));
        const auto row = std::get<std::int32_t>(readInput(id, 1));
        const auto column = std::get<std::int32_t>(readInput(id, 2));
        const auto value = std::get<float>(readInput(id, 3));
        if (row >= 0 && row < yellow::ColorMatrix::size && column >= 0 && column < yellow::ColorMatrix::size &&
            std::isfinite(value)) {
            matrix.coefficients[row][column] = value;
        }
        return matrix;
    }
    case ApplyColorMatrix:
        return yellow::applyColorMatrix(std::get<yellow::ColorMatrix>(readInput(id, 1)),
                                        std::get<yellow::Color>(readInput(id, 0)));
    case MakePen: {
        yellow::PenStroke pen;
        pen.color = std::get<yellow::Color>(readInput(id, 0));
        const auto width = std::get<float>(readInput(id, 1));
        if (std::isfinite(width) && width >= 0.0F) {
            pen.width = width;
        }
        pen.style = yellow::parseLineStyle(std::get<std::string>(readInput(id, 2)));
        return yellow::PaintTool{pen};
    }
    case PenInfo: {
        const auto tool = std::get<yellow::PaintTool>(readInput(id, 0));
        const auto* stroke = std::get_if<yellow::PenStroke>(&tool);
        const auto pen = stroke ? *stroke : yellow::PenStroke{};
        if (output == 0) return pen.color;
        if (output == 1) return pen.width;
        return std::string(yellow::lineStyleName(pen.style));
    }
    case MakeSolid:
        return yellow::PaintTool{yellow::SolidBrush{std::get<yellow::Color>(readInput(id, 0))}};
    case MakeBrush:
        return readBrush(entry);
    case MakeFont: {
        auto font = entry.localValue ? std::get<yellow::Font>(*entry.localValue) : yellow::Font{};
        auto face = std::get<std::string>(readInput(id, 0));
        if (!face.empty()) {
            font.face = std::move(face);
        }
        const auto size = std::get<float>(readInput(id, 1));
        if (std::isfinite(size) && size > 0.0F) {
            font.size = size;
        }
        font.style = yellow::parseFontStyle(std::get<std::string>(readInput(id, 2)));
        font.alternateFace = std::get<std::string>(readInput(id, 3));
        return font;
    }
    case EmbedFonts:
        return entry.capturedSignals.empty() ? Value{bool(entry.embeddedFonts.registration)} : entry.capturedSignals.front();
    case FontAttributes: {
        auto font = std::get<yellow::Font>(readInput(id, 0));
        switch (output) {
        case 0: return std::move(font.face);
        case 1: return font.size;
        case 2: return yellow::fontStyleName(font.style);
        }
        throw std::logic_error("Unknown FontAttr output");
    }
    case MakeStringFormat:
        return yellow::parseStringFormat(std::get<std::string>(readInput(id, 0)),
                                         std::get<std::string>(readInput(id, 1)),
                                         std::get<std::string>(readInput(id, 2)));
    case StringFormatAttributes: {
        const auto format = std::get<yellow::StringFormat>(readInput(id, 0));
        switch (output) {
        case 0: return yellow::stringFormatStyleName(format);
        case 1: return std::string(yellow::alignmentName(format.horizontal));
        case 2: return std::string(yellow::alignmentName(format.vertical));
        }
        throw std::logic_error("Unknown StrFmtAttr output");
    }
    case LoadBitmap: {
        const auto& bitmap = std::get<yellow::Bitmap>(*entry.localValue);
        switch (output) {
        case 0: return bitmap;
        case 1: return bitmap.width();
        case 2: return bitmap.height();
        }
        throw std::logic_error("Unknown Bitmap output");
    }
    case BitmapCanvas:
        return output == 1 ? Value{bitmapView(entry)} : *entry.localValue;
    case BitmapMetadata:
        return readBitmapMetadata(entry, output);
    case BitmapOperation:
    case LoadBitmapArray:
        return *entry.localValue;
    case CreateBus: {
        green::Bus bus;
        for (std::size_t i = 0; i < entry.channelNames.size(); ++i) {
            bus.channels.push_back({entry.channelNames[i], inputType(id, firstBusChannelInput + static_cast<int>(i))});
        }
        return bus;
    }
    case ExtractBus: {
        if (const auto source = findBusSource(id)) {
            const int input = busChannelInput(*source, entry.channelNames.at(output));
            if (input >= 1 && source->graph->node(source->id).kind == PrimitiveKind::SfzParser) {
                return source->graph->readSfzColumn(source->graph->node(source->id), input - 1);
            }
            if (input >= 0 && source->graph->inputType(source->id, input) == type) {
                return source->graph->readInput(source->id, input);
            }
        }
        return defaultValue(*type);
    }
    case ModuleInput:
        return parent_->readInput(path_.back(), entry.boundaryIndex);
    case PropertyInput:
        if (const auto* boundary = propertyBoundary(entry); boundary && boundary->boundaryType == type) {
            return readInput(boundary->id, 0);
        }
        return defaultValue(*type);
    case Module: {
        const auto* child = module(id);
        return child->readInput(entry.moduleOutputs.at(output).endpoint, 0);
    }
    case Binary: {
        auto left = readInput(id, 0);
        const auto right = readInput(id, 1);
        if (*type == Type::String) {
            return std::get<std::string>(std::move(left)) + std::get<std::string>(right);
        }
        const auto operation = spec.operation.value();
        if (isArray(*type)) {
            return green::applyArrayOperation(operation, left, right);
        }
        if (*type == Type::Point) {
            const auto a = std::get<green::Point>(left);
            const auto b = std::get<green::Point>(right);
            return green::Point{green::calculate(operation, a.x, b.x),
                                green::calculate(operation, a.y, b.y)};
        }
        if (*type == Type::Int) {
            return green::calculate(operation, std::get<std::int32_t>(left), std::get<std::int32_t>(right));
        }
        const auto number = [](const Value& operand) {
            if (auto integer = std::get_if<std::int32_t>(&operand)) {
                return static_cast<float>(*integer);
            }
            return std::get<float>(operand);
        };
        return green::normalizeFloat(green::calculate(operation, number(left), number(right)));
    }
    case SampleMono:
        return entry.localValue.value_or(defaultValue(*type));
    case Counter:
        return *entry.localValue;
    case CaptureSignal:
        return entry.capturedSignals.empty() ? defaultValue(*type) : entry.capturedSignals.at(output);
    case ProcessTrigger:
        if (entry.kind == PrimitiveKind::Loop && entry.greenLoopValue) {
            return output == 0 ? entry.greenLoopValue->value : Value{entry.greenLoopValue->index};
        }
        return entry.localValue ? *entry.localValue : defaultValue(*type);
    case ValueBox:
    case Blocker:
    case Switch:
        return readInput(id, 0);
    case AudioControl:
        return readAudioControl(entry, output);
    case TriggerButton:
    case Tick:
    case Timer:
    case AudioEndpoint:
    case SendWireless:
    case ModuleOutput:
    case PropertyOutput:
    case MouseInfo:
    case DspCode:
    case PolyBridge:
    case StreamMath:
    case StreamConversion:
    case StreamRouting:
    case MidiStream:
    case VoiceSignal:
    case ShowCursor:
        throw std::logic_error("This node has no value output");
    }
    throw std::logic_error("Unknown primitive behavior");
}

bool Schematic::compareInputs(const SchematicNode& entry) const
{
    if (!inputType(entry.id, 0)) {
        return false;
    }
    const auto left = readInput(entry.id, 0);
    const auto right = readInput(entry.id, 1);
    const auto matches = [&](std::partial_ordering order) {
        switch (entry.kind) {
        case PrimitiveKind::Equal:
        case PrimitiveKind::EqualTrigger: return order == 0;
        case PrimitiveKind::LessThan:
        case PrimitiveKind::LessThanTrigger: return order < 0;
        case PrimitiveKind::LessThanOrEqual:
        case PrimitiveKind::LessThanOrEqualTrigger: return order <= 0;
        case PrimitiveKind::GreaterThan:
        case PrimitiveKind::GreaterThanTrigger: return order > 0;
        case PrimitiveKind::GreaterThanOrEqual:
        case PrimitiveKind::GreaterThanOrEqualTrigger: return order >= 0;
        default: throw std::logic_error("Unknown comparison primitive");
        }
    };
    if (entry.compareArrayElements &&
        (isNumericComparisonArray(green::typeOf(left)) || green::typeOf(left) == Type::StringArray)) {
        const auto size = [](const Value& value) {
            if (const auto* array = std::get_if<green::IntArray>(&value)) return array->size();
            if (const auto* array = std::get_if<green::FloatArray>(&value)) return array->size();
            return std::get<green::StringArray>(value).size();
        };
        const auto element = [](const Value& value, std::size_t i) -> Value {
            if (const auto* array = std::get_if<green::IntArray>(&value)) return (*array)[i];
            if (const auto* array = std::get_if<green::FloatArray>(&value)) return (*array)[i];
            return std::get<green::StringArray>(value)[i];
        };
        const auto count = size(left);
        if (count != size(right)) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto order = comparisonOrder(element(left, i), element(right, i));
            // Native array helpers reject a failing ordered pair; an unordered
            // float pair therefore passes relational predicates, but not equality.
            if (order == std::partial_ordering::unordered &&
                entry.kind != PrimitiveKind::Equal && entry.kind != PrimitiveKind::EqualTrigger) continue;
            if (!matches(order)) return false;
        }
        return true;
    }
    return matches(comparisonOrder(left, right));
}

int Schematic::routerSelection(const SchematicNode& entry) const
{
    const auto selected = std::get<std::int32_t>(readInput(entry.id, 0));
    const auto count = entry.kind == PrimitiveKind::Selector ? entry.inputTriggers.size() - 1 : entry.outputTriggers.size();
    if (entry.kind == PrimitiveKind::Multiplexer && entry.flowStoneRoutingBranches)
        return std::clamp(selected, 0, std::max(0, int(count) - 2));
    return selected >= 0 && std::cmp_less(selected, count) ? selected : -1;
}

std::optional<int> Schematic::routedDataInput(PrimitiveKind kind, int selected, int output)
{
    if (selected < 0) return std::nullopt;
    if (kind == PrimitiveKind::Selector) return selected + 1;
    return selected == output ? std::optional{1} : std::nullopt;
}

Value Schematic::readGreenLogic(const SchematicNode& entry, Type type, int output) const
{
    using enum PrimitiveKind;
    const int id = entry.id;
    switch (entry.kind) {
    case IfThenElse:
        return readIfThenElse(id);
    case Select:
        return readInput(id, std::get<bool>(readInput(id, 2)) ? 1 : 0);
    case Selector: case Multiplexer: {
        const auto input = routedDataInput(entry.kind, routerSelection(entry), output);
        return input ? readInput(id, *input) : defaultValue(type);
    }
    case LastSwitch: {
        if (entry.flowStoneRoutingBranches) return readInput(id, entry.lastSwitchInput);
        const auto linked = [&](int port) {
            return std::ranges::any_of(links_, [=](auto link) { return link.target == id && link.input == port; });
        };
        const int last = entry.lastSwitchInput;
        return readInput(id, linked(last) ? last : 1 - last);
    }
    case Changed:
        return entry.localValue ? *entry.localValue : readInput(id, 0);
    case SampleAndHold:
        return entry.localValue ? *entry.localValue : defaultValue(type);
    case Not:
        return !std::get<bool>(readInput(id, 0));
    case IntNot:
        return ~std::get<std::int32_t>(readInput(id, 0));
    case ShiftLeft: case ShiftRight: {
        const auto value = static_cast<std::uint32_t>(std::get<std::int32_t>(readInput(id, 0)));
        const auto bits = std::get<std::int32_t>(readInput(id, 1));
        if (bits < 0 || bits >= 32) {
            return std::int32_t{0};
        }
        return static_cast<std::int32_t>(entry.kind == ShiftLeft ? value << bits : value >> bits);
    }
    case And: case Or: case Xor: {
        const auto a = readInput(id, 0);
        const auto b = readInput(id, 1);
        const auto apply = [&](auto left, auto right) {
            if (entry.kind == And) return left & right;
            if (entry.kind == Or) return left | right;
            return left ^ right;
        };
        if (type == Type::Boolean) {
            return apply(std::get<bool>(a), std::get<bool>(b)) != 0;
        }
        return apply(std::get<std::int32_t>(a), std::get<std::int32_t>(b));
    }
    default:
        throw std::logic_error("This Green logic primitive has no value output");
    }
}

std::string Schematic::readIfThenElse(int id) const
{
    const auto mode = std::get<std::string>(readInput(id, 0));
    auto control = readInput(id, 1);
    const auto op = std::get<std::string>(readInput(id, 2));
    const auto conditions = std::get<green::StringArray>(readInput(id, 3));
    auto results = std::get<green::StringArray>(readInput(id, 4));
    const auto otherwise = [&] { return std::get<std::string>(readInput(id, 5)); };

    const auto type = mode == "int" ? Type::Int : mode == "float" ? Type::Float : Type::String;
    const auto convert = green::findValueConverter(Type::String, type);
    if (convert) {
        try {
            control = convert(control);
        } catch (const std::out_of_range&) {
            return otherwise();
        }
    }

    const auto matches = [&](std::partial_ordering order) {
        if (op.empty() || op == "=" || op == "==") return order == 0;
        if (op == "<") return order < 0;
        if (op == "<=") return order <= 0;
        if (op == ">") return order > 0;
        if (op == ">=") return order >= 0;
        return false;
    };
    const auto count = std::min(conditions.size(), results.size());
    for (std::size_t index = 0; index < count; ++index) {
        Value condition = conditions[index];
        try {
            if (convert) condition = convert(condition);
        } catch (const std::out_of_range&) {
            continue;
        }
        if (matches(comparisonOrder(control, condition))) {
            return std::move(results[index]);
        }
    }
    return otherwise();
}

void Schematic::receiveGreenLogicTrigger(SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    const int id = entry.id;
    if (!outputType(id)) {
        return;
    }

    switch (entry.kind) {
    case BoolToFalseTrigger: case BoolToTrueTrigger: {
        const bool current = std::get<bool>(readInput(id, 0));
        const bool previous = entry.localValue && std::get<bool>(*entry.localValue);
        entry.localValue = current;
        if (current == previous || current != (entry.kind == BoolToTrueTrigger)) return;
        break;
    }
    case Changed: {
        auto value = readInput(id, 0);
        if (entry.localValue == value) return;
        entry.localValue = std::move(value);
        break;
    }
    case SampleAndHold:
        if (input != 1) return;
        entry.localValue = readInput(id, 0);
        break;
    case Select: {
        const bool selected = std::get<bool>(readInput(id, 2));
        if (input == 2) {
            const bool previous = entry.localValue && std::get<bool>(*entry.localValue);
            entry.localValue = selected;
            if (selected == previous) return;
        } else if (input != (selected ? 1 : 0)) return;
        break;
    }
    case Selector:
        if (input == 0) {
            const auto type = outputType(id);
            if (type && (*type == Type::Trigger || green::deliversDiscreteEvents(*type))) return;
        } else if (input - 1 != std::get<std::int32_t>(readInput(id, 0))) {
            return;
        }
        break;
    case Multiplexer: {
        const auto selected = entry.flowStoneRoutingBranches ? routerSelection(entry) : std::get<std::int32_t>(readInput(id, 0));
        const auto publish = [&](int branch) {
            if (branch >= 0 && std::cmp_less(branch, entry.outputTriggers.size())) fireOutput(entry, branch);
        };
        if (input == 0) {
            const auto previous = std::exchange(entry.localValue, selected);
            const auto oldBranch = previous ? std::get<std::int32_t>(*previous) : 0;
            const auto type = outputType(id);
            if (selected == oldBranch || (!entry.flowStoneRoutingBranches && type &&
                (*type == Type::Trigger || green::deliversDiscreteEvents(*type)))) return;
            if (!entry.flowStoneRoutingBranches) publish(oldBranch);
        }
        publish(selected);
        return;
    }
    case IntTransition: {
        if (input != 0) return;
        const auto current = std::get<std::int32_t>(readInput(id, 0));
        const auto previous = std::exchange(entry.localValue, current);
        if (!previous) return;
        const auto last = std::get<std::int32_t>(*previous);
        if (last == current || last != std::get<std::int32_t>(readInput(id, 1)) ||
            current != std::get<std::int32_t>(readInput(id, 2))) return;
        break;
    }
    case LastSwitch:
        entry.lastSwitchInput = input;
        break;
    default:
        break;
    }
    fireOutput(entry, 0);
}

std::vector<MatrixConnection> Schematic::matrixConnections(const SchematicNode& entry) const
{
    const auto parseIndex = [](std::string_view token) -> int {
        constexpr auto whitespace = " \t\r\n";
        const auto begin = token.find_first_not_of(whitespace);
        if (begin == token.npos) return -1;
        token = token.substr(begin, token.find_last_not_of(whitespace) - begin + 1);
        if (token.starts_with('+')) token.remove_prefix(1);
        int index;
        const auto end = token.data() + token.size();
        const auto [next, error] = std::from_chars(token.data(), end, index);
        return error == std::errc{} && next == end ? index : -1;
    };

    const auto value = readInput(entry.id, 0);
    auto fields = std::string_view{std::get<std::string>(value)} | std::views::split(',');
    std::vector<MatrixConnection> connections;
    const auto signalInputs = std::ssize(entry.inputTriggers) - 2;
    const auto signalOutputs = std::ssize(entry.outputTriggers) - 1;
    auto field = fields.begin();
    while (field != fields.end()) {
        const auto input = parseIndex(std::string_view{*field});
        if (++field == fields.end()) break;
        const auto output = parseIndex(std::string_view{*field});
        ++field;
        if (input >= 0 && input < signalInputs && output >= 0 && output < signalOutputs) {
            connections.push_back({input + 1, output});
        }
    }
    return connections;
}

Value Schematic::readMatrix(const SchematicNode& entry, Type type, int output) const
{
    const auto connections = matrixConnections(entry);
    if (green::deliversDiscreteEvents(type) && entry.matrix &&
        output < std::ssize(entry.matrix->lastEventInputs)) {
        const int last = entry.matrix->lastEventInputs[output];
        if (last > 0 && std::ranges::contains(connections, MatrixConnection{last, output}) &&
            hasValueInput(entry.id, last)) {
            return readInput(entry.id, last);
        }
    }

    std::optional<Value> value;
    for (const auto& connection : connections) {
        if (connection.output != output || !hasValueInput(entry.id, connection.input)) continue;
        auto input = readInput(entry.id, connection.input);
        if (green::hasSingleValueSource(type)) return input;
        if (value) green::combineInputValue(*value, input);
        else value = std::move(input);
    }
    return value ? std::move(*value) : defaultValue(type);
}

void Schematic::receiveMatrixTrigger(SchematicNode& entry, int input)
{
    if (input == 0) {
        refreshMatrix(entry, true);
        return;
    }
    const auto type = inputType(entry.id, input);
    const bool event = type && green::deliversDiscreteEvents(*type);
    for (const auto& connection : matrixConnections(entry)) {
        if (connection.input != input) continue;
        if (event) {
            if (!entry.matrix) entry.matrix.emplace();
            entry.matrix->lastEventInputs.resize(entry.outputTriggers.size());
            entry.matrix->lastEventInputs[connection.output] = input;
        }
        fireOutput(entry, connection.output);
    }
}

void Schematic::refreshMatrix(SchematicNode& entry, bool notify)
{
    auto connections = matrixConnections(entry);
    if (!entry.matrix) entry.matrix.emplace();
    if (connections == entry.matrix->connections) return;
    entry.matrix->connections = std::move(connections);
    entry.matrix->lastEventInputs.clear();
    ++entry.matrix->revision;
    const auto type = outputType(entry.id);
    if (!type || !notify) return;
    if (green::isDspType(*type)) {
        auto& root = rootGraph();
        root.dspTraces_.clear();
        root.monoPreviews_.clear();
    } else if (green::isValueType(*type) && !green::deliversDiscreteEvents(*type)) {
        for (int output = 0; output < std::ssize(entry.outputTriggers) - 1; ++output) {
            fireOutput(entry, output);
        }
    }
}

void Schematic::refreshMatrices(bool notify)
{
    for (auto& entry : nodes_) {
        if (entry.kind == PrimitiveKind::Matrix) refreshMatrix(entry, notify);
    }
    for (auto& [id, child] : modules_) child->refreshMatrices(notify);
}

void Schematic::primeGreenLogic()
{
    using enum PrimitiveKind;
    for (auto& entry : nodes_) {
        if ((entry.kind != BoolToTrueTrigger && entry.kind != BoolToFalseTrigger && entry.kind != Changed &&
             entry.kind != IntTransition && entry.kind != Multiplexer) || !inputType(entry.id, 0)) {
            continue;
        }

        // A restored graph establishes its current state without emitting a transition.
        try {
            entry.localValue = entry.kind == Multiplexer && entry.flowStoneRoutingBranches
                ? Value{std::int32_t{routerSelection(entry)}} : readInput(entry.id, 0);
        } catch (const std::logic_error&) {
            // Upstream values may be temporarily unreadable while editing a schematic.
        }
    }
    for (auto& [id, child] : modules_) {
        child->primeGreenLogic();
    }
}

Value Schematic::readInput(int id, int input) const
{
    const auto& entry = node(id);
    if (entry.tick && input == 0) {
        return readTick(entry).rate;
    }
    if (editable(id)) {
        return *entry.localValue;
    }
    if (entry.kind == PrimitiveKind::Module) {
        const auto& child = *module(id);
        const auto& boundary = child.node(entry.moduleInputs.at(input).endpoint);
        if (const auto* writer = child.propertyWriter(boundary);
            writer && boundary.boundaryType == child.inputType(writer->id, 0) &&
            child.hasValueInput(writer->id, 0) && !hasValueInput(id, input)) {
            return child.readInput(writer->id, 0);
        }
    }
    const auto port = ports_.at(id).inputs.at(input);
    if (!port) {
        throw std::invalid_argument("This connector is evaluated by the DSP graph");
    }
    auto value = runtime_->pull(*port);
    if (auto* view = std::get_if<yellow::View>(&value); view && view->panel != 0) {
        // The runtime accepts a single value source for a View. Trigger-only links carry no layer.
        const auto link = std::ranges::find_if(links_, [&](const auto& link) {
            return link.target == id && link.input == input && outputType(link.source, link.output) == Type::View;
        });
        if (link != links_.end()) {
            view->drawOrder.push_back(static_cast<std::size_t>(link - links_.begin()));
        }
    }
    return value;
}

Value Schematic::transformArray(int id, PrimitiveKind kind, Type type) const
{
    const auto amount = std::get<std::int32_t>(readInput(id, arrayTransformAmountInput));
    return withGreenArray(type, [&]<class Array>() -> Value {
        using Element = typename Array::value_type;
        auto values = std::get<Array>(readInput(id, 0));
        switch (kind) {
        case PrimitiveKind::ArraySection: {
            const auto first = std::get<std::int32_t>(readInput(id, arraySectionFirstInput));
            if (first < 0 || amount <= 0 || first >= std::ssize(values)) return Array{};
            const auto count = std::min<std::ptrdiff_t>(amount, std::ssize(values) - first);
            const auto begin = values.begin() + first;
            return Array(begin, begin + count);
        }
        case PrimitiveKind::RotateArray:
            if (!values.empty()) {
                const auto shift = amount % std::ssize(values);
                const auto right = shift < 0 ? shift + std::ssize(values) : shift;
                std::rotate(values.begin(), values.end() - right, values.end());
            }
            return values;
        case PrimitiveKind::ArrayFixedSize: {
            const auto size = static_cast<std::size_t>(std::max(amount, 0));
            if (size <= values.size()) {
                values.resize(size);
                return values;
            }
            const auto fill = std::get<Element>(readInput(id, arrayFixedSizeDefaultInput));
            const auto added = size - values.size();
            const auto bytesPerElement = maxArrayPaddingBytes / added;
            std::size_t textBytes = 0;
            if constexpr (std::is_same_v<Element, std::string>) {
                textBytes = fill.size();
            }
            if (bytesPerElement < sizeof(Element) || textBytes > bytesPerElement - sizeof(Element)) {
                throw std::length_error("Array Fixed Size can add up to 64 MiB of padding");
            }
            values.resize(size, fill);
            return values;
        }
        default:
            throw std::logic_error("Expected an array transform");
        }
    });
}

Value Schematic::readStoredArray(const SchematicNode& entry, int output, Type type) const
{
    if (!entry.arrayStorage) return defaultValue(type);

    const bool stack = entry.kind == PrimitiveKind::ArrayStack;
    const bool history = entry.kind == PrimitiveKind::ArrayHistory;
    return std::visit([&](const auto& values) -> Value {
        if ((isQueueOrStack(entry.kind) && output == 1) || (entry.kind == PrimitiveKind::Array && output == 2)) {
            return static_cast<std::int32_t>(
                std::min<std::size_t>(values.size(), std::numeric_limits<std::int32_t>::max()));
        }
        using Element = typename std::decay_t<decltype(values)>::value_type;
        if (output != storageArrayOutput(entry.kind)) {
            return values.empty() ? Element{} : (stack ? values.back() : values.front());
        }
        if (stack || (history && !std::get<bool>(readInput(entry.id, historyReverseInput)))) {
            return std::vector<Element>(values.rbegin(), values.rend());
        }
        return std::vector<Element>(values.begin(), values.end());
    }, *entry.arrayStorage);
}

void Schematic::receiveStoredArrayTrigger(SchematicNode& entry, int input)
{
    const auto type = outputType(entry.id, storageArrayOutput(entry.kind));
    if (!type) return;

    const int id = entry.id;
    const bool history = entry.kind == PrimitiveKind::ArrayHistory;
    const bool indexed = entry.kind == PrimitiveKind::Array;
    const bool stack = entry.kind == PrimitiveKind::ArrayStack;
    if (input == (history ? historyValueInput : storeValueInput) || (indexed && input == storeIndexInput)) return;

    const bool changed = withGreenArray(*type, [&]<class Array>() -> bool {
        using Element = typename Array::value_type;
        using Queue = std::deque<Element>;
        if (!entry.arrayStorage) entry.arrayStorage.emplace(std::in_place_type<Queue>);
        auto& values = std::get<Queue>(*entry.arrayStorage);

        if (history) {
            if (input == historyReverseInput) return true;
            const auto limit = std::max(0, std::get<std::int32_t>(readInput(id, historySizeInput)));
            if (input == historyAddInput && limit > 0) {
                values.push_back(std::get<Element>(readInput(id, historyValueInput)));
            } else if (std::ssize(values) <= limit) {
                return false;
            }
            while (std::ssize(values) > limit) values.pop_front();
            return true;
        }

        if (input == (indexed ? storeAssignInput : queueReplaceInput)) {
            auto replacement = std::get<Array>(readInput(id, input));
            if (stack) std::ranges::reverse(replacement);
            values = Queue(std::make_move_iterator(replacement.begin()), std::make_move_iterator(replacement.end()));
            return true;
        }
        if (input == (indexed ? storeClearInput : queueClearInput)) {
            if (values.empty()) return false;
            values.clear();
            return true;
        }
        if (!indexed) {
            if (input == queuePushInput) {
                values.push_back(std::get<Element>(readInput(id, storeValueInput)));
            } else if (input == queuePopInput && !values.empty()) {
                if (stack) values.pop_back();
                else values.pop_front();
            } else {
                return false;
            }
            return true;
        }

        const auto index = std::get<std::int32_t>(readInput(id, storeIndexInput));
        if (index < 0) return false;
        if (input == storeDeleteInput) {
            if (index >= std::ssize(values)) return false;
            values.erase(values.begin() + index);
            return true;
        }
        const auto position = static_cast<std::size_t>(index);
        auto value = std::get<Element>(readInput(id, storeValueInput));
        if (position >= values.size()) {
            if (position + 1 - values.size() > maxArrayPaddingBytes / sizeof(Element)) {
                throw std::length_error("Array can add up to 64 MiB of padding");
            }
            values.resize(position + 1);
            values.back() = std::move(value);
        } else if (input == storeSetInput) {
            values[index] = std::move(value);
        } else if (input == storeInsertInput) {
            values.insert(values.begin() + index, std::move(value));
        }
        return true;
    });
    if (changed) fireOutputs(id);
}

bool Schematic::hasValueInput(int id, int input) const
{
    return std::ranges::any_of(links_, [&](const auto& link) {
        if (link.target != id || link.input != input) {
            return false;
        }
        const auto type = outputType(link.source, link.output);
        return type && green::isValueType(*type);
    });
}

SignalReadout Schematic::inspectInput(int id, int input) const
{
    const auto& entry = node(id);
    if ((entry.kind == PrimitiveKind::MonoToPoly || entry.kind == PrimitiveKind::PolyToMono) && input == 1) {
        const auto index = voiceIndex(id);
        return {index ? std::optional<Value>{*index} : std::nullopt, entry.inputTriggers.at(input)};
    }
    const auto type = inputType(id, input);
    return {type && green::isValueType(*type) ? std::optional{readInput(id, input)} : std::nullopt,
            entry.inputTriggers.at(input)};
}

SignalReadout Schematic::inspectOutput(int id, int output) const
{
    const auto& entry = node(id);
    const auto type = outputType(id, output);
    return {type && green::isValueType(*type) ? std::optional{readValue(id, output)} : std::nullopt,
            entry.outputTriggers.at(output)};
}

SignalReadout Schematic::inspectLink(SchematicLink link) const
{
    const auto count = linkTriggers_.at(link);
    const auto source = ports_.at(link.source).outputs.at(link.output);
    const auto target = ports_.at(link.target).inputs.at(link.input);
    return {source && target ? runtime_->readConnection(*source, *target) : std::nullopt, count};
}

yellow::View Schematic::moduleParentView(const SchematicNode& entry) const
{
    auto view = std::get<yellow::View>(readInput(entry.id, parentViewInput));
    if (entry.kind == PrimitiveKind::ModulePropertiesGui && view.panel != 0) {
        const auto* owner = panelOwner(view);
        if (!owner || owner->node(view.panel).kind != PrimitiveKind::ModulePropertiesGui) {
            return {};
        }
    }
    return view;
}

yellow::View Schematic::panelView(const SchematicNode& mgui, const yellow::View& parentView) const
{
    auto bounds = anchoredPanelArea(mgui, parentView, {});
    bounds.x = mgui.panelBounds.x;
    bounds.y = mgui.panelBounds.y;
    return {mgui.id, parentView.panel, bounds, std::get<bool>(readInput(mgui.id, mouseInput)),
            path_, parentView.schematicPath};
}

yellow::Area Schematic::anchoredPanelArea(const SchematicNode& entry, const yellow::View& parent,
                                        green::Point position) const
{
    auto area = yellow::Area{position.x, position.y, entry.panelBounds.width, entry.panelBounds.height};
    if (parent.panel == 0 || !entry.anchorReference) {
        return area;
    }
    const auto adjust = [&](std::uint8_t start, std::uint8_t end, float delta, float& origin, float& size) {
        if (entry.panelAnchors & end) {
            if (entry.panelAnchors & start) {
                size = std::max(size + delta, 1.0F / windowPixelsPerUnit);
            } else {
                origin += delta;
            }
        }
    };
    adjust(AnchorLeft, AnchorRight, parent.bounds.width - entry.anchorReference->x, area.x, area.width);
    adjust(AnchorTop, AnchorBottom, parent.bounds.height - entry.anchorReference->y, area.y, area.height);
    return area;
}

ModulePanel Schematic::readModulePanel(int id) const
{
    const auto& entry = node(id);
    const bool properties = entry.kind == PrimitiveKind::ModulePropertiesGui;
    if (!properties && entry.kind != PrimitiveKind::ModuleGui) {
        throw std::invalid_argument("Choose an MGUI or Props node");
    }
    auto parent = moduleParentView(entry);
    const auto& owner = parent_->node(path_.back());
    const auto area = anchoredPanelArea(entry, parent, properties ? owner.propertiesPosition : owner.panelPosition);
    return {panelView(entry, parent), properties || std::get<bool>(readInput(id, borderInput)), std::move(parent),
            {area.x, area.y}, entry.panelAnchors};
}

ModulePanel Schematic::readPanel(int id) const
{
    const auto& entry = node(id);
    return entry.kind == PrimitiveKind::Window ? ModulePanel{windowView(entry), false, {}} : readModulePanel(id);
}

std::optional<ModulePanel> Schematic::modulePanel(ModulePanelKind kind) const
{
    const auto primitive = kind == ModulePanelKind::Front ? PrimitiveKind::ModuleGui : PrimitiveKind::ModulePropertiesGui;
    const auto entry = std::ranges::find(nodes_, primitive, &SchematicNode::kind);
    if (entry == nodes_.end()) {
        return std::nullopt;
    }
    return readModulePanel(entry->id);
}

void Schematic::setPanelHidden(int id, bool hidden)
{
    if (!module(id)) {
        throw std::invalid_argument("Choose a Module");
    }
    node(id).panelHidden = hidden;
}

void Schematic::setPanelUnlocked(int id, bool unlocked)
{
    if (!module(id)) {
        throw std::invalid_argument("Choose a Module");
    }
    node(id).panelUnlocked = unlocked;
}

void Schematic::setPropertiesVisibility(int id, PropertiesVisibility visibility)
{
    const auto* child = module(id);
    if (!child || (visibility != PropertiesVisibility::Closed &&
                   !child->modulePanel(ModulePanelKind::Properties))) {
        throw std::invalid_argument("Choose a Module with a properties panel");
    }
    node(id).propertiesVisibility = visibility;
}

Schematic& Schematic::findPanelItem(int moduleId, std::span<const int> itemPath,
                                  ModulePanelKind surface, ModulePanelKind item)
{
    auto* owner = module(moduleId);
    const auto target = owner ? owner->modulePanel(surface) : std::nullopt;
    if (!target || itemPath.empty()) {
        throw std::invalid_argument("Choose a child Module on the panel");
    }
    auto* containingGraph = owner;
    for (int id : itemPath.first(itemPath.size() - 1)) {
        containingGraph = containingGraph->module(id);
        if (!containingGraph) {
            throw std::invalid_argument("Choose a child Module on the panel");
        }
    }
    const int itemId = itemPath.back();
    auto* child = containingGraph->module(itemId);
    const auto panel = child ? child->modulePanel(item) : std::nullopt;
    if (!panel || !panel->view.attachesTo(target->view)) {
        throw std::invalid_argument("Connect the child panel to this View before positioning it");
    }
    return *child;
}

void Schematic::movePanelItem(int moduleId, std::span<const int> itemPath, green::Point position,
                              ModulePanelKind surface, ModulePanelKind item)
{
    if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
        throw std::invalid_argument("Panel position must be finite");
    }
    auto& child = findPanelItem(moduleId, itemPath, surface, item);
    const auto panel = *child.modulePanel(item);
    auto bounds = panel.view.bounds;
    bounds.x = position.x;
    bounds.y = position.y;
    child.applyPanelArea(panel.view.panel, bounds);
    child.refreshConnections();
}

void Schematic::setPanelAnchors(int id, std::uint8_t anchors)
{
    const auto panel = readModulePanel(id);
    node(id).panelAnchors = anchors;
    applyPanelArea(id, {panel.position.x, panel.position.y, panel.view.bounds.width, panel.view.bounds.height});
}

void Schematic::resizePanel(int moduleId, float width, float height, ModulePanelKind surface)
{
    auto* owner = module(moduleId);
    const auto panel = owner ? owner->modulePanel(surface) : std::nullopt;
    if (!panel || !std::isfinite(width) || !std::isfinite(height) || width <= 0.0F || height <= 0.0F) {
        throw std::invalid_argument("Choose an existing module panel and a positive size");
    }
    const auto position = panel->position;
    owner->applyPanelArea(panel->view.panel, {position.x, position.y, width, height});
    owner->refreshConnections();
}

void Schematic::setValue(int id, Value value)
{
    if (!editable(id) || green::typeOf(value) != primitive(node(id).kind).outputs.front().type) {
        throw std::invalid_argument("This node cannot accept that value");
    }
    if (auto* number = std::get_if<float>(&value)) {
        *number = green::normalizeFloat(*number);
    }
    auto& current = *node(id).localValue;
    if (current != value) {
        current = std::move(value);
        fireOutputs(id);
        refreshConnections();
    }
}

void Schematic::fireTrigger(int id)
{
    auto& entry = node(id);
    const auto behavior = primitive(entry.kind).behavior;
    if (behavior == CaptureSignal) {
        captureSignals(entry);
    } else if (behavior == SampleMono) {
        sampleMono(entry);
    } else if (behavior == TriggerButton) {
        fireOutputs(id);
    } else {
        throw std::invalid_argument("Choose a Trigger node");
    }
    refreshConnections();
}

bool Schematic::hasDeferredTriggers() const
{
    return !rootGraph().deferredTriggers_.empty();
}

DeferredTriggerDispatch Schematic::dispatchDeferredTriggers()
{
    auto& root = rootGraph();
    auto& pending = root.deferredTriggers_;
    DeferredTriggerDispatch result;
    constexpr std::size_t maxTriggersPerTurn = 256;
    const auto count = std::min(pending.size(), maxTriggersPerTurn);
    for (std::size_t i = 0; i < count; ++i) {
        const auto pulse = std::move(pending.front());
        pending.pop_front();

        auto* owner = &root;
        for (const int module : pulse.path) {
            owner = owner->module(module);
            if (!owner) break;
        }
        if (!owner) continue;
        const auto entry = std::ranges::find(owner->nodes_, pulse.node, &SchematicNode::id);
        if (entry == owner->nodes_.end() || entry->kind != PrimitiveKind::TriggerThread) continue;
        try {
            result.fired = true;
            owner->fireOutput(*entry, 0);
            owner->refreshConnections();
        } catch (const std::exception&) {
            if (!result.error) result.error = std::current_exception();
        }
    }
    return result;
}

void Schematic::loadBitmap(int id, std::string_view path)
{
    loadBitmapIntoNode(id, path);
    refreshConnections();
}

void Schematic::loadBitmapIntoNode(int id, std::string_view path)
{
    auto& entry = node(id);
    if (entry.kind == PrimitiveKind::Bitmap) {
        storeBitmap(id, decodeBitmap(path));
        return;
    }
    if (entry.kind != PrimitiveKind::BitmapArrayFromFile) {
        throw std::invalid_argument("Choose a bitmap file loader");
    }
    const auto index = std::get<std::int32_t>(readInput(id, bitmapArrayIndexInput));
    if (index < 0) {
        throw std::invalid_argument("Bitmap Array index must be zero or greater");
    }
    auto image = decodeBitmap(path);
    storeBitmapArrayEntry(entry, index, std::move(image));
}

yellow::Bitmap Schematic::decodeBitmap(std::string_view path) const
{
    if (path.empty()) {
        throw std::invalid_argument("Choose a bitmap file");
    }
    if (!bitmapLoader_) {
        throw std::runtime_error("Bitmap file loading is unavailable");
    }
    auto bitmap = bitmapLoader_(path);
    if (bitmap.empty()) {
        throw std::runtime_error("The file did not produce a bitmap");
    }
    return bitmap.copy();
}

void Schematic::storeBitmap(int id, yellow::Bitmap bitmap)
{
    node(id).localValue = std::move(bitmap);
    fireOutputs(id);
}

const SchematicNode& Schematic::node(int id) const
{
    const auto found = std::ranges::find(nodes_, id, &SchematicNode::id);
    if (found == nodes_.end()) {
        throw std::invalid_argument("Unknown node");
    }
    return *found;
}

SchematicNode& Schematic::node(int id)
{
    return const_cast<SchematicNode&>(std::as_const(*this).node(id));
}

Schematic::PortTypes Schematic::resolveTypes(const SchematicNode& entry,
    const std::vector<std::vector<Type>>& sources, std::optional<Type> hint) const
{
    const auto& spec = primitive(entry.kind);
    PortTypes types;
    for (const auto& port : entry.outputPorts()) {
        types.outputs.push_back(port.type);
    }
    for (const auto& port : entry.inputPorts()) {
        types.inputs.push_back(port.type);
    }
    if (entry.kind == PrimitiveKind::Indexer) {
        types.inputs.assign(std::max<std::size_t>(1, sources.size()), Type::Trigger);
    }
    if (isIndexedRouter(entry.kind) || entry.kind == PrimitiveKind::Matrix) {
        const auto count = std::max(spec.inputs.size(), sources.size());
        auto type = entry.typeOverride;
        if (!type) {
            // Prefer the stream domain if a data wire carries a stream.
            for (std::size_t i = 1; i < sources.size() && !type; ++i) {
                for (const auto candidate : sources[i]) {
                    if (green::isDspType(candidate) && supportsTemplateType(entry.kind, candidate)) {
                        type = candidate;
                        break;
                    }
                }
            }
            if (!type && hint && green::isDspType(*hint) &&
                std::ranges::all_of(sources | std::views::drop(1), [&](const auto& connected) {
                    return std::ranges::all_of(connected, [&](Type candidate) {
                        return canSupplyDspInput(candidate, *hint);
                    });
                })) {
                type = hint;
            }
        }
        for (std::size_t i = 1; !type && i < sources.size(); ++i) {
            const auto& connected = sources[i];
            const auto found = std::ranges::find_if(connected, [&](Type candidate) {
                return supportsTemplateType(entry.kind, candidate);
            });
            if (found != connected.end()) type = *found;
        }
        if (!type) type = hint;
        types.inputs.resize(count);
        std::fill(types.inputs.begin() + 1, types.inputs.end(), type);
        std::ranges::fill(types.outputs, type);
        return types;
    }
    if (spec.behavior == CreateBus) {
        for (std::size_t i = firstBusChannelInput; i < sources.size(); ++i) {
            const auto& connected = sources[i];
            const auto value = std::ranges::find_if(connected, [](Type type) {
                return type != Type::Bus && green::isValueType(type);
            });
            if (value != connected.end()) {
                types.inputs[i] = *value;
            } else if (std::ranges::find(connected, Type::Trigger) != connected.end()) {
                types.inputs[i] = Type::Trigger;
            }
        }
        return types;
    }
    if (!spec.hasTemplateConnectors() || isModuleEndpoint(entry.kind)) {
        return types;
    }

    if (entry.kind == PrimitiveKind::StreamToGreen) {
        auto sampleType = entry.typeOverride;
        if (!sampleType && !sources.empty() && !sources.front().empty()) {
            const auto source = sources.front().front();
            sampleType = source == Type::StreamBoolean || source == Type::MonoBoolean ? Type::Boolean : Type::Float;
        }
        if (!sampleType) sampleType = hint.value_or(Type::Float);
        types.inputs.front() = sampleType == Type::Boolean ? Type::MonoBoolean : Type::Mono;
        types.outputs.front() = sampleType;
        return types;
    }

    if (isPropertyEndpoint(entry.kind)) {
        const auto* boundary = propertyBoundary(entry);
        const auto bound = boundary ? boundary->boundaryType : std::nullopt;
        auto type = entry.typeOverride;
        if (bound && supportsTemplateType(entry.kind, *bound)) {
            if (type && type != bound) {
                throw std::invalid_argument("The property connector must match the named module connector");
            }
            type = bound;
        }
        if (entry.kind == PrimitiveKind::ModuleInputProperty) {
            types.outputs.front() = type ? type : hint;
        } else {
            if (!type && !sources.empty()) {
                const auto& connected = sources.front();
                const auto value = std::ranges::find_if(connected, [&](Type candidate) {
                    return candidate != Type::Trigger && supportsTemplateType(entry.kind, candidate);
                });
                if (value != connected.end()) type = *value;
                else if (std::ranges::contains(connected, Type::Trigger)) type = Type::Trigger;
            }
            types.inputs.front() = type;
        }
        return types;
    }

    if (supportsStreamMath(spec)) {
        auto signal = entry.typeOverride;
        if (!signal) {
            for (const auto& connected : sources) {
                for (const auto type : connected) {
                    if ((signal = numericStreamFamily(type))) break;
                }
                if (signal) break;
            }
            if (!signal && hint) signal = numericStreamFamily(*hint);
        }
        if (!signal && spec.behavior == StreamMath) signal = Type::Stream;
        if (signal && numericStreamFamily(*signal)) {
            std::ranges::fill(types.inputs, signal);
            std::ranges::fill(types.outputs, signal);
            return types;
        }
    }
    if (entry.kind == PrimitiveKind::ViewLoop || entry.kind == PrimitiveKind::Loop) {
        const std::size_t first = entry.kind == PrimitiveKind::ViewLoop ? 1 : 0;
        auto number = entry.typeOverride;
        if (!number) {
            for (std::size_t input = first; input < first + 3 && input < sources.size(); ++input) {
                for (const auto type : sources[input]) {
                    if (type == Type::Float || (type == Type::Int && !number)) {
                        number = type;
                    }
                }
            }
        }
        number = number ? number : hint;
        std::fill_n(types.inputs.begin() + first, 3, number);
        types.outputs[first] = number;
        return types;
    }
    if (spec.behavior == BuildArray) {
        auto element = entry.typeOverride;
        for (const auto& connected : sources) {
            if (element) break;
            const auto first = std::ranges::find_if(connected, [](Type type) { return arrayForElement(type).has_value(); });
            if (first != connected.end()) element = *first;
        }
        if (!element && hint) element = arrayElement(*hint);
        types.inputs.assign(std::max<std::size_t>(1, sources.size()), element);
        types.outputs[0] = element ? arrayForElement(*element) : std::nullopt;
        return types;
    }
    if (spec.behavior == StoreArray) {
        const bool history = entry.kind == PrimitiveKind::ArrayHistory;
        const int valuePort = history ? historyValueInput : storeValueInput;
        const int assignPort = entry.kind == PrimitiveKind::Array ? storeAssignInput : queueReplaceInput;
        auto array = entry.typeOverride;
        bool hasText = false;
        if (!array && !sources.empty()) {
            if (!history) {
                const auto& assign = sources[assignPort];
                const auto first = std::ranges::find_if(assign, isArray);
                if (first != assign.end()) array = *first;
                hasText = std::ranges::contains(assign, Type::String);
            }
            for (const auto type : sources[valuePort]) {
                if (!array) array = arrayForElement(type);
            }
        }
        if (!array) array = hint;
        if (!array && hasText) array = Type::StringArray;
        const auto element = array ? arrayElement(*array) : std::nullopt;
        types.inputs[valuePort] = element;
        if (!history) types.inputs[assignPort] = array;
        if (isQueueOrStack(entry.kind)) types.outputs[0] = element;
        types.outputs[storageArrayOutput(entry.kind)] = array;
        return types;
    }
    if (entry.kind == PrimitiveKind::ArrayLength) {
        types.inputs[0] = entry.typeOverride;
        if (!types.inputs[0] && !sources.empty()) {
            const auto found = std::ranges::find_if(sources[0], [](Type type) {
                return type == Type::String || isArray(type);
            });
            if (found != sources[0].end()) types.inputs[0] = *found;
        }
        return types;
    }
    if (hasGenericArrayInput(entry.kind) || entry.kind == PrimitiveKind::SortArray || spec.behavior == GetArrayElement ||
        spec.behavior == AppendArrays || spec.behavior == TransformArray) {
        const bool getting = spec.behavior == GetArrayElement || hasGenericArrayInput(entry.kind);
        const bool appending = spec.behavior == AppendArrays;
        const bool resizing = entry.kind == PrimitiveKind::ArrayFixedSize;
        auto array = entry.typeOverride;
        bool hasText = false;
        for (const auto& connected : sources | std::views::take(appending ? 2 : 1)) {
            if (array) break;
            const auto first = std::ranges::find_if(connected, isArray);
            if (first != connected.end()) array = *first;
            hasText |= std::ranges::contains(connected, Type::String);
        }
        if (!array && resizing && sources.size() > arrayFixedSizeDefaultInput) {
            const auto& defaults = sources[arrayFixedSizeDefaultInput];
            const auto first = std::ranges::find_if(defaults, [](Type type) { return arrayForElement(type).has_value(); });
            if (first != defaults.end()) array = arrayForElement(*first);
        }
        if (!array && hint) array = getting ? arrayForElement(*hint) : hint;
        if (!array && hasText) array = Type::StringArray;
        types.inputs[0] = array;
        if (appending) types.inputs[1] = array;
        if (resizing && array) types.inputs[arrayFixedSizeDefaultInput] = arrayElement(*array);
        types.outputs[0] = array && getting ? arrayElement(*array) : array;
        if (entry.kind == PrimitiveKind::ArraySplitter) {
            std::ranges::fill(types.outputs, types.outputs.front());
        }
        return types;
    }
    if (entry.kind == PrimitiveKind::ViewArrayLoop) {
        auto array = entry.typeOverride;
        if (!array && sources.size() > 1) {
            const auto& input = sources[1];
            const auto actual = std::ranges::find_if(input, [](Type type) { return arrayElement(type).has_value(); });
            if (actual != input.end()) {
                array = *actual;
            } else if (std::ranges::contains(input, Type::String)) {
                array = hint ? arrayForElement(*hint) : std::optional{Type::StringArray};
            }
        }
        if (!array && hint) {
            array = arrayForElement(*hint);
        }
        types.inputs[1] = array;
        types.outputs[1] = array ? arrayElement(*array) : std::nullopt;
        return types;
    }

    // Honor a pinned type first. Otherwise infer from incoming links, using a
    // wired destination as a hint when there are no typed operands yet.
    const auto preferred = [&](std::size_t index) -> std::optional<Type> {
        if (entry.typeOverride) {
            return entry.typeOverride;
        }
        if (index < sources.size()) {
            std::optional<Type> textType;
            for (auto type : sources[index]) {
                if (supportsTemplateType(entry.kind, type) && type != Type::Trigger) {
                    // In automatic arithmetic, a String can be parsed in a
                    // numeric context. Explicit String mode keeps concatenation.
                    if ((spec.behavior == Binary || spec.behavior == Compare) && type == Type::String) {
                        textType = type;
                        continue;
                    }
                    return type;
                }
            }
            if (textType) {
                return textType;
            }
            if ((spec.behavior == Switch || spec.behavior == SendWireless) &&
                std::ranges::find(sources[index], Type::Trigger) != sources[index].end()) {
                return Type::Trigger;
            }
        }
        return {};
    };
    if (spec.behavior == SendWireless) {
        types.inputs[0] = preferred(0);
        return types;
    }
    if (spec.behavior == ReceiveWireless) {
        types.outputs[0] = entry.typeOverride ? entry.typeOverride : hint;
        return types;
    }

    auto left = preferred(0);
    if (spec.behavior == NoteConversion) {
        if (!left && hint) left = noteInputForOutput(entry.kind, *hint);
        types.inputs.front() = left;
        if (left) {
            const bool array = isArray(*left);
            types.outputs.front() = entry.kind == PrimitiveKind::IntToNoteName
                ? (array ? Type::StringArray : Type::String) : (array ? Type::IntArray : Type::Int);
        }
        return types;
    }
    if (spec.behavior == GreenLogic) {
        const bool bitwise = isBitwiseBinary(entry.kind);
        if (bitwise || entry.kind == PrimitiveKind::Select || entry.kind == PrimitiveKind::LastSwitch) {
            const auto right = preferred(1);
            if (!left || (bitwise && right == Type::Int) || (left == Type::Int && right == Type::Float)) {
                left = right;
            }
        }
        const auto type = left ? left : hint;
        for (auto& port : types.inputs) {
            if (!port) port = type;
        }
        types.outputs.front() = type;
        return types;
    }
    if (spec.behavior != Binary && spec.behavior != Compare) {
        types.outputs.front() = left ? left : hint;
        types.inputs[0] = types.outputs.front();
        return types;
    }

    auto right = preferred(1);
    const auto convertsTextTo = [&](Type type) { return type != Type::StringArray || spec.behavior == Compare; };
    if (left == Type::String && right && convertsTextTo(*right)) {
        left = right;
    } else if (right == Type::String && left && convertsTextTo(*left)) {
        right = left;
    }
    if (!left) {
        left = right ? right : hint;
    }
    if (!right) {
        right = left;
    }
    if (left && right) {
        if (spec.behavior == Binary) {
            types.outputs.front() = arithmeticResult(*left, *right);
        } else if (*left != *right && !(isNumericComparisonType(*left) && isNumericComparisonType(*right)) &&
                   !(isNumericComparisonArray(*left) && isNumericComparisonArray(*right))) {
            throw std::invalid_argument("These connector types cannot be compared");
        }
    }
    types.inputs[0] = left;
    types.inputs[1] = right;
    return types;
}

Schematic::NodePorts Schematic::makePorts(green::Runtime& runtime, const SchematicNode& entry, PortTypes types)
{
    const auto id = entry.id;
    const bool capturesSignal = primitive(entry.kind).behavior == CaptureSignal;
    const bool isModuleGui = entry.kind == PrimitiveKind::ModuleGui;
    const bool isMatrixSet = entry.kind == PrimitiveKind::ColorMatrixSet;
    const bool isPen = entry.kind == PrimitiveKind::Pen;
    const bool isGraph = entry.kind == PrimitiveKind::GraphDots || entry.kind == PrimitiveKind::LineGraph;
    const bool isPanelInput = entry.kind == PrimitiveKind::EditControl || entry.kind == PrimitiveKind::PopupList;
    NodePorts ports{{}, {}, std::move(types)};
    for (int index = 0; index < static_cast<int>(ports.types.inputs.size()); ++index) {
        const auto onTrigger = [this, id, index](auto&, auto) { receiveTrigger(id, index); };
        const auto type = ports.types.inputs[index];
        if (type && green::isDspType(*type)) {
            ports.inputs.push_back(std::nullopt);
        } else if (!type || *type == Type::Trigger) {
            ports.inputs.push_back(runtime.addTriggerInput(onTrigger));
        } else {
            auto initial = defaultValue(*type);
            using enum PrimitiveKind;
            if ((entry.kind == MidiInSelect && index == 0) || (entry.kind == MidiOutSelect && index == 1)) {
                initial = std::int32_t{-1};
            }
            if ((entry.kind == MidiChannelFilter && index == 1) || (entry.kind == MidiEvent && index == 1) ||
                (entry.kind == MidiNoteEvent && index == 0)) initial = std::int32_t{1};
            if ((entry.kind == MidiEvent && index == 0) || (entry.kind == MidiStatusFilter && index == 1)) {
                initial = std::int32_t{0x90};
            }
            if (entry.kind == MidiEvent && index == 3) initial = std::int32_t{100};
            if ((entry.kind == MidiNoteEvent && index == 1) || (entry.kind == MidiNoteFilter && index == 1)) {
                initial = std::int32_t{60};
            }
            if (entry.kind == MidiNoteEvent && index == 2) initial = std::int32_t{100};
            if (entry.kind == MidiNoteEvent && index == 3) initial = std::int32_t{250};
            if (entry.kind == AudioStreamIn && index == 4) initial = 1.F;
            if (entry.kind == VideoStreamIn && index == 1) initial = std::int32_t{640};
            if (entry.kind == VideoStreamIn && index == 2) initial = std::int32_t{480};
            if ((entry.kind == NetworkClient && index == 2) || (entry.kind == NetworkServer && index == 1)) {
                initial = std::string{"TCP"};
            }
            if (entry.kind == MidiCCStream && index == 3) initial = 1.0F;
            if (entry.kind == FileDialog && entry.flowStoneFileDialog && index == 2) initial = std::int32_t{1};
            if ((entry.kind == DeZipper || entry.kind == FloatDeZipper) && index == 1) initial = 40.0f;
            if (entry.kind == PresetManager && index == 0) initial = std::int32_t{1};
            if (entry.kind == PresetLoad && index == 5) initial = std::numeric_limits<std::int32_t>::max();
            if (entry.kind == PresetSave && index == 5) initial = std::int32_t{1};
            if (entry.kind == PresetFloat && index == 9 && entry.presetParameter && entry.presetParameter->flowStoneFloat)
                initial = entry.presetParameter->flowStoneFloat->order;
            if (((entry.kind == PresetFloat || entry.kind == PresetBool || entry.kind == PresetBypass) &&
                 (index == 5 || index == 6 || index == 8)) ||
                (entry.kind == PresetString && index == 5) ||
                (entry.kind == PresetArray && (index == 7 || index == 8 || index == 12))) initial = true;
            if (entry.kind == Voices && index < 2) initial = std::int32_t{32};
            if (entry.kind == PolyToMonoHop && index == 1) initial = std::int32_t{1};
            if ((entry.kind == NotesPitchFilter || entry.kind == NotesVelocityFilter || entry.kind == NotesChannelFilter)
                && (index == 3 || index == 5)) initial = std::int32_t{-1};
            if ((isModuleGui && index == borderInput) || (entry.kind == PrimitiveKind::ShowCursor && index == 0)) {
                initial = true;
            } else if (capturesSignal && index == static_cast<int>(ports.types.inputs.size()) - 2) {
                initial = std::int32_t{512};
            } else if (entry.kind == PrimitiveKind::Window && index == 1) {
                initial = std::int32_t{defaultWindowWidth};
            } else if (entry.kind == PrimitiveKind::Window && index == 2) {
                initial = std::int32_t{defaultWindowHeight};
            } else if (entry.kind == PrimitiveKind::Window && index == 4) {
                initial = std::string{"Window"};
            } else if ((entry.kind == PrimitiveKind::ViewLoop && index == 2) ||
                       (entry.kind == PrimitiveKind::Loop && index == loopIncrementInput)) {
                initial = type == Type::Int ? Value{std::int32_t{1}} : Value{1.0F};
            } else if (entry.kind == PrimitiveKind::ViewScale && index == 1) {
                initial = 1.0F;
            } else if (entry.kind == PrimitiveKind::ViewBlur && index == 1) {
                initial = yellow::ViewBlur{}.size;
            } else if (entry.kind == PrimitiveKind::ViewShadow && index == 1) {
                initial = yellow::ViewShadow{}.distance;
            } else if (entry.kind == PrimitiveKind::ViewShadow && index == 2) {
                initial = yellow::ViewShadow{}.blur;
            } else if (entry.kind == PrimitiveKind::ViewShadow && index == 3) {
                initial = yellow::ViewShadow{}.alpha;
            } else if (entry.kind == PrimitiveKind::ViewGlow && index == 1) {
                initial = yellow::ViewGlow{}.blur;
            } else if (entry.kind == PrimitiveKind::ViewGlow && index == 2) {
                initial = yellow::ViewGlow{}.color;
            } else if (entry.kind == PrimitiveKind::ViewGlow && index == 3) {
                initial = yellow::ViewGlow{}.strength;
            } else if (entry.kind == PrimitiveKind::ViewShader && index == 3) {
                initial = yellow::ViewShader{}.amount;
            } else if (entry.kind == PrimitiveKind::ViewBloom && index == 1) {
                initial = yellow::ViewBloom{}.blur;
            } else if (entry.kind == PrimitiveKind::ViewBloom && index == 2) {
                initial = yellow::ViewBloom{}.threshold;
            } else if (entry.kind == PrimitiveKind::ViewBloom && index == 3) {
                initial = yellow::ViewBloom{}.strength;
            } else if (entry.kind == PrimitiveKind::DropList && index == 5) {
                initial = std::int32_t{-1};
            } else if (isPanelInput && index == 3) {
                initial = yellow::Color{0xFFEEEEEB};
            } else if (isPanelInput && index == 4) {
                initial = yellow::Color{0xFF1E1E1D};
            } else if (entry.kind == PrimitiveKind::PopupList && index == 6) {
                initial = std::int32_t{8};
            } else if ((entry.kind == PrimitiveKind::DropFiles && index == 4) ||
                       (entry.kind == PrimitiveKind::TriggerDiv && index == 1)) {
                initial = std::int32_t{1};
            } else if (entry.kind == PrimitiveKind::CounterAdvanced && index == counterMaxInput) {
                initial = std::numeric_limits<std::int32_t>::max() - 1;
            } else if (entry.kind == PrimitiveKind::DragFiles && index == 2) {
                initial = std::string{"copy"};
            } else if ((entry.kind == PrimitiveKind::BitmapCreate && index == 2) ||
                       (entry.kind == PrimitiveKind::DrawToBitmap && index == 1)) {
                initial = windowPixelsPerUnit;
            } else if (entry.kind == PrimitiveKind::BitmapResize && index == 1) {
                initial = 1.0F;
            } else if (entry.kind == PrimitiveKind::BitmapDraw && index == bitmapDrawAlphaInput) {
                initial = std::int32_t{255};
            } else if (isGraph && index == 4) {
                initial = 1.0F;
            } else if (isGraph && index == 5) {
                initial = -1.0F;
            } else if (entry.kind == PrimitiveKind::GraphDots && index == 7) {
                initial = defaultDotRadius;
            } else if (isPointPlot(entry.kind) && index >= firstPointPlotBoundInput &&
                       index - firstPointPlotBoundInput < static_cast<int>(pointPlotBounds.size())) {
                initial = pointPlotBounds[index - firstPointPlotBoundInput];
            } else if (entry.kind == PrimitiveKind::Dots && index == dotRadiusInput) {
                initial = defaultDotRadius;
            } else if (entry.kind == PrimitiveKind::MouseDragAccumulate && index == 3) {
                initial = green::Point{0.05F, 0.05F};
            } else if (entry.kind == PrimitiveKind::ArrayHistory && index == historySizeInput) {
                initial = std::int32_t{1};
            } else if (isMatrixSet && index == 3) {
                initial = 1.0F;
            } else if (entry.kind == PrimitiveKind::ColorDialog && index == 1) {
                initial = std::int32_t{255};
            } else if (isPen) {
                const auto pen = entry.localValue ? std::get<yellow::PenStroke>(std::get<yellow::PaintTool>(*entry.localValue)) : yellow::PenStroke{};
                if (index == 0) initial = pen.color;
                else if (index == 1) initial = pen.width;
                else initial = std::string(yellow::lineStyleName(pen.style));
            } else if (primitive(entry.kind).behavior == MakeBrush && entry.localValue) {
                initial = brushInputDefault(entry, index);
            } else if (primitive(entry.kind).behavior == MakeBrush && type == Type::Color) {
                initial = entry.kind == PrimitiveKind::HatchBrush && index == 1
                    ? yellow::HatchBrush{}.background : yellow::Color{0xFF000000};
            } else if (entry.kind == PrimitiveKind::Solid) {
                initial = entry.localValue ? std::get<yellow::SolidBrush>(std::get<yellow::PaintTool>(*entry.localValue)).color : yellow::SolidBrush{}.color;
            } else if (entry.kind == PrimitiveKind::StringFormat && entry.localValue) {
                const auto& format = std::get<yellow::StringFormat>(*entry.localValue);
                if (index == 0) initial = yellow::stringFormatStyleName(format);
                else if (index == 1) initial = std::string(yellow::alignmentName(format.horizontal));
                else initial = std::string(yellow::alignmentName(format.vertical));
            } else if (entry.kind == PrimitiveKind::Font) {
                const auto font = entry.localValue ? std::get<yellow::Font>(*entry.localValue) : yellow::Font{};
                if (index == 0) initial = font.face;
                else if (index == 1) initial = font.size;
                else if (index == 2) initial = yellow::fontStyleName(font.style);
                else initial = font.alternateFace;
            }
            ports.inputs.push_back(runtime.addInput(std::move(initial), onTrigger));
        }
    }
    for (int index = 0; index < static_cast<int>(ports.types.outputs.size()); ++index) {
        const auto type = ports.types.outputs[index];
        if (type && green::isDspType(*type)) {
            ports.outputs.push_back(std::nullopt);
        } else if (!type || *type == Type::Trigger) {
            ports.outputs.push_back(runtime.addTriggerOutput());
        } else {
            ports.outputs.push_back(runtime.addComputedOutput(*type, [this, id, index](const auto&) {
                return readValue(id, index);
            }));
        }
    }
    return ports;
}

void Schematic::receiveTriggerToolPulse(SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    switch (entry.kind) {
    case AboutToClose:
        if (input == closeDoneInput) entry.holdingClose = false;
        return;
    case TriggerThread: {
        auto& pending = rootGraph().deferredTriggers_;
        constexpr std::size_t maxPendingTriggers = 65'536;
        if (pending.size() == maxPendingTriggers) {
            throw std::runtime_error("Deferred Green trigger queue exceeded its capacity");
        }
        pending.push_back({path_, entry.id});
        return;
    }
    case TriggerDiv: {
        if (input == 1) {
            entry.triggersSinceDivision = 0;
            return;
        }
        const auto divisor = std::get<std::int32_t>(readInput(entry.id, 1));
        if (++entry.triggersSinceDivision >= divisor) {
            entry.triggersSinceDivision = 0;
            fireOutput(entry, 0);
        }
        return;
    }
    case TriggerDelta: {
        const auto& tickTime = rootGraph().tickDispatchTime_;
        const auto now = tickTime ? *tickTime : TickTime{std::chrono::steady_clock::now()};
        const auto previous = std::exchange(entry.previousTriggerTime, now);
        if (previous) {
            entry.localValue = std::chrono::duration<float, std::milli>(now - *previous).count();
            fireOutput(entry, 0);
        }
        return;
    }
    case Indexer:
        entry.localValue = static_cast<std::int32_t>(input);
        fireOutput(entry, 0);
        return;
    case Loop:
        if (input == loopRunInput) runNumericLoop(entry);
        return;
    case CounterAdvanced:
        receiveAdvancedCounterPulse(entry, input);
        return;
    default:
        throw std::logic_error("Expected a Trigger primitive");
    }
}

void Schematic::runNumericLoop(SchematicNode& entry)
{
    using Clock = std::chrono::steady_clock;
    const auto type = inputType(entry.id, 0);
    if (!type || entry.loopRunning) return;
    entry.greenLoopValue.reset();

    const auto run = [&]<typename Number>() {
        // Widen Int arithmetic so that crossing an endpoint cannot wrap around.
        using Cursor = std::conditional_t<std::is_same_v<Number, float>, float, std::int64_t>;
        auto value = static_cast<Cursor>(std::get<Number>(readInput(entry.id, 0)));
        const Cursor step = std::get<Number>(readInput(entry.id, loopIncrementInput));
        const Cursor end = std::get<Number>(readInput(entry.id, 2));
        if (step == 0 || !std::isfinite(value) || !std::isfinite(step) || !std::isfinite(end)) return;

        const auto deadline = std::get<bool>(readInput(entry.id, 3)) ? Clock::time_point::max()
                                                                    : Clock::now() + std::chrono::milliseconds(50);
        for (std::int64_t iteration = 0;
             (step > 0 ? value < end : value > end) &&
                 iteration <= std::numeric_limits<std::int32_t>::max() && Clock::now() < deadline;
             ++iteration) {
            entry.greenLoopValue = LoopValue{static_cast<Number>(value), static_cast<std::int32_t>(iteration)};
            fireOutput(entry, 2);
            const auto next = value + step;
            if (next == value) break;
            value = next;
        }
    };

    entry.loopRunning = true;
    try {
        if (type == Type::Int) run.template operator()<std::int32_t>();
        else run.template operator()<float>();
    } catch (...) {
        entry.loopRunning = false;
        throw;
    }
    entry.loopRunning = false;
    if (entry.greenLoopValue) {
        fireOutput(entry, 3);
    }
}

void Schematic::receiveAdvancedCounterPulse(SchematicNode& entry, int input)
{
    if (input == counterTypeInput || input == counterStartInput) return;

    const auto readInt = [&](int port) { return std::get<std::int32_t>(readInput(entry.id, port)); };
    const auto maximum = readInt(counterMaxInput);
    const auto minimum = readInt(counterMinInput);
    const auto previous = entry.localValue ? std::get<std::int32_t>(*entry.localValue) : 0;
    auto value = previous;
    std::optional<int> reached;

    if (input == counterStepInput) {
        const auto mode = readInt(counterTypeInput);
        if (mode >= 0 && mode <= 2) {
            const bool ascending = mode == 0 || (mode == 2 && entry.counterAscending);
            // Alpha uses a 32-bit step before applying the limits.
            value = green::addWrappedInts(value, ascending ? 1 : -1);
            if (ascending ? value >= maximum : value <= minimum) {
                value = ascending ? maximum : minimum;
                if (mode == 2) entry.counterAscending = !ascending;
                if (value != previous) reached = ascending ? reachedMaxOutput : reachedMinOutput;
            }
        }
        value = std::max(minimum, std::min(value, maximum));
    } else {
        if (input == counterResetInput) {
            value = hasValueInput(entry.id, counterStartInput) ? readInt(counterStartInput) : minimum;
            if (readInt(counterTypeInput) == 2) entry.counterAscending = true;
        }
        value = std::min(maximum, std::max(value, minimum));
        if (input == counterResetInput && value != previous) {
            if (value == minimum) reached = reachedMinOutput;
            else if (value == maximum) reached = reachedMaxOutput;
        }
    }

    entry.localValue = value;
    if (reached) fireOutput(entry, *reached);
    if (value != previous) fireOutput(entry, 0);
}

void Schematic::receiveTrigger(int id, int input)
{
    auto& entry = node(id);
    // Compilation can rebuild the Green runtime. Defer it until dispatch unwinds.
    if (entry.codeSourcePort(false) == input) {
        entry.codeSourceDirty = true;
        return;
    }
    const auto& spec = primitive(entry.kind);
    switch (spec.behavior) {
    case Module:
        module(id)->fireOutputs(entry.moduleInputs.at(input).endpoint);
        return;
    case ModuleOutput:
        parent_->fireOutput(parent_->node(path_.back()), entry.boundaryIndex);
        for (auto& property : nodes_) {
            if (property.kind == PrimitiveKind::ModuleInputProperty &&
                propertyBoundary(property) == &entry && outputType(property.id) == entry.boundaryType) {
                fireOutput(property, 0);
            }
        }
        return;
    case PropertyOutput:
        if (const auto* boundary = propertyBoundary(entry);
            boundary && boundary->boundaryType == inputType(id, 0) && propertyWriter(*boundary) == &entry &&
            !parent_->hasValueInput(path_.back(), boundary->boundaryIndex)) {
            fireOutputs(boundary->id);
        }
        return;
    case Counter: {
        auto& count = std::get<std::int32_t>(*entry.localValue);
        count = input == 1 ? 0 : green::addWrappedInts(count, 1);
        if (entry.kind == PrimitiveKind::BasicCounter && count < 0) count = 0;
        break;
    }
    case GreenLogic:
        if (isIndexedRouter(entry.kind)) {
            const auto type = outputType(id);
            if (type && green::isDspType(*type)) {
                auto& root = rootGraph();
                root.dspTraces_.clear();
                root.monoPreviews_.clear();
                return;
            }
            if (type == Type::View && input == 0) ++rootGraph().drawingTopology_;
        }
        receiveGreenLogicTrigger(entry, input);
        return;
    case RubyCode:
        receiveRuby(entry, input);
        return;
    case MatrixRouting:
        receiveMatrixTrigger(entry, input);
        return;
    case ProcessTrigger:
        receiveTriggerToolPulse(entry, input);
        return;
    case MemoryOperation:
        receiveMemoryTrigger(entry, input);
        return;
    case SerialDevice:
        receiveSerialDevice(entry, input);
        return;
    case MidiDevice:
        receiveMidiDeviceTrigger(entry, input);
        return;
    case SequencerTrack:
        receiveSequenceMidi(entry);
        return;
    case MidiOperation:
    case MidiStream:
        receiveMidiTrigger(entry, input);
        return;
    case NotesFilter:
        routeNoteLayers(entry, input);
        return;
    case VoiceRouting:
        receiveVoiceRouting(entry, input);
        return;
    case PresetUtility:
        receivePresetUtility(entry, input);
        return;
    case ManagePresets:
        receivePresetManager(entry, input);
        return;
    case PresetParameter:
        receivePresetParameter(entry, input);
        return;
    case PluginHost:
        receivePluginHost(entry, input);
        return;
    case HostMidi:
        receiveHostMidiTrigger(entry, input);
        return;
    case VoiceSignal:
        receiveVoiceSignal(entry, input);
        return;
    case SampleTime:
        entry.localTime = sampleLocalTime();
        break;
    case Timer:
        receiveTimerTrigger(entry, input);
        return;
    case SystemInfo:
        if (entry.kind == PrimitiveKind::SystemFolders) {
            fireOutput(entry, 2);
        } else {
            refreshSystemFonts();
            fireOutputs(id);
        }
        return;
    case StateMachineLogic:
        receiveStateMachine(entry, input);
        return;
    case SystemSettings:
        return;
    case PersistSettings:
        receiveSettingsStore(entry, input);
        return;
    case SystemAction:
        receiveSystemAction(entry, input);
        return;
    case MediaSource:
        receiveMedia(entry, input);
        return;
    case NetworkTransfer:
        receiveNetwork(entry, input);
        return;
    case ParseSfz:
        receiveSfz(entry, input);
        return;
    case MouseCaptureControl:
        receiveMouseCapture(entry, input);
        return;
    case HttpTransfer:
        receiveHttpRequest(entry, input);
        return;
    case EmbedFonts:
        if (input == 1) receiveFontEmbed(entry);
        return;
    case ParseColorMatrix:
        if (input == 1) receiveColorMatrix(entry);
        return;
    case FileUtility:
        receiveFileUtility(entry, input);
        return;
    case TextFile: {
        const bool loading = entry.kind == PrimitiveKind::TextLoad;
        const int pathInput = loading ? 0 : 1;
        if (input != pathInput + 1) return;

        const auto path = std::get<std::string>(readInput(id, pathInput));
        if (path.empty() || path.find('\0') != std::string::npos) {
            throw std::invalid_argument("Choose a valid text file path");
        }
        const auto& files = rootGraph().textFiles_;
        if (loading) {
            if (!files.load) throw std::runtime_error("Text file loading is unavailable");
            entry.localValue = files.load(path);
        } else {
            if (!files.save) throw std::runtime_error("Text file saving is unavailable");
            const auto text = std::get<std::string>(readInput(id, 0));
            files.save(path, text);
        }
        fireOutputs(id);
        return;
    }
    case Blocker:
        return;
    case Switch:
        if (input != 0 || !std::get<bool>(readInput(id, 1))) {
            return;
        }
        break;
    case SendWireless:
        if (input == 0) {
            auto* scope = isModuleWirelessOutput(entry.kind) ? parent_ : this;
            if (scope) scope->publishWireless({this, id});
        }
        return;
    case ReceiveWireless:
        // Changing Name rebinds the route after the current local pulse finishes.
        return;
    case ModuleGui:
        if (input == borderInput) {
            return;
        }
        break;
    case Window:
        receiveWindowTrigger(entry, input);
        return;
    case SetGuiArea:
        receiveSetGuiArea(entry, input);
        return;
    case DropList:
        receiveDropListTrigger(entry, input);
        return;
    case IndexSelector:
        receiveIndexSelectorTrigger(entry, input);
        return;
    case EditControl:
        receiveEditControlTrigger(entry, input);
        return;
    case PopupList:
        receivePopupListTrigger(entry, input);
        return;
    case MouseArea:
    case MouseInfo:
        return;
    case ParseMouseMessage:
        receiveMouseMessage(entry);
        return;
    case ShowCursor:
        if (input == 0) {
            receiveShowCursor(id);
        }
        return;
    case MouseDrag:
        receiveMouseDrag(entry, input);
        return;
    case DragFiles:
        receiveDragFilesTrigger(entry, input);
        return;
    case MouseMove:
        return;
    case DropFiles:
        if (entry.fileDrag && entry.fileDrag->preview) {
            publishFileDrag(entry, std::nullopt, false);
        }
        return;
    case LoadBitmap:
        if (input == bitmapLoadInput) {
            loadBitmapIntoNode(id, std::get<std::string>(readInput(id, bitmapPathInput)));
        } else if (input == bitmapClearInput) {
            storeBitmap(id, {});
        }
        return;
    case LoadBitmapArray:
        if (input == bitmapArrayLoadInput) {
            if (entry.kind == PrimitiveKind::BitmapArrayFromFile) {
                loadBitmapIntoNode(id, std::get<std::string>(readInput(id, bitmapPathInput)));
            } else {
                const auto index = std::get<std::int32_t>(readInput(id, bitmapArrayIndexInput));
                storeBitmapArrayEntry(entry, index, std::get<yellow::Bitmap>(readInput(id, 0)));
            }
        } else if (input == (entry.kind == PrimitiveKind::BitmapArrayFromFile ? bitmapArrayClearInput : 4)) {
            entry.localValue = yellow::BitmapArray{};
            fireOutput(entry, 0);
        }
        return;
    case BitmapOperation:
        receiveBitmapOperation(entry, input);
        return;
    case BitmapCanvas:
        renderBitmap(entry, input);
        return;
    case CreateBus:
        if (input >= firstBusChannelInput) {
            fireBusChannel(entry, input);
        }
        return;
    case ExtractBus:
        if (input == extractBusInput) {
            receiveBusPulse(entry);
        }
        return;
    case Compare:
        if (spec.outputs.front().type == Type::Boolean || compareInputs(entry)) {
            fireOutput(entry, 0);
        }
        return;
    case ValueBox:
    case Binary:
    case UnaryMath:
    case NoteConversion:
    case MakePoint:
    case ViewLoop:
        fireOutput(entry, 0);
        return;
    case GeneralProcessing:
        receiveGeneralPulse(entry, input);
        return;
    case WaveFile:
        receiveWaveFile(entry, input);
        return;
    case PanelUtility:
        receivePanelUtility(entry);
        return;
    case PresentationControl:
        receivePresentationControl(entry, input);
        return;
    case KeyboardInput:
        if (entry.kind == PrimitiveKind::IsKeyPressed && input == 1) fireOutput(entry, 0);
        return;
    case Redraw:
        receiveRedraw(entry, input);
        return;
    case ViewCache:
        if (input == 1) {
            clearViewCache(entry);
        } else {
            fireOutput(entry, 0);
        }
        return;
    case TransformView:
        if (entry.kind != PrimitiveKind::ViewSmooth || input == 0) {
            fireOutput(entry, 0);
        }
        return;
    case DspCode:
    case PolyBridge:
    case StreamMath:
    case StreamConversion:
    case StreamRouting:
        return; // Green controls are read when rendering.
    case Tick:
        return; // The clock reads TPS when it schedules the next pulse.
    case AudioControl:
        receiveAudioControl(entry, input);
        return;
    case AudioEndpoint:
        if (entry.kind == PrimitiveKind::ClearAudio) clearAudio();
        return;
    case CaptureSignal:
        if (static_cast<std::size_t>(input) == entry.inputTriggers.size() - 1) {
            captureSignals(entry);
        }
        return;
    case SampleMono:
        if (input == 1) {
            sampleMono(entry);
        }
        return;
    case ProcessArray:
        receiveArrayToolTrigger(entry, input);
        return;
    case StoreArray:
        receiveStoredArrayTrigger(entry, input);
        return;
    case GetArrayElement:
        if (input == arrayGetInput) {
            if (const auto type = inputType(id, 0)) {
                const auto index = static_cast<std::size_t>(std::get<std::int32_t>(readInput(id, arrayIndexInput)));
                entry.localValue = withGreenArray(*type, [&]<class Array>() -> Value {
                    auto values = std::get<Array>(readInput(id, 0));
                    return index < values.size() ? std::move(values[index]) : typename Array::value_type{};
                });
                fireOutput(entry, 0);
            }
            fireOutput(entry, 1);
        }
        return;
    case TransformArray:
        if (static_cast<std::size_t>(input) == spec.inputs.size() - 1) {
            if (const auto type = inputType(id, 0)) {
                entry.localValue = transformArray(id, entry.kind, *type);
                fireOutput(entry, 0);
            }
        }
        return;
    case BuildArray:
    case AppendArrays:
        fireOutput(entry, 0);
        return;
    case ViewAttributes:
    case ViewEffect:
    case BitmapMetadata:
    case MeasureText:
    case AreaGeometry:
    case ProcessString:
    case DrawView:
    case SplitColor:
    case MakeColor:
    case ColorMatrixSet:
    case ApplyColorMatrix:
    case MakePen:
    case MakeSolid:
    case PenInfo:
    case MakeBrush:
    case MakeFont:
    case FontAttributes:
    case MakeStringFormat:
    case StringFormatAttributes:
    case TriggerButton:
    case ModuleInput:
    case PropertyInput:
    case SplitPoint:
        break;
    }
    fireOutputs(id);
}

void Schematic::rebuildAfterRemoval()
{
    try {
        rebuildConnectedRuntimes();
    } catch (const std::invalid_argument&) {
        // The remaining links were valid with their previous types. Keep
        // those types if re-inference would break any of the remaining links.
        rebuildRuntime(true);
    }
    auto& root = rootGraph();
    if (root.hiddenCursor_ && !root.nativeCursorVisibility().hidden) {
        root.hiddenCursor_.reset();
    }
}

void Schematic::rebuildRuntime(bool keepTypes, bool commit)
{
    auto build = buildRuntime(keepTypes);
    if (commit) {
        installRuntime(std::move(build));
    }
}

Schematic::RuntimeBuild Schematic::buildRuntime(bool keepTypes)
{
    RuntimeBuild build;
    if (keepTypes) {
        for (const auto& entry : nodes_) {
            auto types = ports_.at(entry.id).types;
            if (hasExpandableInputs(entry.kind)) {
                auto incoming = links_ | std::views::filter([&](auto link) { return link.target == entry.id; });
                types.inputs.resize(expandablePortCount(incoming, &SchematicLink::input, firstExpandableInput(entry.kind)),
                                    types.inputs.back());
            }
            if (hasExpandableOutputs(entry.kind)) {
                auto outgoing = links_ | std::views::filter([&](auto link) { return link.source == entry.id; });
                types.outputs.resize(expandableOutputCount(entry, outgoing), types.outputs.front());
            }
            build.ports.emplace(entry.id, NodePorts{{}, {}, std::move(types)});
        }
    } else {
        build.ports = resolvePorts();
    }
    wireRuntime(build);
    return build;
}

void Schematic::wireRuntime(RuntimeBuild& build)
{
    auto& [runtime, ports] = build;
    runtime = std::make_unique<green::Runtime>();
    for (const auto& entry : nodes_) {
        auto& port = ports.at(entry.id);
        port = makePorts(*runtime, entry, std::move(port.types));
    }
    for (auto link : links_) {
        wire(*runtime, ports, link);
    }
}

void Schematic::installRuntime(RuntimeBuild build)
{
    auto& root = rootGraph();
    ++root.audioTopology_;
    root.dspTraces_.clear();
    root.monoPreviews_.clear();
    auto& [runtime, ports] = build;
    for (auto& entry : nodes_) {
        if (hasExpandableInputs(entry.kind)) {
            entry.inputTriggers.resize(ports.at(entry.id).types.inputs.size());
        }
        if (hasExpandableOutputs(entry.kind)) {
            entry.outputTriggers.resize(ports.at(entry.id).types.outputs.size());
        }
        const auto behavior = primitive(entry.kind).behavior;
        if ((behavior == Blocker || behavior == GetArrayElement || behavior == TransformArray || behavior == SampleMono ||
             entry.kind == PrimitiveKind::ArrayInterpolatedGet || entry.kind == PrimitiveKind::Changed ||
             entry.kind == PrimitiveKind::SampleAndHold) && entry.localValue &&
            ports.at(entry.id).types.outputs.front() != green::typeOf(*entry.localValue)) {
            entry.localValue.reset();
        }
        if (entry.greenLoopValue &&
            ports.at(entry.id).types.outputs[0] != green::typeOf(entry.greenLoopValue->value)) {
            entry.greenLoopValue.reset();
        }
        if (entry.arrayStorage) {
            const auto type = ports.at(entry.id).types.outputs[storageArrayOutput(entry.kind)];
            const bool retainsType = type && withGreenArray(*type, [&]<class Array> {
                return std::holds_alternative<std::deque<typename Array::value_type>>(*entry.arrayStorage);
            });
            if (!retainsType) entry.arrayStorage.reset();
        }
    }
    runtime_ = std::move(runtime);
    ports_ = std::move(ports);
}

std::map<int, Schematic::NodePorts> Schematic::resolvePorts(const RuntimeBuilds* staged) const
{
    std::map<int, NodePorts> ports;
    std::map<int, std::vector<SchematicLink>> incoming;
    std::map<int, std::vector<SchematicLink>> outgoing;
    std::map<int, std::size_t> pending;
    std::vector<int> ready;
    // Fixed outputs stay typed when their inputs are wired through feedback.
    const auto dependsOnInputs = [](PrimitiveKind kind) {
        return kind != PrimitiveKind::Module && kind != PrimitiveKind::AboutToClose &&
               kind != PrimitiveKind::TriggerThread && kind != PrimitiveKind::CustomTimer &&
               kind != PrimitiveKind::VoicesToPoly && !isDspEditor(kind) && !isStreamDelay(kind);
    };
    for (const auto& entry : nodes_) {
        pending.emplace(entry.id, 0);
    }
    for (auto link : links_) {
        incoming[link.target].push_back(link);
        outgoing[link.source].push_back(link);
        if (dependsOnInputs(node(link.target).kind)) {
            ++pending.at(link.target);
        }
    }
    for (const auto& [id, count] : pending) {
        if (count == 0) {
            ready.push_back(id);
        }
    }
    for (std::size_t i = 0; i < ready.size(); ++i) {
        for (auto link : outgoing[ready[i]]) {
            if (dependsOnInputs(node(link.target).kind) && --pending.at(link.target) == 0) {
                ready.push_back(link.target);
            }
        }
    }

    if (ready.size() != nodes_.size()) {
        throw std::invalid_argument("This link would create a feedback loop");
    }

    // Propagate downstream hints back through unresolved template chains.
    std::map<int, std::optional<Type>> hints;
    for (auto id : ready | std::views::reverse) {
        const auto& entry = node(id);
        const auto outputs = entry.outputPorts();
        if (std::ranges::all_of(outputs, [](const Port& port) { return port.type.has_value(); })) {
            continue;
        }
        for (auto link : outgoing[id]) {
            if (outputs.at(link.output).type) {
                continue;
            }
            const auto& destination = node(link.target);
            const auto inputs = destination.inputPorts();
            const auto fixed = static_cast<std::size_t>(link.input) < inputs.size()
                ? inputs[link.input].type : std::nullopt;
            auto hint = fixed ? fixed : destination.typeOverride;
            if (!hint) {
                hint = hints[link.target];
                if (hint && destination.kind == PrimitiveKind::ArrayBuilder) {
                    hint = arrayElement(*hint);
                } else if (hint && destination.kind == PrimitiveKind::ArrayGet) {
                    hint = arrayForElement(*hint);
                }
            }
            if (hint && hasGenericArrayInput(destination.kind) && !fixed && !destination.typeOverride) {
                hint = arrayForElement(*hint);
            }
            if (hint && destination.kind == PrimitiveKind::ArrayFixedSize && link.input == arrayFixedSizeDefaultInput) {
                hint = arrayElement(*hint);
            }
            if (hint && primitive(destination.kind).behavior == StoreArray &&
                link.input == (destination.kind == PrimitiveKind::ArrayHistory ? historyValueInput : storeValueInput)) {
                hint = arrayElement(*hint);
            }
            if (hint && isQueueOrStack(entry.kind) && link.output == 0) {
                hint = arrayForElement(*hint);
            }
            // A trigger-only destination does not constrain a value source.
            if (fixed == Type::Trigger) {
                if (primitive(entry.kind).behavior == ReceiveWireless || entry.kind == PrimitiveKind::ModuleInputProperty ||
                    isIndexedRouter(entry.kind) || entry.kind == PrimitiveKind::Matrix) {
                    hints.try_emplace(id, Type::Trigger);
                }
                continue;
            }
            const auto acceptsOutputHint = [&](Type type) {
                switch (entry.kind) {
                case PrimitiveKind::ViewArrayLoop:
                case PrimitiveKind::ArrayGet:
                case PrimitiveKind::ArrayInterpolatedGet:
                case PrimitiveKind::ArrayLoop:
                case PrimitiveKind::ArraySplitter:
                case PrimitiveKind::ArraySum: return arrayForElement(type).has_value();
                case PrimitiveKind::ArrayBuilder: return isArray(type);
                case PrimitiveKind::NoteNameToInt:
                case PrimitiveKind::IntToNoteName: return noteInputForOutput(entry.kind, type).has_value();
                default: return supportsTemplateType(entry.kind, type);
                }
            };
            const bool supported = hint && acceptsOutputHint(*hint);
            if (supported &&
                (*hint != Type::Trigger || primitive(entry.kind).behavior == Switch ||
                 primitive(entry.kind).behavior == ReceiveWireless || entry.kind == PrimitiveKind::ModuleInputProperty ||
                 isIndexedRouter(entry.kind) || entry.kind == PrimitiveKind::Matrix)) {
                hints[id] = hint;
                break;
            }
        }
    }
    for (auto id : ready) {
        const auto& entry = node(id);
        const auto inputCount = hasExpandableInputs(entry.kind)
            ? expandablePortCount(incoming[id], &SchematicLink::input, firstExpandableInput(entry.kind))
            : entry.inputPorts().size();
        std::vector<std::vector<Type>> sources(inputCount);
        if (dependsOnInputs(entry.kind)) {
            for (auto link : incoming[id]) {
                if (const auto type = ports.at(link.source).types.outputs.at(link.output)) {
                    sources[link.input].push_back(*type);
                }
            }
        }
        auto types = resolveTypes(entry, sources, hints[id]);
        if (hasExpandableOutputs(entry.kind)) {
            types.outputs.resize(expandableOutputCount(entry, outgoing[id]), types.outputs.front());
        }
        if (entry.kind == PrimitiveKind::BusExtract) {
            if (const auto source = findBusSource(id)) {
                // A source behind a module boundary may not have been visited in this pass.
                const auto& building = !staged || (source->graph == this && ports.contains(source->id))
                    ? ports : staged->at(source->graph).ports;
                for (std::size_t i = 0; i < entry.channelNames.size(); ++i) {
                    types.outputs[i] = busChannelType(*source, entry.channelNames[i], building, staged != nullptr);
                }
            }
        }
        ports.emplace(id, NodePorts{{}, {}, std::move(types)});
    }
    return ports;
}

void Schematic::wire(green::Runtime& runtime, const std::map<int, NodePorts>& ports, SchematicLink link)
{
    const auto& source = ports.at(link.source);
    const auto& target = ports.at(link.target);
    const auto sourceType = source.types.outputs.at(link.output);
    const auto targetType = target.types.inputs.at(link.input);
    const auto output = source.outputs.at(link.output);
    const auto input = target.inputs.at(link.input);
    if (!output || !input) {
        const auto kind = node(link.target).kind;
        const auto& spec = primitive(kind);
        const bool captureInput = spec.behavior == CaptureSignal;
        const bool signalOperator = supportsStreamMath(spec) || spec.behavior == StreamConversion;
        const bool routing = spec.behavior == StreamRouting || spec.behavior == MatrixRouting || isIndexedRouter(kind);
        const bool acceptsConstant = isDspEditor(kind) || isPolyBridge(kind) ||
                                     captureInput || signalOperator ||
                                     (routing && kind != PrimitiveKind::Unpack && kind != PrimitiveKind::UnpackDouble);
        const bool constantInput = acceptsConstant && sourceType && targetType && canSupplyDspInput(*sourceType, *targetType);
        if (sourceType != targetType && !constantInput) {
            throw std::invalid_argument("DSP links require matching connector types");
        }
        if (kind == PrimitiveKind::VoicesToPoly && link.input == 1 &&
            std::ranges::count_if(links_, [link](auto wire) { return wire.target == link.target && wire.input == link.input; }) > 1) {
            throw std::invalid_argument("Voice retirement accepts one signal source");
        }
        return;
    }
    if (sourceType && *sourceType != Type::Trigger && !targetType) {
        throw std::invalid_argument("This primitive does not support the connected type");
    }
    runtime.connect(*output, *input, [this, link] {
        ++linkTriggers_.at(link);
        ++node(link.target).inputTriggers[link.input];
    });
}

void Schematic::armCloseHolds(std::exception_ptr& error)
{
    for (auto& entry : nodes_) {
        if (entry.kind == PrimitiveKind::AboutToClose) {
            try {
                entry.holdingClose = std::get<bool>(readInput(entry.id, closeHoldInput));
            } catch (const std::exception&) {
                if (!error) error = std::current_exception();
            }
        }
    }
    for (auto& [id, child] : modules_) child->armCloseHolds(error);
}

bool Schematic::hasCloseHolds() const
{
    return std::ranges::any_of(nodes_, &SchematicNode::holdingClose) ||
           std::ranges::any_of(modules_, [](const auto& child) { return child.second->hasCloseHolds(); });
}

void Schematic::fireLifecycleTriggers(PrimitiveKind kind, std::span<SchematicNode> entries, std::exception_ptr& error)
{
    for (auto& entry : entries) {
        if (entry.kind == kind) {
            try {
                fireOutput(entry, 0);
                refreshConnections();
            } catch (const std::exception&) {
                if (!error) error = std::current_exception();
            }
        } else if (auto* child = module(entry.id)) {
            child->fireLifecycleTriggers(kind, child->nodes_, error);
        }
    }
}

void Schematic::notifyPrimitiveOutputs(PrimitiveKind kind, std::optional<std::size_t> output)
{
    for (auto& entry : nodes_) {
        if (entry.kind != kind) continue;
        if (output) fireOutput(entry, *output);
        else fireOutputs(entry.id);
    }
    for (auto& [id, module] : modules_) module->notifyPrimitiveOutputs(kind, output);
}

void Schematic::fireOutputs(int id)
{
    auto& entry = node(id);
    for (std::size_t index = 0; index < entry.outputTriggers.size(); ++index) {
        fireOutput(entry, index);
    }
}

void Schematic::fireOutput(SchematicNode& entry, std::size_t index)
{
    const auto output = ports_.at(entry.id).outputs.at(index);
    if (!output) {
        return;
    }
    ++triggers_;
    ++entry.outputTriggers.at(index);
    runtime_->fire(*output);
}

}
