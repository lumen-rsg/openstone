#pragma once

#include "green/Value.hpp"

#include <atomic>
#include <span>

namespace openstone::stream {

struct VoiceSlot {
    green::NoteEvent note;
    std::uint64_t id{};
    std::uint64_t generation{};
    std::uint64_t triggerCount{};
};

struct VoiceSettings {
    static constexpr int maxCapacity = 256;
    int capacity = 32;
    int stolenPerSlot = 32;
    bool returnStolen{};
    bool retriggerSteal{};
    bool retriggerReturn{};
    bool operator==(const VoiceSettings&) const = default;
};

class VoiceBank {
public:
    VoiceBank();
    void configure(VoiceSettings settings);
    void clear() noexcept;
    [[nodiscard]] std::vector<green::VoiceEvent> apply(std::span<const green::NoteEvent> notes);
    [[nodiscard]] int capacity() const noexcept { return settings_.capacity; }

    // Called from the audio thread. The supplied scratch and result have the same size.
    void read(std::span<VoiceSlot> result, std::span<VoiceSlot> scratch) const noexcept;

private:
    struct Slot {
        VoiceSlot active;
        std::vector<VoiceSlot> stolen;
    };
    static constexpr int fieldsPerSlot = 4 + 16;
    [[nodiscard]] green::VoiceEvent press(const green::NoteEvent& note);
    [[nodiscard]] std::optional<green::VoiceEvent> release(const green::NoteEvent& note);
    void activate(VoiceSlot& slot, VoiceSlot replacement, bool retrigger);
    void publish() noexcept;

    VoiceSettings settings_;
    std::vector<Slot> slots_;
    std::vector<green::VoiceEvent> held_;
    std::uint64_t nextId_{};
    std::uint64_t nextGeneration_{};
    std::unique_ptr<std::atomic<std::uint64_t>[]> published_;
    std::atomic<std::uint64_t> sequence_{};
    std::size_t publishedSlots_{};
};

}
