#include "stream/VoiceBank.hpp"

#include <algorithm>
#include <bit>

namespace openstone::stream {
namespace {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

bool sameNote(const green::NoteEvent& a, const green::NoteEvent& b)
{
    return a.key == b.key && a.channel == b.channel && a.tags == b.tags;
}

}

VoiceBank::VoiceBank()
    : slots_(settings_.capacity),
      published_(std::make_unique<std::atomic<std::uint64_t>[]>(VoiceSettings::maxCapacity * fieldsPerSlot))
{
}

void VoiceBank::configure(VoiceSettings settings)
{
    settings.capacity = std::clamp(settings.capacity, 1, VoiceSettings::maxCapacity);
    settings.stolenPerSlot = std::clamp(settings.stolenPerSlot, 0, VoiceSettings::maxCapacity);
    if (settings == settings_) return;
    if (settings.capacity != settings_.capacity) {
        // Changing the layout starts a fresh bank, preserving the monotonic IDs.
        slots_.assign(settings.capacity, {});
        held_.clear();
    }
    settings_ = settings;
    for (auto& slot : slots_) {
        const auto keep = settings.returnStolen ? static_cast<std::size_t>(settings.stolenPerSlot) : 0;
        if (slot.stolen.size() > keep) {
            slot.stolen.erase(slot.stolen.begin(), slot.stolen.end() - keep);
        }
    }
    publish();
}

void VoiceBank::clear() noexcept
{
    held_.clear();
    for (auto& slot : slots_) {
        slot.active = {};
        slot.stolen.clear();
    }
    publish();
}

void VoiceBank::activate(VoiceSlot& slot, VoiceSlot replacement, bool retrigger)
{
    replacement.generation = retrigger ? ++nextGeneration_ : slot.generation;
    replacement.triggerCount = slot.triggerCount + retrigger;
    slot = replacement;
}

green::VoiceEvent VoiceBank::press(const green::NoteEvent& note)
{
    auto slot = std::ranges::find_if(slots_, [](const Slot& slot) { return !slot.active.id; });
    if (slot == slots_.end()) {
        slot = std::ranges::min_element(slots_, {}, [](const Slot& slot) {
            return std::pair{slot.active.note.noteOn, slot.active.id};
        });
    }
    auto& active = slot->active;
    const bool retrigger = !active.id || !active.note.noteOn || settings_.retriggerSteal;
    if (active.id && active.note.noteOn && settings_.returnStolen && settings_.stolenPerSlot > 0) {
        if (slot->stolen.size() == static_cast<std::size_t>(settings_.stolenPerSlot)) {
            slot->stolen.erase(slot->stolen.begin());
        }
        slot->stolen.push_back(active);
    }
    const green::VoiceEvent event{++nextId_, note};
    activate(active, {note, event.id}, retrigger);
    held_.push_back(event);
    return event;
}

std::optional<green::VoiceEvent> VoiceBank::release(const green::NoteEvent& note)
{
    const auto held = std::ranges::find_if(held_, [&](const auto& event) { return sameNote(event.note, note); });
    if (held == held_.end()) return {};
    const green::VoiceEvent event{held->id, note};
    held_.erase(held);
    for (auto& slot : slots_) {
        if (slot.active.id == event.id) {
            if (slot.stolen.empty()) {
                slot.active.note.noteOn = false;
            } else {
                activate(slot.active, slot.stolen.back(), settings_.retriggerReturn);
                slot.stolen.pop_back();
            }
            break;
        }
        const auto stolen = std::ranges::find(slot.stolen, event.id, &VoiceSlot::id);
        if (stolen != slot.stolen.end()) {
            slot.stolen.erase(stolen);
            break;
        }
    }
    return event;
}

std::vector<green::VoiceEvent> VoiceBank::apply(std::span<const green::NoteEvent> notes)
{
    std::vector<green::VoiceEvent> result;
    result.reserve(notes.size());
    for (const auto& note : notes) {
        if (note.channel > 15 || note.key > 127 || note.velocity > 127) continue;
        if (note.noteOn) {
            result.push_back(press(note));
        } else if (auto event = release(note)) {
            result.push_back(*event);
        }
    }
    if (!result.empty()) publish();
    return result;
}

void VoiceBank::publish() noexcept
{
    sequence_.fetch_add(1);
    auto* target = published_.get();
    const auto count = std::max(publishedSlots_, slots_.size());
    publishedSlots_ = slots_.size();
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = i < slots_.size() ? slots_[i].active : VoiceSlot{};
        *target++ = slot.id;
        *target++ = slot.generation;
        *target++ = slot.triggerCount;
        *target++ = static_cast<std::uint64_t>(slot.note.channel) | (slot.note.key << 8) |
                    (slot.note.velocity << 16) | (static_cast<unsigned>(slot.note.noteOn) << 24);
        for (const auto tag : slot.note.tags) *target++ = std::bit_cast<std::uint32_t>(tag);
    }
    sequence_.fetch_add(1);
}

void VoiceBank::read(std::span<VoiceSlot> result, std::span<VoiceSlot> scratch) const noexcept
{
    if (result.size() != scratch.size() || scratch.size() > VoiceSettings::maxCapacity) return;
    // All payload words and sequence markers are atomic. A bounded retry
    // keeps the previous complete snapshot if the writer is still active.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto sequence = sequence_.load();
        if (sequence & 1U) continue;
        const auto* source = published_.get();
        for (auto& slot : scratch) {
            slot.id = *source++;
            slot.generation = *source++;
            slot.triggerCount = *source++;
            const auto packed = source++->load();
            slot.note = {static_cast<std::uint8_t>(packed), static_cast<std::uint8_t>(packed >> 8),
                         static_cast<std::uint8_t>(packed >> 16), (packed >> 24) != 0};
            for (auto& tag : slot.note.tags) tag = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(source++->load()));
        }
        if (sequence_.load() == sequence) {
            std::ranges::copy(scratch, result.begin());
            return;
        }
    }
}

}
