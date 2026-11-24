#pragma once

#include "green/Value.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace openstone::green {

enum class InputId : std::size_t {};
enum class OutputId : std::size_t {};

// Apply the fan-in rule for additional sources of the same type.
void combineInputValue(Value& result, const Value& value);

// A runtime belongs to one control thread. Connector IDs are local to that runtime.
class Runtime {
public:
    using TriggerHandler = std::function<void(Runtime&, InputId)>;
    using ValueReader = std::function<Value(const Runtime&)>;
    using DeliveryHandler = std::function<void()>;

    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    InputId addInput(Value defaultValue, TriggerHandler onTrigger = {});
    InputId addTriggerInput(TriggerHandler onTrigger);
    OutputId addOutput(Value initialValue);
    // Evaluated on each pull. Readers may pull inputs and must return the declared type.
    OutputId addComputedOutput(Type type, ValueReader reader);
    OutputId addTriggerOutput();

    // Links are visited in connection order. Trigger-only links are ignored by pull.
    // The optional observer runs immediately before the destination's trigger handler.
    void connect(OutputId source, InputId target, DeliveryHandler onDelivery = {});

    // Store changes the value available to pull. Fire sends a pulse on either kind of output.
    void store(OutputId output, Value value);
    [[nodiscard]] Value pull(InputId input) const;
    // Read one link after conversion, before the input combines its sources.
    [[nodiscard]] std::optional<Value> readConnection(OutputId source, InputId target) const;
    void fire(OutputId output);
    // Deliver a pulse along a single existing link without firing the source's other links.
    void fireConnection(OutputId source, InputId target);
    [[nodiscard]] bool busy() const { return triggerDepth_ != 0 || !activeReads_.empty(); }

private:
    struct ValueLink {
        OutputId source;
        ValueConverter convert;
    };

    struct Input {
        std::optional<Value> defaultValue;
        TriggerHandler onTrigger;
        std::vector<ValueLink> valueSources;
        std::optional<OutputId> lastEventSource;
    };

    struct Delivery {
        InputId target;
        OutputId source;
        const Delivery* previous;
    };

    struct Output {
        Type type;
        std::variant<std::monostate, Value, ValueReader> source;
        struct Connection {
            InputId target;
            DeliveryHandler onDelivery;
        };
        std::vector<Connection> targets;
    };

    [[nodiscard]] Value readLink(const ValueLink& link) const;
    void dispatch(OutputId source, std::span<const Output::Connection> connections);
    void requireNotReading() const;
    void requireIdle() const;

    std::vector<Input> inputs_;
    std::vector<Output> outputs_;
    mutable std::vector<OutputId> activeReads_;
    const Delivery* activeDelivery_ = nullptr;
    std::size_t triggerDepth_ = 0;
    std::size_t deliveriesLeft_ = 0;
};

}
