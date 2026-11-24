#include "Media.hpp"

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QImage>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace openstone::desktop {
namespace {

class QtMediaPlayback final : public MediaPlayback {
public:
    explicit QtMediaPlayback(const MediaSettings& settings)
        : samples_(settings.samples), options_(settings.video)
    {
        if (samples_) {
            QAudioFormat format;
            format.setChannelConfig(QAudioFormat::ChannelConfigStereo);
            format.setSampleRate(static_cast<int>(std::round(settings.sampleRate)));
            format.setSampleFormat(QAudioFormat::Float);
            audioBuffers_ = std::make_unique<QAudioBufferOutput>(format);
            QObject::connect(audioBuffers_.get(), &QAudioBufferOutput::audioBufferReceived, &player_,
                             [this, format](const QAudioBuffer& buffer) {
                const auto decoded = buffer.format();
                if (!buffer.isValid() || decoded.channelCount() != 2 ||
                    decoded.sampleRate() != format.sampleRate() || decoded.sampleFormat() != QAudioFormat::Float) return;
                static_assert(sizeof(MediaAudioBuffer::Frame) == 2 * sizeof(float));
                const auto* data = buffer.constData<char>();
                std::array<MediaAudioBuffer::Frame, 512> frames;
                for (qsizetype pos = 0; pos < buffer.frameCount();) {
                    const auto count = std::min<qsizetype>(frames.size(), buffer.frameCount() - pos);
                    std::memcpy(frames.data(), data + pos * sizeof(frames[0]), count * sizeof(frames[0]));
                    samples_->push(std::span{frames}.first(count));
                    pos += count;
                }
            });
            player_.setAudioBufferOutput(audioBuffers_.get());
        } else {
            player_.setVideoSink(&video_);
            player_.setAudioOutput(&audio_);
            setOptions(options_);
            QObject::connect(&video_, &QVideoSink::videoFrameChanged, &player_, [this](const QVideoFrame& frame) {
                if (!frame.isValid()) return;
                latest_ = frame;
                if (!options_.manual || stepping_ || grabbing_) pending_ = frame;
                grabbing_ = false;
                if (std::exchange(stepping_, false)) player_.pause();
            });
        }
        auto source = QString::fromStdString(settings.source);
        const QUrl url(source);
        player_.setSource(url.scheme().size() > 1 ? url : QUrl::fromLocalFile(source));
        player_.play();
    }

    void command(MediaCommand command, const MediaOptions& options) override
    {
        setOptions(options);
        switch (command) {
        case MediaCommand::PausePlay:
            if (player_.playbackState() == QMediaPlayer::PlayingState) player_.pause();
            else player_.play();
            if (samples_) samples_->setPlaying(player_.playbackState() == QMediaPlayer::PlayingState);
            break;
        case MediaCommand::Grab:
            if (options_.manual) {
                if (latest_.isValid()) pending_ = latest_;
                else grabbing_ = true;
            }
            break;
        case MediaCommand::Step:
            stepping_ = true;
            player_.play();
            break;
        }
    }

    void setOptions(const MediaOptions& options) override
    {
        if (options.manual && !options_.manual) pending_ = {};
        options_ = options;
        audio_.setMuted(options_.mute);
    }

    MediaPoll poll() override
    {
        MediaPoll result;
        const bool end = player_.mediaStatus() == QMediaPlayer::EndOfMedia;
        const bool invalid = player_.mediaStatus() == QMediaPlayer::InvalidMedia;
        if (player_.error() != QMediaPlayer::NoError) result.error = player_.errorString().toStdString();
        result.finished = invalid || !result.error.empty() || (end && (!samples_ || samples_->empty()));
        switch (player_.playbackState()) {
        case QMediaPlayer::PlayingState: result.state = 1; break;
        case QMediaPlayer::PausedState: result.state = 2; break;
        case QMediaPlayer::StoppedState: result.state = end && !result.finished ? 1 : 0; break;
        }
        if (samples_) samples_->setPlaying(!result.finished && result.state == 1);
        if (player_.duration() > 0) {
            result.position = std::clamp(100.F * player_.position() / player_.duration(), 0.F, 100.F);
        }
        if (pending_.isValid()) {
            auto image = std::exchange(pending_, {}).toImage()
                .scaled(options_.width, options_.height).convertToFormat(QImage::Format_RGBA8888);
            const auto size = static_cast<std::size_t>(image.width()) * image.height() * 4;
            if (size) {
                result.frame = yellow::Bitmap(image.width(), image.height(),
                    std::vector<std::uint8_t>(image.constBits(), image.constBits() + size));
            }
        }
        return result;
    }

private:
    std::shared_ptr<MediaAudioBuffer> samples_;
    MediaOptions options_;
    QVideoSink video_;
    QAudioOutput audio_;
    std::unique_ptr<QAudioBufferOutput> audioBuffers_;
    QVideoFrame latest_, pending_;
    bool stepping_ = false;
    bool grabbing_ = false;
    QMediaPlayer player_;
};

}

std::shared_ptr<MediaPlayback> openMedia(const MediaSettings& settings)
{
    return std::make_shared<QtMediaPlayback>(settings);
}

}
