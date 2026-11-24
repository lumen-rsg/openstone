#pragma once

#include "Schematic.hpp"
#include "stream/AudioProcessor.hpp"

namespace openstone {

struct Schematic::AudioMonitor {
    std::unique_ptr<stream::AudioProcessor> processor;
    std::map<DspTraceKey, std::size_t> probes;
    struct FrameProbe {
        std::size_t probe;
        std::size_t frames = 0;
        std::size_t sequence = 0;
    };
    mutable std::map<DspTraceKey, FrameProbe> frames;
    std::uint64_t topology = 0;
};

class Schematic::AudioPatch {
public:
    [[nodiscard]] bool hasFrames() const noexcept { return !monitor_->frames.empty(); }
    [[nodiscard]] AudioApi inputApi() const noexcept { return inputApi_; }
    [[nodiscard]] AudioApi outputApi() const noexcept { return outputApi_; }
    [[nodiscard]] stream::AudioProcessor& processor() const { return *monitor_->processor; }
    [[nodiscard]] std::shared_ptr<stream::AudioProcessor> processorHandle() const
    {
        return {monitor_, monitor_->processor.get()};
    }

private:
    friend class Schematic;
    AudioPatch() = default;
    std::shared_ptr<AudioMonitor> monitor_;
    std::vector<std::pair<std::size_t, DspSource>> parameters_;
    std::optional<std::size_t> sampleRateParameter_;
    std::vector<std::pair<DspSource, bool>> voices_;
    std::vector<FloatArrayReadBinding> arrayReaders_;
    std::vector<WaveBufferBinding> waveBuffers_;
    std::vector<MatrixRouteBinding> matrixRoutes_;
    std::map<std::pair<const Schematic*, int>, int> streamRoutes_;
    std::optional<std::pair<const Schematic*, int>> pluginModule_;
    std::size_t blockSize_ = 0;
    AudioApi inputApi_ = AudioApi::Any;
    AudioApi outputApi_ = AudioApi::Any;
    const Schematic* root_ = nullptr;
};

}
