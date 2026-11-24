#include "green/ArrayOperations.hpp"
#include "green/Runtime.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace openstone::green;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template<class Error, class Action>
void checkThrows(Action action, std::string_view message)
{
    try {
        action();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void triggerBlockerFlow()
{
    Runtime runtime;
    const auto cutoff = runtime.addOutput(440.0F);
    const auto blocked = runtime.addOutput(440.0F);
    const auto apply = runtime.addTriggerOutput();
    std::vector<std::string_view> trace;
    std::vector<float> appliedValues;

    const auto blockerInput = runtime.addInput(0.0F, [&](Runtime& rt, InputId input) {
        trace.push_back("blocker reads cutoff");
        rt.store(blocked, rt.pull(input));
    });
    const auto receiverValue = runtime.addInput(0.0F, [&](Runtime&, InputId) {
        trace.push_back("unexpected value notification");
    });
    const auto receiverApply = runtime.addTriggerInput([&](Runtime& rt, InputId) {
        trace.push_back("apply reads cutoff");
        appliedValues.push_back(std::get<float>(rt.pull(receiverValue)));
    });

    runtime.connect(cutoff, blockerInput);
    runtime.connect(blocked, receiverValue);
    runtime.connect(apply, receiverApply);

    check(std::get<float>(runtime.pull(receiverValue)) == 440.0F, "Expected the initial cutoff");
    runtime.store(cutoff, 660.0F);
    check(trace.empty(), "Storing a Green value fired a trigger");
    check(std::get<float>(runtime.pull(receiverValue)) == 440.0F, "Blocker changed before its input fired");

    runtime.fire(cutoff);
    check(trace == std::vector<std::string_view>{"blocker reads cutoff"},
          "The blocker forwarded the cutoff notification");
    check(std::get<float>(runtime.pull(receiverValue)) == 660.0F,
          "The blocker did not expose the first notified value");

    runtime.store(cutoff, 880.0F);
    runtime.fire(cutoff);
    check(appliedValues.empty(), "Cutoff changed before Apply fired");
    check(std::get<float>(runtime.pull(receiverValue)) == 880.0F,
          "The blocker made the latest value unavailable to pull");

    runtime.fire(apply);
    runtime.fire(apply);
    check(appliedValues == std::vector{880.0F, 880.0F}, "Each Apply must pull the latest cutoff");
    check(trace == std::vector<std::string_view>{
              "blocker reads cutoff", "blocker reads cutoff", "apply reads cutoff", "apply reads cutoff"},
          "Unexpected Trigger Blocker cascade");
}

void connectorTypesAndLinkOrder()
{
    Runtime runtime;
    const auto fallback = runtime.addInput(std::int32_t{3});
    const auto ints = runtime.addOutput(std::int32_t{9});
    const auto trigger = runtime.addTriggerOutput();
    const auto triggerInput = runtime.addTriggerInput({});
    check(typeOf(runtime.pull(fallback)) == Type::Int && std::get<std::int32_t>(runtime.pull(fallback)) == 3,
          "Unconnected inputs must supply their declared default");
    checkThrows<std::invalid_argument>([&] { runtime.store(trigger, 1.0F); }, "Trigger output accepted a value");
    checkThrows<std::invalid_argument>([&] { (void)runtime.pull(triggerInput); }, "Trigger input returned a value");
    checkThrows<std::invalid_argument>([&] { runtime.store(ints, 1.0F); }, "Store changed the output type");
    runtime.connect(ints, fallback);
    check(std::get<std::int32_t>(runtime.pull(fallback)) == 9, "Connected input used its default");

    const auto a = runtime.addOutput(std::string{"A"});
    const auto b = runtime.addOutput(std::string{"B"});
    const auto text = runtime.addInput(std::string{});
    runtime.connect(b, text);
    runtime.connect(a, text);
    check(std::get<std::string>(runtime.pull(text)) == "BA", "String fan-in ignored link order");
    checkThrows<std::invalid_argument>([&] { runtime.connect(a, text); }, "Accepted a duplicate link");
}

void mixedValueLinks()
{
    Runtime runtime;
    const auto checkConversion = [&](Value source, Value fallback, Value expected) {
        const auto output = runtime.addOutput(std::move(source));
        const auto input = runtime.addInput(std::move(fallback));
        runtime.connect(output, input);
        check(runtime.pull(input) == expected, "Mixed Green value conversion returned the wrong value or type");
    };

    using namespace openstone::yellow;
    checkConversion(Color{0x80123456}, PaintTool{PenStroke{}}, PaintTool{SolidBrush{Color{0x80123456}}});
    auto format = stringFormatFromFlags(stringFormatFlagMask);
    format.horizontal = HorizontalAlignment::Right; format.vertical = VerticalAlignment::Center;
    checkConversion(std::string("31783,right,center"), StringFormat{}, format);
    checkConversion(format, std::string{}, std::string("31783,right,center"));
    checkConversion(std::string("0,left,top"), format, StringFormat{});
    check(parseStringFormat(stringFormatStyleName(format), "right", "center") == format,
          "All String Format flags survive their named style representation");
    for (const auto& text : {"0,left", "0,left,top,extra", "65535,left,top"})
        checkThrows<std::invalid_argument>([&] { findValueConverter(Type::String, Type::StringFormat)(std::string(text)); }, "Accepted invalid String Format text");
    checkConversion(-3.9F, std::int32_t{}, std::int32_t{-3});
    checkConversion(std::int32_t{-3}, 0.0F, -3.0F);
    checkConversion(std::int32_t{-3}, false, true);
    checkConversion(std::int32_t{0}, true, false);
    checkConversion(true, std::int32_t{}, std::int32_t{1});
    checkConversion(false, std::int32_t{1}, std::int32_t{0});
    checkConversion(std::string("True"), false, true);
    checkConversion(std::string("true"), false, true);
    checkConversion(std::string("TRUE"), false, true);
    checkConversion(std::string("tRuE"), false, true);
    checkConversion(std::string(" TRUE"), true, false);
    checkConversion(std::string("true "), true, false);
    checkConversion(std::string("Tr ue"), true, false);
    checkConversion(std::string("False"), true, false);
    for (const auto* text : {"on", "oN", "yes", "YeS", "y", "Y"}) {
        checkConversion(std::string(text), false, true);
    }
    for (const auto* text : {" on", "yes ", " y ", "only"}) {
        checkConversion(std::string(text), true, false);
    }
    checkConversion(true, std::string{}, std::string("True"));
    checkConversion(false, std::string{}, std::string("False"));
    checkConversion(std::string("10.567"), 0.0F, 10.567F);
    checkConversion(std::string("10.567"), std::int32_t{}, std::int32_t{10});
    checkConversion(std::string("1 2 3"), std::int32_t{99}, std::int32_t{1});
    checkConversion(std::string("12 3"), std::int32_t{99}, std::int32_t{12});
    checkConversion(std::string("+1"), std::int32_t{99}, std::int32_t{1});
    checkConversion(std::string("-1"), std::int32_t{99}, std::int32_t{-1});
    checkConversion(std::string("--1"), std::int32_t{99}, std::int32_t{0});
    checkConversion(std::string(" +12.5 "), 0.0F, 12.5F);
    checkConversion(std::string("abc"), 1.0F, 0.0F);
    checkConversion(10.567F, std::string{}, std::string("10.567"));
    checkConversion(std::int32_t{-3}, std::string{}, std::string("-3"));
    checkConversion(std::string("1.3\r\n2.1\n3.0\n4.9\n5.6\n"), FloatArray{},
                    FloatArray{1.3F, 2.1F, 3.0F, 4.9F, 5.6F});
    checkConversion(FloatArray{3.3F, 4.1F, 5.0F, 6.9F, 7.6F}, std::string{},
                    std::string("3.300000\n4.100000\n5.000000\n6.900000\n7.600000"));
    checkConversion(IntArray{10, 11, 12}, std::string{}, std::string("10\n11\n12"));
    checkConversion(std::string("10\r\n11\n12"), IntArray{}, IntArray{10, 11, 12});
    checkConversion(StringArray{"test1cool1", "test2cool2", "test3"}, std::string{},
                    std::string("test1cool1\ntest2cool2\ntest3"));

    const auto text = runtime.addOutput(std::string("unknown"));
    const auto number = runtime.addOutput(1.0F);
    const auto boolean = runtime.addInput(false);
    checkThrows<std::invalid_argument>([&] { runtime.connect(number, boolean); },
                                       "Float to Boolean inferred an undocumented conversion");
    runtime.connect(text, boolean);
    check(!std::get<bool>(runtime.pull(boolean)), "Unrecognized Boolean text did not yield False");
    runtime.store(text, std::string("True"));
    check(std::get<bool>(runtime.pull(boolean)), "String converter did not reread the source");

    const auto integer = runtime.addInput(std::int32_t{});
    runtime.connect(number, integer);
    for (const float invalid : {2147483648.0F, std::numeric_limits<float>::infinity(),
                                 std::numeric_limits<float>::quiet_NaN()}) {
        runtime.store(number, invalid);
        checkThrows<std::out_of_range>([&] { (void)runtime.pull(integer); },
                                       "Non-representable Float converted to Int");
    }
    runtime.store(number, -2147483648.0F);
    check(std::get<std::int32_t>(runtime.pull(integer)) == std::numeric_limits<std::int32_t>::min(),
          "A failed conversion changed the link or rejected a valid endpoint");
}

void floatRange()
{
    constexpr float limit = 1.0e38F;
    constexpr float infinity = std::numeric_limits<float>::infinity();
    const float beyond = std::nextafter(limit, infinity);
    Runtime runtime;

    const auto value = runtime.addOutput(beyond);
    int notifications = 0;
    const auto input = runtime.addInput(-beyond, [&](Runtime&, InputId) { ++notifications; });
    const auto pull = [&](InputId id) { return std::get<float>(runtime.pull(id)); };
    check(pull(input) == -infinity, "Float fallback beyond the negative limit did not become -inf");
    runtime.connect(value, input);
    check(pull(input) == infinity, "Initial Float beyond the positive limit did not become +inf");
    for (const float boundary : {limit, -limit}) {
        runtime.store(value, boundary);
        check(pull(input) == boundary, "Float at the limit did not stay finite");
    }
    runtime.store(value, -beyond);
    check(pull(input) == -infinity, "Stored Float beyond the negative limit did not become -inf");
    check(notifications == 0, "Storing -inf unexpectedly fired a trigger");
    runtime.fire(value);
    check(notifications == 1, "A Float infinity prevented trigger delivery");

    const auto computed = runtime.addComputedOutput(Type::Float, [=](const Runtime&) -> Value {
        return beyond;
    });
    const auto computedText = runtime.addInput(std::string{});
    runtime.connect(computed, computedText);
    check(std::get<std::string>(runtime.pull(computedText)) == "inf",
          "Computed Float did not become infinity before conversion to String");

    const auto text = runtime.addOutput(std::string{});
    const auto parsed = runtime.addInput(0.0F);
    runtime.connect(text, parsed);
    for (const auto& [source, expected] : {std::pair{"1e38", limit}, std::pair{"-1e38", -limit},
                                           std::pair{"+1.1e38 Hz", infinity},
                                           std::pair{"-1.1e38", -infinity},
                                           std::pair{"1e400", infinity}, std::pair{"-1e400", -infinity}}) {
        runtime.store(text, std::string(source));
        check(pull(parsed) == expected, "String to Float returned the wrong value at or beyond the limit");
    }
    runtime.store(text, std::string("1e-400"));
    checkThrows<std::out_of_range>([&] { (void)runtime.pull(parsed); },
                                   "Float text underflow was treated as overflow");

    const auto left = runtime.addOutput(limit);
    const auto right = runtime.addOutput(limit);
    const auto sum = runtime.addInput(0.0F);
    runtime.connect(left, sum);
    runtime.connect(right, sum);
    check(pull(sum) == infinity, "Finite Float sources did not produce +inf when their sum exceeded the limit");
    runtime.store(left, -limit);
    runtime.store(right, -limit);
    check(pull(sum) == -infinity, "Finite Float sources did not produce -inf when their sum exceeded the limit");
    const auto offset = runtime.addOutput(limit);
    runtime.connect(offset, sum);
    check(pull(sum) == -limit, "Float fan-in applied the limit before summing all its sources");
}

void mixedTriggerLinks()
{
    Runtime runtime;
    const auto ticker = runtime.addTriggerOutput();
    const auto value = runtime.addOutput(std::int32_t{913});
    std::vector<std::int32_t> reads;
    int notifications = 0;
    const auto input = runtime.addInput(std::int32_t{42}, [&](Runtime& rt, InputId id) {
        reads.push_back(std::get<std::int32_t>(rt.pull(id)));
    });
    const auto triggerInput = runtime.addTriggerInput([&](Runtime&, InputId) { ++notifications; });
    runtime.connect(ticker, input);
    runtime.fire(ticker);
    check(reads == std::vector<std::int32_t>{42}, "A pulse-only source replaced the input default");

    runtime.connect(value, input);
    runtime.connect(value, triggerInput);
    runtime.store(value, std::int32_t{914});
    check(reads.size() == 1 && notifications == 0, "A silent store sent a pulse");
    runtime.fire(ticker);
    runtime.fire(ticker);
    runtime.fire(value);
    check(reads == std::vector<std::int32_t>{42, 914, 914, 914},
          "Pulse-only source did not reread the typed source");
    check(notifications == 1, "Value output did not deliver its pulse to the Trigger input");
    checkThrows<std::invalid_argument>([&] { runtime.connect(ticker, input); },
                                       "Accepted a duplicate pulse-only link");
}

void mixedValueFanIn()
{
    Runtime runtime;
    const auto left = runtime.addOutput(2.25F);
    const auto right = runtime.addOutput(std::int32_t{3});
    const auto pulse = runtime.addTriggerOutput();
    std::vector<float> sums;
    const auto input = runtime.addInput(100.0F, [&](Runtime& rt, InputId id) {
        sums.push_back(std::get<float>(rt.pull(id)));
    });
    runtime.connect(left, input);
    runtime.connect(pulse, input);
    runtime.connect(right, input);
    runtime.fire(left);
    runtime.store(right, std::int32_t{-4});
    runtime.fire(pulse);
    runtime.fire(right);
    check(sums == std::vector{5.25F, -1.75F, -1.75F},
          "Float fan-in did not sum the current, converted values on each pulse");

    const auto text = runtime.addOutput(std::string("Count="));
    const auto label = runtime.addInput(std::string("fallback"));
    runtime.connect(text, label);
    runtime.connect(right, label);
    check(std::get<std::string>(runtime.pull(label)) == "Count=-4",
          "String fan-in ignored conversion or link order");

    const auto extra = runtime.addOutput(0.75F);
    runtime.connect(extra, input);
    check(std::get<float>(runtime.pull(input)) == -1.0F, "Float fan-in ignored a Float source");
}

void parsesStringsBeforeFloatFanIn()
{
    Runtime runtime;
    const auto first = runtime.addOutput(std::string{"1.25"});
    const auto second = runtime.addOutput(std::string{"2.5"});
    const auto half = runtime.addOutput(0.5F);
    const auto sum = runtime.addInput(0.0F);

    runtime.connect(first, sum);
    runtime.connect(second, sum);
    check(std::get<float>(runtime.pull(sum)) == 3.75F,
          "Float fan-in did not parse each String before adding");

    runtime.connect(half, sum);
    check(std::get<float>(runtime.pull(sum)) == 4.25F,
          "Float fan-in did not add the native Float to the parsed Strings");
}

void convertsFloatsBeforeIntFanIn()
{
    Runtime runtime;
    const auto first = runtime.addOutput(1.75F);
    const auto second = runtime.addOutput(2.75F);
    const auto ten = runtime.addOutput(std::int32_t{10});
    const auto firstOnly = runtime.addInput(std::int32_t{0});
    const auto secondOnly = runtime.addInput(std::int32_t{0});
    const auto sum = runtime.addInput(std::int32_t{0});

    runtime.connect(first, firstOnly);
    runtime.connect(second, secondOnly);
    check(std::get<std::int32_t>(runtime.pull(firstOnly)) == 1 &&
              std::get<std::int32_t>(runtime.pull(secondOnly)) == 2,
          "Single Float links did not truncate toward zero at Int inputs");

    runtime.connect(first, sum);
    runtime.connect(second, sum);
    check(std::get<std::int32_t>(runtime.pull(sum)) == 3,
          "Int fan-in did not convert the two Floats before adding");

    runtime.connect(ten, sum);
    check(std::get<std::int32_t>(runtime.pull(sum)) == 13,
          "Int fan-in did not add the native Int to the converted Floats");
}

void parsesStringsBeforeIntFanIn()
{
    Runtime runtime;
    const auto first = runtime.addOutput(std::string{"12"});
    const auto second = runtime.addOutput(std::string{"34"});
    const auto ten = runtime.addOutput(std::int32_t{10});
    const auto sum = runtime.addInput(std::int32_t{});

    runtime.connect(first, sum);
    runtime.connect(second, sum);
    check(std::get<std::int32_t>(runtime.pull(sum)) == 46,
          "Int fan-in did not parse each String before adding");

    runtime.connect(ten, sum);
    check(std::get<std::int32_t>(runtime.pull(sum)) == 56,
          "Int fan-in did not add the native Int to the parsed Strings");
}

void intFanIn()
{
    Runtime runtime;
    const auto left = runtime.addOutput(std::int32_t{2});
    const auto right = runtime.addOutput(std::int32_t{3});
    std::vector<std::int32_t> reads;
    const auto input = runtime.addInput(std::int32_t{99}, [&](Runtime& rt, InputId id) {
        reads.push_back(std::get<std::int32_t>(rt.pull(id)));
    });
    runtime.connect(left, input);
    runtime.connect(right, input);
    runtime.fire(left);
    runtime.store(right, std::int32_t{-3});
    runtime.fire(right);
    check(reads == std::vector<std::int32_t>{5, -1}, "Int input did not sum its current sources");

    constexpr auto max = std::numeric_limits<std::int32_t>::max();
    constexpr auto min = std::numeric_limits<std::int32_t>::min();
    runtime.store(left, std::int32_t{2'147'483'646});
    runtime.store(right, std::int32_t{2});
    runtime.fire(left);
    check(reads.back() == min, "Int overflow did not wrap to -2147483648 as in FlowStone");
    runtime.store(left, min);
    runtime.store(right, std::int32_t{-1});
    runtime.fire(right);
    check(reads == std::vector<std::int32_t>{5, -1, min, max},
          "Int underflow did not wrap to 2147483647 on the next trigger");
}

void booleanFanIn()
{
    Runtime runtime;
    const auto left = runtime.addOutput(false);
    const auto right = runtime.addOutput(false);
    const auto joined = runtime.addOutput(false);
    const auto boolean = runtime.addInput(true, [joined](Runtime& rt, InputId id) {
        rt.store(joined, rt.pull(id));
        rt.fire(joined);
    });
    const auto integer = runtime.addInput(std::int32_t{});
    const auto joinedValue = runtime.addInput(false);
    int leftPulses = 0;
    int rightPulses = 0;
    std::vector<bool> joinedReads;
    const auto leftCounter = runtime.addTriggerInput([&](Runtime&, InputId) { ++leftPulses; });
    const auto rightCounter = runtime.addTriggerInput([&](Runtime&, InputId) { ++rightPulses; });
    const auto joinedCounter = runtime.addTriggerInput([&](Runtime& rt, InputId) {
        joinedReads.push_back(std::get<bool>(rt.pull(joinedValue)));
    });
    runtime.connect(left, boolean);
    runtime.connect(right, boolean);
    runtime.connect(left, integer);
    runtime.connect(right, integer);
    runtime.connect(left, leftCounter);
    runtime.connect(right, rightCounter);
    runtime.connect(joined, joinedValue);
    runtime.connect(joined, joinedCounter);

    check(!std::get<bool>(runtime.pull(boolean)), "Two False sources produced True at a Boolean input");
    runtime.store(left, true);
    runtime.fire(left);
    check(std::get<std::int32_t>(runtime.pull(integer)) == 1, "True and False did not sum to 1 at an Int input");
    runtime.store(right, true);
    runtime.fire(right);
    check(std::get<std::int32_t>(runtime.pull(integer)) == 2, "Two True sources did not sum to 2 at an Int input");
    check(leftPulses == 1 && rightPulses == 1, "Each Boolean source should send one pulse to its own counter");
    check(joinedReads == std::vector<bool>{true, true}, "Boolean fan-in did not pass both pulses while remaining True");
}

void convertsIntsBeforeBooleanFanIn()
{
    Runtime runtime;
    const auto positive = runtime.addOutput(std::int32_t{1});
    const auto negative = runtime.addOutput(std::int32_t{-1});
    const auto input = runtime.addInput(false);
    runtime.connect(positive, input);
    runtime.connect(negative, input);

    check(std::get<bool>(runtime.pull(input)), "Opposite Ints canceled at a Boolean input");
    runtime.store(positive, std::int32_t{0});
    check(std::get<bool>(runtime.pull(input)), "A negative Int paired with zero did not produce True");
    runtime.store(negative, std::int32_t{0});
    check(!std::get<bool>(runtime.pull(input)), "Two zero Ints did not produce False");
}

void convertsStringsBeforeBooleanFanIn()
{
    Runtime runtime;
    const auto first = runtime.addOutput(std::string{"False"});
    const auto second = runtime.addOutput(std::string{"True"});
    const auto combined = runtime.addInput(false);
    const auto joinedText = runtime.addOutput(std::string{"FalseTrue"});
    const auto single = runtime.addInput(true);
    runtime.connect(first, combined);
    runtime.connect(second, combined);
    runtime.connect(joinedText, single);

    check(std::get<bool>(runtime.pull(combined)), "Separate False and True links did not yield True");
    check(!std::get<bool>(runtime.pull(single)), "A single FalseTrue String did not yield False");
    runtime.store(second, std::string{"False"});
    check(!std::get<bool>(runtime.pull(combined)), "Two False Strings did not yield False");
}

void stringAndIntArrayOperations()
{
    using enum ArrayOperation;
    const StringArray words{"test1", "test2", "test3"};
    check(applyArrayOperation(Add, words, StringArray{"cool1", "cool2", "cool3"}) ==
              Value{StringArray{"test1cool1", "test2cool2", "test3cool3"}},
          "String Array addition did not concatenate matching entries");
    check(applyArrayOperation(Add, words, StringArray{"cool1", "cool2"}) ==
              Value{StringArray{"test1cool1", "test2cool2", "test3"}},
          "String Array addition lost the unmatched third entry");

    const IntArray a{1, 2, 3};
    const IntArray b{10, 11};
    check(applyArrayOperation(Add, a, IntArray{10, 11, 12}) == Value{IntArray{11, 13, 15}},
          "Int Array addition paired the wrong entries");
    check(applyArrayOperation(Add, a, b) == Value{IntArray{11, 13, 3}},
          "Int Array addition did not pad the shorter operand with zero");
    check(applyArrayOperation(Multiply, a, b) == Value{IntArray{10, 22, 0}},
          "Int Array multiplication did not pad the shorter operand with zero");
    check(applyArrayOperation(Subtract, a, b) == Value{IntArray{-9, -9, 3}} &&
              applyArrayOperation(Subtract, b, a) == Value{IntArray{9, 9, -3}},
          "Int Array subtraction reversed the operands or used the wrong padding");
    check(applyArrayOperation(Divide, a, b) == Value{IntArray{0, 0, 0}},
          "Int Array division A / B did not truncate fractions or use zero for a missing divisor");
    check(applyArrayOperation(Divide, b, a) == Value{IntArray{10, 5, 0}},
          "Int Array division B / A did not truncate fractions or use zero for a missing numerator");

    constexpr auto min = std::numeric_limits<std::int32_t>::min();
    check(applyArrayOperation(Divide, IntArray{-11, 3, min}, IntArray{2, 0, -1}) ==
              Value{IntArray{-5, 0, min}},
          "Int Array division mishandled negative quotients, zero divisors, or overflow");
}

void floatArrayOperations()
{
    using enum ArrayOperation;
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    const FloatArray a{0.1F, 0.2F, 0.3F};
    const FloatArray b{1.1F, 1.2F};
    const auto checkResult = [](const Value& result, const FloatArray& expected, std::string_view message) {
        const auto& actual = std::get<FloatArray>(result);
        check(actual.size() == expected.size(), message);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            check(actual[i] == expected[i] || std::abs(actual[i] - expected[i]) < 0.00001F, message);
        }
    };
    checkResult(applyArrayOperation(Add, a, FloatArray{1.1F, 1.2F, 1.3F}), {1.2F, 1.4F, 1.6F},
                "Float Array addition paired the wrong entries");
    checkResult(applyArrayOperation(Add, a, b), {1.2F, 1.4F, 0.3F}, "Float Array addition");
    checkResult(applyArrayOperation(Multiply, a, b), {0.11F, 0.24F, 0}, "Float Array multiplication");
    checkResult(applyArrayOperation(Subtract, a, b), {-1, -1, 0.3F}, "Float Array subtraction A - B");
    checkResult(applyArrayOperation(Subtract, b, a), {1, 1, -0.3F}, "Float Array subtraction B - A");
    checkResult(applyArrayOperation(Divide, a, b), {0.0909091F, 0.166667F, infinity},
                "Float Array division A / B");
    checkResult(applyArrayOperation(Divide, b, a), {11, 6, 0}, "Float Array division B / A");
}

void pointArrayOperations()
{
    using enum ArrayOperation;
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    const PointArray a{{1, 2}, {3, 4}, {5, 6}};
    const PointArray b{{10, 11}, {12, 13}};
    check(applyArrayOperation(Add, a, PointArray{{10, 11}, {12, 13}, {14, 15}}) ==
              Value{PointArray{{11, 13}, {15, 17}, {19, 21}}},
          "Point Array addition paired the wrong coordinates");
    check(applyArrayOperation(Add, a, b) == Value{PointArray{{11, 13}, {15, 17}, {5, 6}}},
          "Point Array addition lost the unmatched point");
    check(applyArrayOperation(Multiply, a, b) == Value{PointArray{{10, 22}, {36, 52}, {0, 0}}},
          "Point Array multiplication did not multiply corresponding coordinates");
    check(applyArrayOperation(Subtract, a, b) == Value{PointArray{{-9, -9}, {-9, -9}, {5, 6}}},
          "Point Array subtraction A - B");
    check(applyArrayOperation(Subtract, b, a) == Value{PointArray{{9, 9}, {9, 9}, {-5, -6}}},
          "Point Array subtraction B - A");

    // Route a text readout back into a typed operand, as in the FlowStone schematic.
    Runtime runtime;
    const auto source = runtime.addOutput(a);
    const auto left = runtime.addInput(PointArray{});
    const auto text = runtime.addOutput(std::string{"10, 11\r\n12, 13"});
    const auto right = runtime.addInput(PointArray{});
    const auto quotient = runtime.addComputedOutput(Type::PointArray, [=](const Runtime& rt) {
        return applyArrayOperation(Divide, rt.pull(left), rt.pull(right));
    });
    const auto result = runtime.addInput(PointArray{});
    const auto readout = runtime.addInput(std::string{});
    runtime.connect(source, left);
    runtime.connect(text, right);
    runtime.connect(quotient, result);
    runtime.connect(quotient, readout);
    check(runtime.pull(result) == Value{PointArray{{0.1F, 2.0F / 11}, {0.25F, 4.0F / 13}, {infinity, infinity}}},
          "Point Array division A / B");
    check(std::get<std::string>(runtime.pull(readout)) == "0.1, 0.181818\n0.25, 0.307692\ninf, inf",
          "Point Array division readout did not preserve coordinates and infinity");
    check(applyArrayOperation(Divide, b, a) == Value{PointArray{{10, 5.5F}, {4, 3.25F}, {0, 0}}},
          "Point Array division B / A");
}

void arrayZeroDivisors()
{
    constexpr auto inf = std::numeric_limits<float>::infinity();
    constexpr auto divide = ArrayOperation::Divide;

    const IntArray ints{6, -6, 0, 8};
    check(applyArrayOperation(divide, ints, IntArray{2, 2, 2, 2}) == Value{IntArray{3, -3, 0, 4}},
          "Int Array division with nonzero divisors differed from FlowStone");
    check(applyArrayOperation(divide, ints, IntArray{0, 0, 0, 2}) == Value{IntArray{0, 0, 0, 4}},
          "Int Array division with explicit zero divisors differed from FlowStone");

    const FloatArray floats{6, -6, 0, 8};
    check(applyArrayOperation(divide, floats, FloatArray{0, 0, 0, 2}) == Value{FloatArray{inf, inf, inf, 4}},
          "Float Array division by zero did not give positive infinity for every numerator");
    check(applyArrayOperation(divide, PointArray{{6, -6}, {0, 8}}, PointArray{{0, 0}, {0, 2}}) ==
              Value{PointArray{{inf, inf}, {inf, 4}}},
          "Point Array division by zero did not give positive infinity per coordinate");
    check(applyArrayOperation(divide, floats, 0.0F) == Value{FloatArray{inf, inf, inf, inf}},
          "A broadcast Float zero did not use the array zero-divisor rule");
}

void arrayScalarOperations()
{
    using enum ArrayOperation;
    const IntArray numbers{1, 2, 3};
    check(applyArrayOperation(Add, numbers, std::int32_t{1}) == Value{IntArray{2, 3, 4}},
          "Int Array addition did not apply the scalar to every element");
    check(applyArrayOperation(Multiply, numbers, std::int32_t{4}) == Value{IntArray{4, 8, 12}},
          "Int Array multiplication did not apply the scalar to every element");
    check(applyArrayOperation(Subtract, numbers, std::int32_t{3}) == Value{IntArray{-2, -1, 0}} &&
              applyArrayOperation(Subtract, std::int32_t{3}, numbers) == Value{IntArray{2, 1, 0}},
          "Int Array subtraction reversed the scalar and array operands");
    check(applyArrayOperation(Divide, numbers, 2.0F) == Value{FloatArray{0.5F, 1, 1.5F}} &&
              applyArrayOperation(Divide, 4.0F, numbers) == Value{FloatArray{4, 2, 4.0F / 3.0F}},
          "A Float scalar in division did not preserve fractions and operand order");
    check(applyArrayOperation(Divide, numbers, std::int32_t{2}) == Value{IntArray{0, 1, 1}},
          "An Int scalar in Int Array division did not use integer division");

    const StringArray words{"test1", "test2", "test3"};
    const std::string text{"add"};
    check(applyArrayOperation(Add, words, text) == Value{StringArray{"test1add", "test2add", "test3add"}} &&
              applyArrayOperation(Add, text, words) == Value{StringArray{"addtest1", "addtest2", "addtest3"}},
          "String Array addition did not concatenate the scalar in operand order");
    check(applyArrayOperation(Add, FloatArray{0.5F, 1.5F}, std::int32_t{2}) == Value{FloatArray{2.5F, 3.5F}},
          "Float Array addition did not promote the Int scalar");
    check(applyArrayOperation(Subtract, Point{10, 20}, PointArray{{1, 2}, {3, 4}}) ==
              Value{PointArray{{9, 18}, {7, 16}}}, "Point Array subtraction did not broadcast both coordinates");
    check(applyArrayOperation(Divide, IntArray{}, 2.0F) == Value{FloatArray{}},
          "Broadcasting to an empty array produced an element");
    check(applyArrayOperation(Add, numbers, IntArray{1}) == Value{IntArray{2, 2, 3}},
          "A single-element array was treated as a scalar");
    checkThrows<std::invalid_argument>([&] { (void)applyArrayOperation(Add, std::int32_t{1}, 2.0F); },
                                       "Array operations accepted two scalar operands");
}

void scalarArrayOperandConversion()
{
    using enum ArrayOperation;
    Runtime runtime;
    const auto numbers = runtime.addOutput(IntArray{1, 2, 3});
    const auto arrayInput = runtime.addInput(IntArray{});
    const auto two = runtime.addOutput(std::int32_t{2});
    const auto divisor = runtime.addInput(0.0F);
    const auto three = runtime.addOutput(3.5F);
    const auto subtrahend = runtime.addInput(std::int32_t{0});
    runtime.connect(numbers, arrayInput);
    runtime.connect(two, divisor);
    runtime.connect(three, subtrahend);

    const auto quotient = runtime.addComputedOutput(Type::FloatArray, [=](const Runtime& rt) {
        return applyArrayOperation(Divide, rt.pull(arrayInput), rt.pull(divisor));
    });
    const auto difference = runtime.addComputedOutput(Type::IntArray, [=](const Runtime& rt) {
        return applyArrayOperation(Subtract, rt.pull(arrayInput), rt.pull(subtrahend));
    });
    const auto quotientInput = runtime.addInput(FloatArray{});
    const auto differenceInput = runtime.addInput(IntArray{});
    runtime.connect(quotient, quotientInput);
    runtime.connect(difference, differenceInput);
    check(runtime.pull(quotientInput) == Value{FloatArray{0.5F, 1, 1.5F}},
          "An Int source connected to a Float divisor input did not yield a Float Array");
    check(runtime.pull(differenceInput) == Value{IntArray{-2, -1, 0}},
          "A Float source connected to an Int operand was not converted before subtraction");
}

void arrayFanIn()
{
    Runtime runtime;
    const auto checkSum = [&runtime](auto left, auto right, auto expected, std::string_view message) {
        const auto rightOutput = runtime.addOutput(std::move(right));
        const auto leftOutput = runtime.addOutput(std::move(left));
        const auto input = runtime.addInput(decltype(expected){});
        runtime.connect(leftOutput, input);
        runtime.connect(rightOutput, input);
        check(runtime.pull(input) == Value{std::move(expected)}, message);
    };
    checkSum(StringArray{"left", "tail"}, StringArray{"right"}, StringArray{"leftright", "tail"},
             "String Array input did not concatenate by index in link order");
    checkSum(FloatArray{0.25F}, FloatArray{0.5F, 1.0F}, FloatArray{0.75F, 1.0F},
             "Float Array input did not add by index and keep the longer array's length");
    checkSum(PointArray{{1, 2}, {5, 6}}, PointArray{{10, 11}}, PointArray{{11, 13}, {5, 6}},
             "Point Array input did not add corresponding coordinates");

    const auto a = runtime.addOutput(IntArray{1, 2, 3});
    const auto b = runtime.addOutput(IntArray{10, 11});
    const auto c = runtime.addOutput(IntArray{20, 30, 40, 50});
    std::vector<IntArray> reads;
    const auto input = runtime.addInput(IntArray{}, [&](Runtime& rt, InputId id) {
        reads.push_back(std::get<IntArray>(rt.pull(id)));
    });
    runtime.connect(a, input);
    runtime.connect(b, input);
    runtime.connect(c, input);
    runtime.fire(a);
    runtime.fire(b);
    runtime.store(c, IntArray{20});
    runtime.fire(c);
    check(reads == std::vector<IntArray>{{31, 43, 43, 50}, {31, 43, 43, 50}, {31, 13, 3}},
          "Int Array input did not sum all sources on each pulse or drop the old tail");
}

void computedValuesPullUpstream()
{
    Runtime runtime;
    const auto left = runtime.addOutput(3.0F);
    const auto right = runtime.addOutput(std::int32_t{9});
    const auto apply = runtime.addTriggerOutput();
    const auto scaleInput = runtime.addInput(0.0F);
    runtime.connect(left, scaleInput);

    int reads = 0;
    const auto doubled = runtime.addComputedOutput(Type::Float, [&](const Runtime& rt) -> Value {
        ++reads;
        return 2 * std::get<float>(rt.pull(scaleInput));
    });
    const auto sumInput = runtime.addInput(0.0F);
    runtime.connect(doubled, sumInput);
    runtime.connect(right, sumInput);
    const auto sum = runtime.addComputedOutput(Type::Float, [=](const Runtime& rt) {
        return rt.pull(sumInput);
    });

    std::vector<std::int32_t> applied;
    const auto display = runtime.addInput(std::int32_t{}, [&](Runtime& rt, InputId input) {
        applied.push_back(std::get<std::int32_t>(rt.pull(input)));
    });
    runtime.connect(sum, display);
    runtime.connect(apply, display);

    runtime.fire(apply);
    runtime.store(left, 4.25F);
    runtime.store(right, std::int32_t{10});
    check(reads == 1 && applied == std::vector<std::int32_t>{15},
          "Storing an upstream value caused a read or delivered a pulse");
    runtime.fire(apply);
    runtime.fire(sum);
    check(reads == 3 && applied == std::vector<std::int32_t>{15, 18, 18},
          "A downstream pull did not traverse computed outputs, fan-in, and conversions");
    checkThrows<std::invalid_argument>([&] { runtime.store(sum, 0.0F); },
                                       "Store replaced a computed output");
}

void sharedReadDependency()
{
    Runtime runtime;
    const auto shared = runtime.addComputedOutput(Type::Float, [](const Runtime&) -> Value {
        return 3.0F;
    });
    const auto sharedInput = runtime.addInput(0.0F);
    runtime.connect(shared, sharedInput);

    const auto left = runtime.addComputedOutput(Type::Float, [=](const Runtime& rt) {
        return rt.pull(sharedInput);
    });
    const auto right = runtime.addComputedOutput(Type::Float, [=](const Runtime& rt) {
        return rt.pull(sharedInput);
    });
    const auto result = runtime.addInput(0.0F);
    runtime.connect(left, result);
    runtime.connect(right, result);
    check(std::get<float>(runtime.pull(result)) == 6.0F,
          "Two branches could not pull the same upstream output");
}

void computedReadFailuresLeaveRuntimeUsable()
{
    Runtime runtime;
    const auto leftInput = runtime.addInput(0.0F);
    const auto rightInput = runtime.addInput(0.0F);
    bool useFeedback = true;
    const auto left = runtime.addComputedOutput(Type::Float, [&](const Runtime& rt) -> Value {
        return useFeedback ? rt.pull(leftInput) : Value{7.0F};
    });
    const auto right = runtime.addComputedOutput(Type::Float, [=](const Runtime& rt) {
        return rt.pull(rightInput);
    });
    runtime.connect(right, leftInput);
    runtime.connect(left, rightInput);
    std::string diagnostic;
    try {
        (void)runtime.pull(rightInput);
    } catch (const std::runtime_error& error) {
        diagnostic = error.what();
    }
    check(diagnostic.find("0 -> 1 -> 0") != std::string::npos, "Read cycle did not report its output path");
    useFeedback = false;
    check(std::get<float>(runtime.pull(rightInput)) == 7.0F, "Failed read left a stale cycle");

    const auto wrongType = runtime.addComputedOutput(Type::Float, [](const Runtime&) -> Value {
        return std::int32_t{1};
    });
    const auto wrongInput = runtime.addInput(0.0F);
    runtime.connect(wrongType, wrongInput);
    checkThrows<std::logic_error>([&] { (void)runtime.pull(wrongInput); },
                                  "Reader returned a different type than its connector declared");

    const auto pulse = runtime.addTriggerOutput();
    bool delivered = false;
    runtime.connect(pulse, runtime.addTriggerInput([&](Runtime&, InputId) { delivered = true; }));
    const auto firingOutput = runtime.addComputedOutput(Type::Float, [&](const Runtime&) -> Value {
        runtime.fire(pulse);
        return 0.0F;
    });
    const auto firingInput = runtime.addInput(0.0F);
    runtime.connect(firingOutput, firingInput);
    checkThrows<std::logic_error>([&] { (void)runtime.pull(firingInput); }, "Read could fire a pulse");
    check(!delivered, "A pull notified a trigger handler");
    runtime.fire(pulse);
    check(delivered, "A failed read prevented the next explicit pulse");
}

void orderedTriggerBranches()
{
    Runtime runtime;
    const auto root = runtime.addTriggerOutput();
    const auto left = runtime.addTriggerOutput();
    const auto right = runtime.addTriggerOutput();
    std::vector<std::string_view> trace;

    const auto branch = [&](std::string_view name, OutputId output) {
        return runtime.addTriggerInput([&, name, output](Runtime& rt, InputId) {
            trace.push_back(name);
            rt.fire(output);
        });
    };
    const auto join = runtime.addTriggerInput([&](Runtime&, InputId) { trace.push_back("join"); });
    runtime.connect(root, branch("left", left));
    runtime.connect(root, branch("right", right));
    runtime.connect(left, join);
    runtime.connect(right, join);
    runtime.fire(root);
    check(trace == std::vector<std::string_view>{"left", "join", "right", "join"},
          "Branch order changed or separate deliveries were merged");
}

void loopLeavesRuntimeUsable()
{
    Runtime runtime;
    const auto loop = runtime.addTriggerOutput();
    const auto listener = runtime.addTriggerInput([=](Runtime& rt, InputId) { rt.fire(loop); });
    runtime.connect(loop, listener);
    checkThrows<std::runtime_error>([&] { runtime.fire(loop); }, "Trigger feedback must terminate");

    const auto manual = runtime.addTriggerOutput();
    bool delivered = false;
    runtime.connect(manual, runtime.addTriggerInput([&](Runtime&, InputId) { delivered = true; }));
    runtime.fire(manual);
    check(delivered, "A failed cascade left the runtime unable to dispatch");
}

void yellowValuesAndTriggers()
{
    using openstone::yellow::Area;
    using openstone::yellow::View;
    Runtime runtime;
    const auto text = runtime.addOutput(std::string("1.5, -2, +7, -8.25"));
    const auto area = runtime.addInput(Area{});
    runtime.connect(text, area);
    check(runtime.pull(area) == Value{Area{1.5F, -2.0F, 7.0F, -8.25F}},
          "Each Area coordinate should parse from the String in grid units");
    runtime.store(text, std::string("1, 2, 3,"));
    checkThrows<std::invalid_argument>([&] { (void)runtime.pull(area); },
                                       "Area text should reject missing coordinates");

    const View initial{4, 0, {0, 0, 16, 10}, false};
    const auto panel = runtime.addOutput(initial);
    int notifications = 0;
    const auto target = runtime.addInput(View{}, [&](Runtime&, InputId) { ++notifications; });
    const auto pulse = runtime.addTriggerOutput();
    runtime.connect(panel, target);
    runtime.connect(pulse, target);
    check(runtime.pull(target) == Value{initial} && notifications == 0,
          "Pulling a View should read the target without delivering an event");
    runtime.fire(pulse);
    check(runtime.pull(target) == Value{initial} && notifications == 1,
          "A pulse-only connection should leave the current View intact");
    checkThrows<std::invalid_argument>([&] { runtime.connect(text, target); },
                                       "A String is not convertible to a View");
}

void mouseMessagesDeliverRepeatedEvents()
{
    using openstone::yellow::MouseButton;
    using openstone::yellow::MouseEventKind;
    using openstone::yellow::MouseMessage;

    Runtime runtime;
    const auto source = runtime.addOutput(MouseMessage{});
    std::vector<MouseMessage> received;
    const auto target = runtime.addInput(MouseMessage{}, [&](Runtime& active, InputId input) {
        received.push_back(std::get<MouseMessage>(active.pull(input)));
    });
    runtime.connect(source, target);
    const auto secondSource = runtime.addOutput(MouseMessage{});
    checkThrows<std::invalid_argument>([&] { runtime.connect(secondSource, target); },
                                       "Mouse inputs should not merge event sources");

    const MouseMessage doubleClick{MouseEventKind::DoubleClick, {3.5F, -1.25F}, MouseButton::Right};
    runtime.store(source, doubleClick);
    check(received.empty() && runtime.readConnection(source, target) == Value{doubleClick},
          "Storing a mouse message should preserve its position and button without notifying receivers");
    runtime.fire(source);

    const MouseMessage wheel{MouseEventKind::Wheel, {-2.5F, 6.25F}, MouseButton::None, {15, -120}};
    runtime.store(source, wheel);
    runtime.fire(source);
    runtime.store(source, wheel);
    runtime.fire(source);
    check(received == std::vector{doubleClick, wheel, wheel},
          "Mouse delivery should preserve both wheel axes and deliver identical events separately");
}

void midiRelatedValuesDeliverEachTrigger()
{
    const NoteEvent note{2, 64, 100, true};
    const std::array values{
        std::pair{Type::MIDI, Value{MidiMessage{{0x91, 64, 100}}}},
        std::pair{Type::Notes, Value{Notes{{note}}}},
        std::pair{Type::Voices, Value{Voices{{VoiceEvent{12, note}}}}},
        std::pair{Type::Preset, Value{PresetProgramChange{7}}},
    };
    for (const auto& [type, value] : values) {
        Runtime runtime;
        check(typeOf(value) == type, "Event payload must keep its connector type");
        int deliveries = 0;
        const auto input = runtime.addInput(value, [&](auto& graph, InputId id) {
            check(graph.pull(id) == value, "An event trigger must expose its typed payload");
            ++deliveries;
        });
        const auto output = runtime.addOutput(value);
        runtime.connect(output, input);
        runtime.store(output, value);
        check(deliveries == 0, "Storing an event value must leave trigger delivery explicit");
        runtime.fire(output);
        runtime.fire(output);
        check(deliveries == 2, "Repeated equal events must both deliver triggers");
    }
}

}

int main()
{
    try {
        triggerBlockerFlow();
        connectorTypesAndLinkOrder();
        mixedValueLinks();
        floatRange();
        mixedTriggerLinks();
        mixedValueFanIn();
        parsesStringsBeforeFloatFanIn();
        convertsFloatsBeforeIntFanIn();
        parsesStringsBeforeIntFanIn();
        intFanIn();
        booleanFanIn();
        convertsIntsBeforeBooleanFanIn();
        convertsStringsBeforeBooleanFanIn();
        stringAndIntArrayOperations();
        floatArrayOperations();
        pointArrayOperations();
        arrayZeroDivisors();
        arrayScalarOperations();
        scalarArrayOperandConversion();
        arrayFanIn();
        computedValuesPullUpstream();
        sharedReadDependency();
        computedReadFailuresLeaveRuntimeUsable();
        orderedTriggerBranches();
        loopLeavesRuntimeUsable();
        yellowValuesAndTriggers();
        mouseMessagesDeliverRepeatedEvents();
        midiRelatedValuesDeliverEachTrigger();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
