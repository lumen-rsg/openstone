#include "AudioPatch.hpp"
#include "SchematicFile.hpp"

#include <QTemporaryDir>

#include <array>
#include <chrono>
#include <iostream>
#include <span>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;
using openstone::Schematic;
using openstone::Session;
using openstone::TickTime;
using openstone::desktop::SchematicFile;
using openstone::green::MidiMessage;
using enum openstone::PrimitiveKind;
using enum openstone::stream::SequenceMode;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

QByteArray saveAndRead(const Session& session, const QString& path)
{
    SchematicFile::write(path, SchematicFile::project(session));
    return SchematicFile::read(path);
}

void midiTakeSurvivesReopening(const QString& path)
{
    auto now = TickTime{};
    Session original({}, {}, {}, {}, {.monotonicClock = [&] { return now; }});
    auto& graph = original.graph();
    const int module = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(module);
    const int track = child.addNode(MidiSequencerTrack, 0, 0);
    const int source = child.addNode(MidiEvent, 0, 0);
    const int send = child.addNode(Trigger, 0, 0);
    const int status = child.addNode(Int, 0, 0, 0x90);
    const int channel = child.addNode(Int, 0, 0, 5);
    const int note = child.addNode(Int, 0, 0, 67);
    const int velocity = child.addNode(Int, 0, 0, 99);
    child.connect({status, source, 0});
    child.connect({channel, source, 1});
    child.connect({note, source, 2});
    child.connect({velocity, source, 3});
    child.connect({send, source, 4}, openstone::LinkNotification::Silent);
    child.connect({source, track, 0});

    graph.setSequencerMode(Recording, now);
    now += 20ms;
    child.fireTrigger(send);
    now += 30ms;
    child.setValue(status, 0x80);
    child.setValue(velocity, 0);
    child.fireTrigger(send);
    now += 20ms;
    graph.setSequencerMode(Stopped, now);

    const auto saved = saveAndRead(original, path);
    Session reopened;
    check(SchematicFile::loadProject(reopened, saved, {}, path), "Open the independently saved MIDI project");
    reopened.beginEdit(openstone::SchematicEdit::AddNode);
    reopened.addNode(Int, 0, 0);
    reopened.finishEdit();
    check(reopened.applyHistory(false, [](openstone::HistoryUpdate) {}),
          "Undo an edit that rebuilds the reopened schematic");
    auto& root = reopened.graph();
    const auto& restored = *root.module(module);
    check(root.sequencerMode() == Stopped && restored.inspectOutput(track).triggers == 0,
          "Opening a project restores the take without sending MIDI or starting playback");
    check(SchematicFile::project(reopened) == saved, "Reopening keeps the saved MIDI content stable");

    const auto start = TickTime{} + 2s;
    root.setSequencerMode(Playing, start);
    check(!root.advanceSequencer(start + 19ms), "A saved MIDI event waits for its timestamp");
    check(root.advanceSequencer(start + 21ms) &&
          std::get<MidiMessage>(restored.readValue(track)) == MidiMessage{{0x94, 67, 99}},
          "MIDI note-on retains its original bytes and timing");
    check(!root.advanceSequencer(start + 49ms), "The reloaded take preserves the gap between MIDI events");
    check(root.advanceSequencer(start + 51ms) &&
          std::get<MidiMessage>(restored.readValue(track)) == MidiMessage{{0x84, 67, 0}},
          "MIDI note-off retains its original bytes and timing");
    check(!root.advanceSequencer(start + 69ms) && root.sequencerMode() == Playing &&
          root.advanceSequencer(start + 71ms) && root.sequencerMode() == Stopped &&
          restored.inspectOutput(track).triggers == 2,
          "The saved transport duration includes the quiet tail and playback ends without replaying events");
}

