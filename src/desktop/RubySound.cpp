#include "RubySound.hpp"

#include <QAudioDevice>
#include <QFileInfo>
#include <QMediaDevices>
#include <QSoundEffect>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <future>
#include <stdexcept>
#include <utility>

namespace openstone::desktop {
namespace {

class RubySoundPlayer {
public:
    RubySoundPlayer()
    {
        context_->moveToThread(&thread_);
        QObject::connect(&thread_, &QThread::finished, context_, &QObject::deleteLater);
        thread_.start();
    }

    ~RubySoundPlayer()
    {
        thread_.quit();
        thread_.wait();
    }

    void play(const std::optional<std::string>& file, RubySoundMode mode)
    {
        QUrl source;
        QAudioDevice device;
        if (file) {
            const QFileInfo info(QString::fromStdString(*file));
            if (!info.isFile() || !info.isReadable()) {
                throw std::runtime_error("Cannot read sound file " + *file);
            }
            device = QMediaDevices::defaultAudioOutput();
            if (device.isNull()) throw std::runtime_error("No audio output device is available");
            source = QUrl::fromLocalFile(info.absoluteFilePath());
        }

        const auto completion = std::make_shared<std::promise<void>>();
        auto finished = completion->get_future();
        if (!QMetaObject::invokeMethod(context_, [this, source, device, mode, completion] {
                replace(source, device, mode, completion);
            }, Qt::QueuedConnection)) {
            throw std::runtime_error("Sound playback is unavailable");
        }
        // The device runs on its own event loop so a sync request cannot re-enter the schematic.
        finished.get();
    }

private:
    void replace(const QUrl& source, const QAudioDevice& device, RubySoundMode mode,
                 std::shared_ptr<std::promise<void>> completion)
    {
        clear();
        if (source.isEmpty()) {
            completion->set_value();
            return;
        }
        completion_ = std::move(completion);
        mode_ = mode;
        started_ = false;
        effect_ = new QSoundEffect(device, context_);
        effect_->setLoopCount(mode == RubySoundMode::Loop ? QSoundEffect::Infinite : 1);
        QObject::connect(effect_, &QSoundEffect::statusChanged, context_, [this] { update(); });
        QObject::connect(effect_, &QSoundEffect::playingChanged, context_, [this] { update(); });
        QTimer::singleShot(5000, effect_, [this, effect = effect_] {
            if (effect_ == effect && !started_) fail("Sound file took too long to load");
        });
        effect_->setSource(source);
    }

    void update()
    {
        if (effect_->status() == QSoundEffect::Error) {
            fail("Could not play sound file");
        } else if (!started_ && effect_->status() == QSoundEffect::Ready) {
            started_ = true;
            effect_->play();
            if (!effect_) return;
            if (!effect_->isPlaying()) {
                fail("Could not start sound playback");
            } else if (mode_ != RubySoundMode::Sync) {
                complete();
            }
        } else if (started_ && !effect_->isPlaying()) {
            complete();
        }
    }

    void complete()
    {
        if (const auto completion = std::exchange(completion_, {})) completion->set_value();
    }

    void fail(const char* message)
    {
        const auto completion = std::exchange(completion_, {});
        clear();
        if (completion) completion->set_exception(std::make_exception_ptr(std::runtime_error(message)));
    }

    void clear()
    {
        if (auto* effect = std::exchange(effect_, nullptr)) {
            QObject::disconnect(effect, nullptr, context_, nullptr);
            effect->stop();
            effect->deleteLater();
        }
    }

    QThread thread_;
    QObject* context_ = new QObject;
    QSoundEffect* effect_ = nullptr;
    std::shared_ptr<std::promise<void>> completion_;
    RubySoundMode mode_ = RubySoundMode::Async;
    bool started_ = false;
};

}

RubySoundBackend makeRubySoundBackend()
{
    return [player = std::shared_ptr<RubySoundPlayer>{}](std::optional<std::string> file, RubySoundMode mode) mutable {
        if (!player && file) player = std::make_shared<RubySoundPlayer>();
        if (player) player->play(file, mode);
    };
}

}
