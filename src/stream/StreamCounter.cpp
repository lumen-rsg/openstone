#include "stream/StreamCounter.hpp"

#include <concepts>

namespace openstone::stream {
namespace {

template<std::floating_point Sample>
class StreamCounter final : public SampleProcessor {
public:
    void reset() noexcept override { *this = {}; }

    double process(std::span<const double> inputs) noexcept override
    {
        const double resetCount = inputs[0];
        if (resetCount != resetCount_) {
            next_ = 0;
            resetCount_ = resetCount;
        }
        const auto count = next_;
        next_ += Sample{1};
        return count;
    }

private:
    Sample next_ = 0;
    double resetCount_ = 0;
};

}

std::unique_ptr<SampleProcessor> makeStreamCounter(SamplePrecision precision)
{
    if (precision == SamplePrecision::Float64) return std::make_unique<StreamCounter<double>>();
    return std::make_unique<StreamCounter<float>>();
}

}