void mono4TakeSurvivesReopening(const QString& path)
{
    Session original;
    auto& graph = original.graph();
    graph.setSampleRate(8000);
    const int input = graph.addNode(AudioIn, 0, 0);
    const int output = graph.addNode(AudioOut, 0, 0);
    const int pack = graph.addNode(Pack, 0, 0);
    const int unpack = graph.addNode(Unpack, 0, 0);
    const int track = graph.addNode(StreamSequencerTrack, 0, 0);
    const int third = graph.addNode(MonoToFrame, 0, 0);
    const int fourth = graph.addNode(MonoToFrame, 0, 0);
    const int positive = graph.addNode(Float, 0, 0, .375F);
    const int negative = graph.addNode(Float, 0, 0, -.625F);
    graph.connect({input, pack, 0, 0});
    graph.connect({input, pack, 1, 1});
    graph.connect({positive, pack, 2});
    graph.connect({negative, pack, 3});
    graph.connect({pack, track, 0});
    graph.connect({track, unpack, 0});
    graph.connect({unpack, output, 0, 0});
    graph.connect({unpack, output, 1, 1});
    graph.connect({unpack, third, 0, 2});
    graph.connect({unpack, fourth, 0, 3});
    {
        auto capture = graph.prepareAudio(3);
        graph.setLiveAudio(&capture);
        graph.setSequencerMode(Recording, {});
        const std::array left{0.F, .5F, 1.F, -.5F, -1.F, .25F, .75F};
        const std::array right{.75F, .25F, -.25F, -.75F, 0.F, 1.F, -1.F};
        std::array<float, 7> outLeft{}, outRight{};
        check(capture.processor().process({left, right}, {outLeft, outRight}), "Record four lanes into the take");

        const auto activePath = path + QStringLiteral(".active");
        const auto active = saveAndRead(original, activePath);
        check(graph.sequencerMode() == Recording, "Saving completed frames does not stop an active recording");
        check(capture.processor().process({left, right}, {outLeft, outRight}), "Continue recording after saving");
        graph.setSequencerMode(Stopped, {});
        graph.setLiveAudio(nullptr);
        const auto full = saveAndRead(original, path);

        for (const auto& [data, rate, frames] : std::array{
                 std::tuple{active, 8000.0, 7}, std::tuple{full, 8000.0, 14}, std::tuple{active, 16000.0, 14}}) {
            Session reopened;
            check(SchematicFile::loadProject(reopened, data, {}), "Open the independently saved Mono4 project");
            auto& restored = reopened.graph();
            restored.setSampleRate(rate);
            check(restored.sequencerMode() == Stopped && SchematicFile::project(reopened) == data,
                  "Opening restores the take and keeps its recorded sample rate");
            auto playback = restored.prepareAudio(5);
            restored.setLiveAudio(&playback);
            restored.setSequencerMode(Playing, {});
            std::vector<float> channelA(frames), channelB(frames);
            check(playback.processor().process({}, {channelA, channelB}), "Play the saved take into fresh audio buffers");
            const auto expect = [&](const auto& recorded, std::span<const float> actual) {
                for (int frame = 0; frame < frames; ++frame) {
                    const int index = rate == 8000 ? frame % 7 : frame / 2;
                    const float sample = rate == 16000 && frame % 2 && index + 1 < 7
                        ? (recorded[index] + recorded[index + 1]) * .5F : recorded[index];
                    check(actual[frame] == sample, "The reloaded audio retains the recorded samples and playback rate");
                }
            };
            expect(left, channelA);
            expect(right, channelB);
            const auto lane2 = restored.liveDspTrace(unpack, 2, true);
            const auto lane3 = restored.liveDspTrace(unpack, 3, true);
            check(lane2 == std::vector<double>(frames, .375) && lane3 == std::vector<double>(frames, -.625),
                  "The other two Mono4 lanes survive independently");
            check(restored.advanceSequencer({}) && restored.sequencerMode() == Stopped,
                  "The take ends after its saved duration at either sample rate");
        }
    }
}

}

int main()
{
    try {
        QTemporaryDir files;
        check(files.isValid(), "Create a temporary project folder");
        midiTakeSurvivesReopening(files.filePath(QStringLiteral("midi.ostone")));
        mono4TakeSurvivesReopening(files.filePath(QStringLiteral("mono4.ostone")));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
