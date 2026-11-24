#include "green/Runtime.hpp"

#include "green/ArrayOperations.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace openstone::green {
namespace {

constexpr std::size_t maxTriggerDepth = 128;
constexpr std::size_t maxDeliveriesPerTrigger = 65'536;
constexpr std::size_t maxReadDepth = 128;

void normalizeScalarValue(Value& value)
{
    if (auto* number = std::get_if<float>(&value)) {
        *number = normalizeFloat(*number);
    }
}

struct DispatchFrame {
    explicit DispatchFrame(std::size_t& depth) : depth(depth) { ++depth; }
    ~DispatchFrame() { --depth; }

    std::size_t& depth;
};

struct ReadFrame {
    ReadFrame(std::vector<OutputId>& stack, OutputId output) : stack(stack)
    {
        stack.push_back(output);
    }
    ~ReadFrame() { stack.pop_back(); }

    std::vector<OutputId>& stack;
};

}

InputId Runtime::addInput(Value defaultValue, TriggerHandler onTrigger)
{
    requireIdle();
    normalizeScalarValue(defaultValue);
    const auto id = static_cast<InputId>(inputs_.size());
    inputs_.push_back({std::move(defaultValue), std::move(onTrigger), {}, {}});
    return id;
}

InputId Runtime::addTriggerInput(TriggerHandler onTrigger)
{
    requireIdle();
    const auto id = static_cast<InputId>(inputs_.size());
    inputs_.push_back({std::nullopt, std::move(onTrigger), {}, {}});
    return id;
}

OutputId Runtime::addOutput(Value initialValue)
{
    requireIdle();
    normalizeScalarValue(initialValue);
    const auto id = static_cast<OutputId>(outputs_.size());
    outputs_.push_back({typeOf(initialValue), std::move(initialValue), {}});
    return id;
}

OutputId Runtime::addComputedOutput(Type type, ValueReader reader)
{
    requireIdle();
    if (!isValueType(type) || !reader) {
        throw std::invalid_argument("A computed Green output requires a value type and a reader");
    }
    const auto id = static_cast<OutputId>(outputs_.size());
    outputs_.push_back({type, std::move(reader), {}});
    return id;
}

OutputId Runtime::addTriggerOutput()
{
    requireIdle();
    const auto id = static_cast<OutputId>(outputs_.size());
    outputs_.push_back({Type::Trigger, std::monostate{}, {}});
    return id;
}

void Runtime::connect(OutputId source, InputId target, DeliveryHandler onDelivery)
{
    requireIdle();
    auto& output = outputs_.at(std::to_underlying(source));
    auto& input = inputs_.at(std::to_underlying(target));
    if (std::ranges::find(output.targets, target, &Output::Connection::target) != output.targets.end()) {
        throw std::invalid_argument("Green link already exists");
    }

    const bool busInput = input.defaultValue && typeOf(*input.defaultValue) == Type::Bus;
    if ((output.type == Type::Bus) != busInput) {
        throw std::invalid_argument("Connect a Bus output to a Bus input");
    }

    if (output.type == Type::Trigger || !input.defaultValue) {
        output.targets.push_back({target, std::move(onDelivery)});
        return;
    }

    const auto targetType = typeOf(*input.defaultValue);
    const auto convert = findValueConverter(output.type, targetType);
    if (output.type != targetType && !convert) {
        throw std::invalid_argument("Unsupported Green connector conversion");
    }
    if (hasSingleValueSource(targetType) && !deliversDiscreteEvents(targetType) && !input.valueSources.empty()) {
        throw std::invalid_argument("This input accepts one value source");
    }

    output.targets.push_back({target, std::move(onDelivery)});
    try {
        input.valueSources.push_back({source, convert});
    } catch (...) {
        output.targets.pop_back();
        throw;
    }
}

void Runtime::store(OutputId output, Value value)
{
    requireNotReading();
    auto& port = outputs_.at(std::to_underlying(output));
    auto* stored = std::get_if<Value>(&port.source);
    if (!stored || port.type != typeOf(value)) {
        throw std::invalid_argument("Green value does not match the output type");
    }
    normalizeScalarValue(value);
    *stored = std::move(value);
}

void combineInputValue(Value& result, const Value& value)
{
    if (auto* text = std::get_if<std::string>(&result)) {
        *text += std::get<std::string>(value);
    } else if (auto* integer = std::get_if<std::int32_t>(&result)) {
        *integer = addWrappedInts(*integer, std::get<std::int32_t>(value));
    } else if (auto* boolean = std::get_if<bool>(&result)) {
        *boolean = *boolean || std::get<bool>(value);
    } else if (auto* number = std::get_if<float>(&result)) {
        *number += std::get<float>(value);
    } else {
        result = applyArrayOperation(ArrayOperation::Add, result, value);
    }
}

