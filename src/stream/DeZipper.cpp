#include "stream/DeZipper.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openstone::stream {
namespace {

float milliseconds(float time) noexcept
{
    return time == 0 ? 40.0f : time;
}

template<bool LengthInSamples>
class StreamRamp final : public SampleProcessor {
public:
    void reset() noexcept override { *this = {}; }

    double process(std::span<const double> inputs) noexcept override
    {
        if (untilUpdate_ == 0) {
            const auto target = static_cast<float>(inputs[0]);
            const double resetCount = LengthInSamples ? 0 : inputs[3];
            if (!initialized_ || resetCount != resetCount_) {
                output_ = target;
                remaining_ = 0;
                step_ = 0;
                initialized_ = true;
            } else {
                remaining_ = target != target_ ? lengthInSamples(inputs) : std::max(0.0f, remaining_ - hop);
                const float difference = target - output_;
                step_ = remaining_ > 0 && std::isfinite(remaining_)
                    ? difference / std::max(remaining_, static_cast<float>(hop)) : 0;
                // Treat increments below the level used to suppress denormals as zero.
                constexpr float bias = 1e-18f;
                step_ = (step_ + bias) - bias;
                if (step_ == 0) output_ = target;
            }
            target_ = target;
            resetCount_ = resetCount;
            untilUpdate_ = hop;
        }
        --untilUpdate_;
        output_ += step_;
        return output_;
    }

private:
    static float lengthInSamples(std::span<const double> inputs) noexcept
    {
        if constexpr (LengthInSamples) {
            return static_cast<float>(inputs[1]);
        } else {
            const auto time = milliseconds(static_cast<float>(inputs[1]));
            return time * static_cast<float>(inputs[2]) / 1000.0f;
        }
    }

    static constexpr int hop = 128;
    float output_ = 0;
    float target_ = 0;
    float remaining_ = 0;
    float step_ = 0;
    double resetCount_ = 0;
    int untilUpdate_ = 0;
    bool initialized_ = false;
};

class FloatDeZipper final : public SampleProcessor {
public:
    void reset() noexcept override { *this = {}; }

    double process(std::span<const double> inputs) noexcept override
    {
        const auto target = static_cast<float>(inputs[0]);
        if (!initialized_ || inputs[3] != resetCount_) {
            output_ = target;
            resetCount_ = inputs[3];
            initialized_ = true;
        } else if (target != output_) {
            const auto time = milliseconds(static_cast<float>(inputs[1]));
            const auto rate = static_cast<float>(inputs[2]);
            if (time != time_ || rate != rate_) {
                const float duration = time * rate;
                step_ = duration > 0 && std::isfinite(duration) ?
                    1000.0f / duration : std::numeric_limits<float>::infinity();
                time_ = time;
                rate_ = rate;
            }
            const float difference = target - output_;
            output_ = std::abs(difference) <= step_ ? target : output_ + std::copysign(step_, difference);
        }
        return output_;
    }

private:
    float output_ = 0;
    float time_ = 0;
    float rate_ = 0;
    float step_ = 0;
    double resetCount_ = 0;
    bool initialized_ = false;
};

}

std::unique_ptr<SampleProcessor> makeStreamDeZipper()
{
    return std::make_unique<StreamRamp<false>>();
}

std::unique_ptr<SampleProcessor> makeGlide()
{
    return std::make_unique<StreamRamp<true>>();
}

std::unique_ptr<SampleProcessor> makeFloatDeZipper()
{
    return std::make_unique<FloatDeZipper>();
}

}
