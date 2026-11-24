#pragma once

#include "stream/Runtime.hpp"
#include "stream/VoiceBank.hpp"

namespace openstone::stream {

enum class PolyValue { External, Mono, Frequency, Pitch, Velocity, Gate, Trigger, Channel, Slot, Note, Tag, ChannelController };
enum class PolyMix { Sum, Minimum, Maximum, Hop, SelectGroup };

struct PolyBinding {
    PolyValue value;
    std::size_t input{};
    std::optional<std::size_t> index;
};

// One snapshot per voice bank in a prepared audio graph. Reducers on
// separate output paths use the same notes at each block boundary.
class VoiceSnapshot {
public:
    explicit VoiceSnapshot(std::shared_ptr<const VoiceBank> bank);
    [[nodiscard]] std::size_t capacity() const noexcept { return voices_.size(); }
    [[nodiscard]] std::span<const VoiceSlot> read(std::uint64_t frame) noexcept;
    void reset() noexcept { frame_.reset(); }

private:
    std::shared_ptr<const VoiceBank> bank_;
    std::vector<VoiceSlot> voices_;
    std::vector<VoiceSlot> scratch_;
    std::optional<std::uint64_t> frame_;
};

class PolyRenderer final : public SampleProcessor {
public:
    PolyRenderer(const Graph& graph, std::span<const NodeId> outputs, std::vector<PolyBinding> bindings,
                 std::shared_ptr<VoiceSnapshot> snapshot, bool managed, PolyMix mix,
                 std::size_t sampleRateInput, std::optional<std::size_t> control);

    void beginBlock(std::uint64_t frame) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] std::string compiledDspCode() const override;
    [[nodiscard]] double process(std::span<const double> external) noexcept override;
    [[nodiscard]] bool setDspArrayInput(DspBlockId block, std::size_t input, std::span<const float> values) noexcept;
    [[nodiscard]] bool setDspMemArrayInput(DspBlockId block, std::size_t input, const green::MemoryArray& values) noexcept;
    [[nodiscard]] bool setDspMemRefInput(DspBlockId block, std::size_t input, const green::Memory& value) noexcept;
    [[nodiscard]] bool setDspEnginePointer(DspBlockId block, std::string_view name, const green::Memory& memory, bool writable = true);
    [[nodiscard]] bool setDspEnginePointerTable(DspBlockId block, std::string_view name, const DspEnginePointerTable& table);

private:
    struct Instance {
        explicit Instance(MonoDoubleRuntime compiled)
            : runtime(std::move(compiled)), inputs(runtime.inputCount()) {}
        MonoDoubleRuntime runtime;
        std::vector<double> inputs;
        std::array<double, 2> outputs{};
        std::uint64_t generation{};
        bool ended{};
        bool active{};
    };
    [[nodiscard]] double inputSample(PolyBinding binding, std::span<const double> external,
                                     const VoiceSlot& voice, std::size_t slot) const noexcept;

    std::shared_ptr<VoiceSnapshot> snapshot_;
    VoiceSlot preview_{.note = {.noteOn = true}, .id = 1, .generation = 1};
    std::span<const VoiceSlot> voices_;
    std::vector<Instance> instances_;
    std::vector<MonoDoubleRuntime::VoicePacket> packets_;
    std::vector<PolyBinding> bindings_;
    PolyMix mix_;
    std::size_t sampleRateInput_;
    std::optional<std::size_t> control_;
    bool hasReleaseEnd_;
    std::size_t frame_{};
    detail::AssemblerSampleClock sampleClock_;
    double held_{};
};

}