Value Runtime::pull(InputId input) const
{
    const auto& port = inputs_.at(std::to_underlying(input));
    if (!port.defaultValue) {
        throw std::invalid_argument("Trigger inputs have no value to pull");
    }
    if (port.valueSources.empty()) {
        return *port.defaultValue;
    }
    if (deliversDiscreteEvents(typeOf(*port.defaultValue))) {
        for (auto* delivery = activeDelivery_; delivery; delivery = delivery->previous) {
            if (delivery->target == input) {
                const auto link = std::ranges::find(port.valueSources, delivery->source, &ValueLink::source);
                return link == port.valueSources.end() ? *port.defaultValue : readLink(*link);
            }
        }
        if (port.lastEventSource) {
            const auto link = std::ranges::find(port.valueSources, *port.lastEventSource, &ValueLink::source);
            return readLink(*link);
        }
        return readLink(port.valueSources.front());
    }
    auto result = readLink(port.valueSources.front());
    for (std::size_t index = 1; index < port.valueSources.size(); ++index) {
        const auto value = readLink(port.valueSources[index]);
        combineInputValue(result, value);
    }
    normalizeScalarValue(result);
    return result;
}

Value Runtime::readLink(const ValueLink& link) const
{
    const auto& output = outputs_[std::to_underlying(link.source)];
    if (const auto* value = std::get_if<Value>(&output.source)) {
        return link.convert ? link.convert(*value) : *value;
    }

    const auto first = std::ranges::find(activeReads_, link.source);
    if (first != activeReads_.end()) {
        std::string message = "Green value read cycle at outputs ";
        for (auto iter = first; iter != activeReads_.end(); ++iter) {
            message += std::to_string(std::to_underlying(*iter)) + " -> ";
        }
        throw std::runtime_error(message + std::to_string(std::to_underlying(link.source)));
    }
    if (activeReads_.size() == maxReadDepth) {
        throw std::runtime_error("Green value read exceeded its depth limit");
    }

    const ReadFrame frame(activeReads_, link.source);
    auto value = std::get<ValueReader>(output.source)(*this);
    if (typeOf(value) != output.type) {
        throw std::logic_error("Computed Green output returned a value of the wrong type");
    }
    normalizeScalarValue(value);
    return link.convert ? link.convert(value) : std::move(value);
}

std::optional<Value> Runtime::readConnection(OutputId source, InputId target) const
{
    const auto& output = outputs_.at(std::to_underlying(source));
    const auto& input = inputs_.at(std::to_underlying(target));
    if (std::ranges::find(output.targets, target, &Output::Connection::target) == output.targets.end()) {
        throw std::invalid_argument("Green link does not exist");
    }
    const auto link = std::ranges::find(input.valueSources, source, &ValueLink::source);
    return link == input.valueSources.end() ? std::nullopt : std::optional{readLink(*link)};
}

void Runtime::fire(OutputId output)
{
    dispatch(output, outputs_.at(std::to_underlying(output)).targets);
}

void Runtime::fireConnection(OutputId source, InputId target)
{
    const auto& targets = outputs_.at(std::to_underlying(source)).targets;
    const auto connection = std::ranges::find(targets, target, &Output::Connection::target);
    if (connection == targets.end()) {
        throw std::invalid_argument("Green link does not exist");
    }
    dispatch(source, std::span{&*connection, 1});
}

void Runtime::dispatch(OutputId source, std::span<const Output::Connection> connections)
{
    requireNotReading();
    if (triggerDepth_ == 0) {
        deliveriesLeft_ = maxDeliveriesPerTrigger;
    }
    if (triggerDepth_ == maxTriggerDepth) {
        throw std::runtime_error("Green trigger cascade exceeded its depth limit");
    }

    const DispatchFrame frame{triggerDepth_};
    for (const auto& connection : connections) {
        if (deliveriesLeft_ == 0) {
            throw std::runtime_error("Green trigger cascade exceeded its delivery limit");
        }
        --deliveriesLeft_;
        auto& port = inputs_[std::to_underlying(connection.target)];
        const Delivery delivery{connection.target, source, activeDelivery_};
        if (port.defaultValue && deliversDiscreteEvents(typeOf(*port.defaultValue)) &&
            std::ranges::contains(port.valueSources, source, &ValueLink::source)) {
            port.lastEventSource = source;
        }
        activeDelivery_ = &delivery;
        try {
            if (connection.onDelivery) connection.onDelivery();
            if (port.onTrigger) port.onTrigger(*this, connection.target);
        } catch (...) {
            activeDelivery_ = delivery.previous;
            throw;
        }
        activeDelivery_ = delivery.previous;
    }
}

void Runtime::requireNotReading() const
{
    if (!activeReads_.empty()) {
        throw std::logic_error("Cannot modify Green state or fire a trigger during a value read");
    }
}

void Runtime::requireIdle() const
{
    requireNotReading();
    if (triggerDepth_ != 0) {
        throw std::logic_error("Cannot change Green connections during a trigger cascade");
    }
}

}
