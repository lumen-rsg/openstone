#pragma once

#include "green/Value.hpp"

#include <bitset>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace openstone {

struct SchematicLocation;

struct RubyPort {
    std::string name;
    green::Type type;
    bool operator==(const RubyPort&) const = default;
};

struct RubyPorts {
    std::vector<RubyPort> inputs;
    std::vector<RubyPort> outputs;
    bool operator==(const RubyPorts&) const = default;
};

enum class RubyActionKind { Output, Input, Method, ClearEvents, Redraw, ShowCursor, CreateEdit, CreateDropList, MessageBox };

struct RubyEditOptions {
    yellow::Area area;
    std::string text;
    yellow::Font font{.face = {}, .size = 0};
    yellow::Color textColor{0xFF000000};
    yellow::Color background{0xFFFFFFFF};
    bool multiline = false;
};

struct RubyDropListOptions {
    green::Point position;
    std::string items;
    int checkedIndex = -1;
    std::string style;
    int disabledIndex = -1;
};

struct RubyMessageBoxOptions {
    std::string text;
    std::string title;
    std::string type;
};

struct RubyAction {
    RubyActionKind kind;
    std::size_t port{};
    std::optional<green::Value> value;
    std::optional<double> timestamp;
    std::string method;
    std::optional<yellow::Area> area;
    std::optional<RubyEditOptions> edit;
    std::optional<RubyDropListOptions> dropList;
    std::optional<RubyMessageBoxOptions> messageBox;
};

struct RubyReply {
    std::string result;
    std::vector<RubyAction> actions;
    bool savesState = false;
    bool drawsView = false;
    bool handlesMouse = false;
    std::optional<bool> captureMouse;
};

using RubyInputs = std::span<const std::optional<green::Value>>;
using RubyDrawing = std::variant<yellow::DrawCommand, yellow::GraphDrawCommand, yellow::TextCommand,
                                 yellow::BitmapDrawCommand>;

struct RubyContext {
    std::bitset<256> keys;
    std::vector<std::optional<green::Point>> viewSizes;
};

[[nodiscard]] bool keyIsPressed(const std::bitset<256>& pressed, std::string_view key);

class RubyInstance {
public:
    virtual ~RubyInstance() = default;
    void setContext(RubyContext context) { context_ = std::move(context); }
    virtual RubyReply start(std::string_view source, RubyInputs inputs, double time,
                            const std::optional<std::string>& savedState) = 0;
    virtual std::optional<std::string> saveState(double time) = 0;
    virtual std::vector<RubyDrawing> draw(std::size_t input, yellow::Area bounds, double time) = 0;
    virtual std::optional<yellow::MouseCursor> mouseHit(int input, green::Point position, double time,
                                                        bool queryCursor) = 0;
    virtual std::optional<RubyReply> mouseEvent(int input, std::string_view callback, green::Point position,
                                                double time) = 0;
    virtual std::optional<RubyReply> editEvent(bool finished, const green::RubyValue& id,
                                               std::string_view text, double time) = 0;
    virtual std::optional<RubyReply> dropListFinished(const green::RubyValue& id, int index,
                                                      std::string_view name, double time) = 0;
    virtual std::optional<RubyReply> messageBoxFinished(const green::RubyValue& id, std::string_view result,
                                                        double time) = 0;
    virtual RubyReply event(std::size_t port, const std::optional<green::Value>& value, double time) = 0;
    virtual RubyReply method(std::string_view name, const std::optional<green::Value>& arguments, double time) = 0;
    virtual void storeOutput(std::size_t port, const std::optional<green::Value>& value) = 0;
    virtual void stopSound() noexcept = 0;

protected:
    RubyContext context_;
};

using RubyBackend = std::function<std::shared_ptr<RubyInstance>(
    const RubyPorts&, std::shared_ptr<const SchematicLocation>)>;

inline constexpr std::string_view rubyStarter = R"(# input Float value
# output Float result

def event(i, v)
  output "result", v * 2
end
)";

struct RubyMouseCapture {
    int input;
    yellow::MouseButton button;
    yellow::PanelControlId panel;
    std::int32_t cursorBalance = 0;
};

struct RubyEditControl {
    int input;
    yellow::PanelControlId panel;
    green::RubyValue callbackId;
    yellow::EditControlCommand command;
};

enum class RubyPortEdit { Count, Name, Type, Insert, Remove, Move };
inline constexpr int maxRubyPorts = 32;

struct RubyBoxSize {
    int width = 520;
    int height = 296;
    bool operator==(const RubyBoxSize&) const = default;
};

struct RubyCodeState {
    std::string draft;
    std::string source;
    RubyPorts ports;
    std::shared_ptr<RubyInstance> instance;
    std::vector<std::optional<green::Value>> outputs;
    std::string status;
    bool error = false;
    std::optional<std::string> savedState;
    bool running = false;
    bool savesState = false;
    bool stateDirty = false;
    bool drawsView = false;
    std::string drawError;
    bool usesRedraw = false;
    bool handlesMouse = false;
    std::optional<RubyMouseCapture> mouseCapture;
    std::map<std::uint64_t, RubyEditControl> edits;
    std::int32_t cursorBalance = 0;
    bool manualPorts = false;
    RubyBoxSize boxSize;
};

[[nodiscard]] RubyPorts rubyPorts(std::string_view source);
[[nodiscard]] RubyCodeState rubyCode(std::string draft, std::string source,
                                    std::optional<RubyPorts> manualPorts = {});
[[nodiscard]] bool supportsRubyPort(green::Type type, bool output);
[[nodiscard]] RubyBoxSize clampRubyBoxSize(RubyBoxSize size);

}
