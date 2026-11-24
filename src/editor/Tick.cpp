#include "Session.hpp"

#include <cmath>
#include <ctime>
#include <stdexcept>

namespace openstone {
namespace {

constexpr int timerDurationInput = 0;
constexpr int timerStopInput = 2;

TimerState startTimer(TickTime now, std::int32_t durationMs)
{
    TimerState timer{durationMs, std::nullopt};
    if (durationMs > 0) {
        timer.deadline = now + std::chrono::milliseconds(durationMs);
    }
    return timer;
}

TickTime nextPeriodicDeadline(TickTime deadline, TickTime now, std::chrono::duration<double> period)
{
    // Keep the phase and skip missed intervals after a late wake.
    const auto late = now - deadline;
    return now + period - std::chrono::duration<double>{std::fmod(late.count(), period.count())};
}

void rememberError(TickSchedule& result)
{
    if (!result.error) {
        result.error = std::current_exception();
    }
}

}

std::array<std::int32_t, 8> Schematic::sampleLocalTime() const
{
    using namespace std::chrono;
    const auto& clock = rootGraph().systemInfo_.wallClock;
    const auto now = clock ? clock() : system_clock::now();
    const auto second = floor<seconds>(now);
    const auto time = system_clock::to_time_t(second);
    std::tm local{};
#ifdef _WIN32
    const bool converted = localtime_s(&local, &time) == 0;
#else
    const bool converted = localtime_r(&time, &local) != nullptr;
#endif
    if (!converted) {
        throw std::runtime_error("Could not read local system time");
    }
    return {local.tm_wday, local.tm_mday, local.tm_mon + 1, local.tm_year + 1900,
            local.tm_hour, local.tm_min, local.tm_sec, static_cast<std::int32_t>(duration_cast<milliseconds>(now - second).count())};
}

void Schematic::receiveTimerTrigger(SchematicNode& entry, int input)
{
    if (input == timerStopInput) {
        entry.timer.reset();
        return;
    }
    if (input == timerDurationInput && !entry.timer) return;

    const auto duration = std::get<std::int32_t>(readInput(entry.id, timerDurationInput));
    const auto& root = rootGraph();
    const auto& clock = root.systemInfo_.monotonicClock;
    const auto now = root.tickDispatchTime_ ? *root.tickDispatchTime_
                                           : clock ? clock() : TickTime{std::chrono::steady_clock::now()};
    entry.timer = startTimer(now, duration);
}

void Schematic::advanceTimer(SchematicNode& entry, TickTime now, TickWake wake, TickSchedule& result)
{
    auto& timer = *entry.timer;
    const auto duration = std::get<std::int32_t>(readInput(entry.id, timerDurationInput));
    if (duration != timer.intervalMs) {
        timer = startTimer(now, duration);
    }
    if (wake == TickWake::Timer && timer.deadline && *timer.deadline <= now) {
        timer.deadline = nextPeriodicDeadline(*timer.deadline, now, std::chrono::milliseconds(duration));
        result.fired = true;
        fireOutput(entry, 0);
    }
}

void Schematic::collectTimerDeadlines(TickSchedule& schedule) const
{
    for (const auto& entry : nodes_) {
        if (entry.timer && entry.timer->deadline && (!schedule.deadline || *entry.timer->deadline < *schedule.deadline)) {
            schedule.deadline = entry.timer->deadline;
        }
        if (entry.kind == PrimitiveKind::MidiNoteEvent && entry.midi && !entry.midi->scheduled.empty()) {
            const auto deadline = entry.midi->scheduled.front().deadline;
            if (!schedule.deadline || deadline < *schedule.deadline) schedule.deadline = deadline;
        }
    }
    for (const auto& [id, child] : modules_) {
        child->collectTimerDeadlines(schedule);
    }
}

void Schematic::setTickRate(int id, float rate)
{
    auto& tick = node(id).tick;
    if (!tick || !std::isfinite(rate) || rate < 0) {
        throw std::invalid_argument("Tick needs a finite TPS of zero or greater");
    }
    tick->manualRate = rate;
}

void Schematic::setTickDisplaySync(int id, bool enabled)
{
    auto& tick = node(id).tick;
    if (!tick) {
        throw std::invalid_argument("Choose a Tick node");
    }
    if (tick->displaySync != enabled) {
        tick->displaySync = enabled;
        tick->deadline.reset();
    }
}

void Session::setTickRate(int id, float rate)
{
    edit({id}, [rate](auto& graph, const auto& ids) { graph.setTickRate(ids.front(), rate); });
}

void Session::setTickDisplaySync(int id, bool enabled)
{
    edit({id}, [enabled](auto& graph, const auto& ids) { graph.setTickDisplaySync(ids.front(), enabled); });
}

TickReadout Schematic::readTick(int id) const
{
    return readTick(node(id));
}

TickReadout Schematic::readTick(const SchematicNode& entry) const
{
    if (!entry.tick) {
        throw std::invalid_argument("Choose a Tick node");
    }
    const bool wired = hasValueInput(entry.id, 0);
    const float rate = wired ? std::get<float>(runtime_->pull(*ports_.at(entry.id).inputs.front()))
                             : entry.tick->manualRate;
    return {rate, wired, entry.tick->displaySync};
}

TickSchedule Schematic::advanceTicks(TickTime now, TickWake wake)
{
    TickSchedule result;
    auto& root = rootGraph();
    const auto previous = std::exchange(root.tickDispatchTime_, now);
    try {
        root.advanceTicks(now, wake, result);
        if (wake == TickWake::Timer) root.advanceRubyEvents(now, result);
        if (result.fired) {
            try {
                root.refreshConnections();
            } catch (const std::exception&) {
                rememberError(result);
            }
        }
        // A scheduled output can start or stop timers anywhere in the graph.
        root.collectTimerDeadlines(result);
        root.collectRubyDeadline(result);
    } catch (...) {
        root.tickDispatchTime_ = previous;
        throw;
    }
    root.tickDispatchTime_ = previous;
    return result;
}

void Schematic::advanceTicks(TickTime now, TickWake wake, TickSchedule& result)
{
    for (auto& entry : nodes_) {
        if (!entry.tick) {
            try {
                if (entry.timer) {
                    advanceTimer(entry, now, wake, result);
                } else if (entry.kind == PrimitiveKind::MidiNoteEvent && entry.midi && wake == TickWake::Timer) {
                    advanceMidiNotes(entry, now, result);
                } else if (entry.kind == PrimitiveKind::FloatSlide) {
                    advanceSlide(entry, now, wake, result);
                }
            } catch (const std::exception&) {
                rememberError(result);
            }
            continue;
        }
        auto& tick = *entry.tick;
        try {
            bool due = false;
            if (tick.displaySync) {
                tick.deadline.reset();
                result.needsDisplay = true;
                due = wake == TickWake::Display;
            } else {
                const auto rate = readTick(entry).rate;
                if (!(rate > 0) || !std::isfinite(rate)) {
                    tick.deadline.reset();
                    continue;
                }
                const std::chrono::duration<double> period{1.0 / rate};
                if (!tick.deadline || tick.scheduledRate != rate) {
                    tick.scheduledRate = rate;
                    tick.deadline = now + period;
                } else if (wake == TickWake::Timer && *tick.deadline <= now) {
                    tick.deadline = nextPeriodicDeadline(*tick.deadline, now, period);
                    due = true;
                }
                if (!result.deadline || *tick.deadline < *result.deadline) {
                    result.deadline = tick.deadline;
                }
            }
            if (due) {
                result.fired = true;
                fireOutput(entry, 0);
            }
        } catch (const std::exception&) {
            rememberError(result);
        }
    }
    for (auto& [id, child] : modules_) {
        child->advanceTicks(now, wake, result);
    }
}

}
