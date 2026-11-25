#pragma once

#include "AudioControls.hpp"
#include "PluginHost.hpp"
#include "Keyboard.hpp"
#include "HttpRequest.hpp"
#include "NetworkBackend.hpp"
#include "MediaBackend.hpp"
#include "stream/Sequencer.hpp"
#include "SfzParser.hpp"

#include "MidiDeviceBackend.hpp"
#include "SerialDeviceBackend.hpp"

#include "SvgDocument.hpp"
#include "green/Arithmetic.hpp"
#include "green/Runtime.hpp"
#include "stream/DspCode.hpp"
#include "RubyCode.hpp"

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <compare>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace openstone {

struct PresetTable;

enum class SystemSetting {
    DefaultZoom, SampleRate, AsioPreferredFrames, AsioMinimumFrames, AsioMaximumFrames, AsioGranularity,
};

inline constexpr std::array<std::string_view, 6> systemSettingNames{
    "Default Zoom Level", "Sample Rate", "ASIO Frames Preferred Size",
    "ASIO Frames Min Size", "ASIO Frames Max Size", "ASIO Frames Granularity",
};

constexpr bool validSystemSetting(int index)
{
    return index >= 0 && static_cast<std::size_t>(index) < systemSettingNames.size();
}

namespace desktop { class SchematicFile; }
namespace stream { class FloatArrayTable; class RouteMixTable; class WaveBuffer; }

enum class PrimitiveKind {
    Int, Float, String, Boolean, Add, Subtract, Multiply, Divide, Trigger, TriggerCounter,
    TriggerBlocker, TriggerSwitch,
    Abs, Modulus, LessThan, GreaterThan, Point, ModuleGui, Area, Color, ColorMatrixSet, ApplyColorMatrix,
    Pen, Solid, Font, FontAttr, StringFormat, StrFmtAttr, Bitmap, BitmapArrayFromFile, BusCreate, BusExtract, Module,
    WirelessOutput, WirelessInput, WirelessLink, WirelessReceiver, ModuleInput, ModuleOutput,
    Arc, Ellipse, Line, Pie, Rect, Text, BitmapArea, BitmapDraw, BitmapDrawTransform, GraphDots, LineGraph, Dots, DrawPointLines, DrawPolygon, DropList, EditControl, PopupList, DropFiles,
    MouseArea, MouseLDbl, MouseLDown, MouseLUp, MouseRDbl, MouseRDown, MouseRUp, MouseWheel, MouseMove,
    PolygonalMouseArea, MouseInfo, MouseDrag, MouseDragAccumulate, DragFiles,
    ViewSection, ViewScale, ViewSize, ViewArea, Clip, GuiArea, SetGuiArea, ViewLoop, ViewArrayLoop, ViewCache, Redraw, RedrawArea,
    AreaToFloat, AreaAroundCenterPoint, AreaFromPoints, AreaCenterPoint, AlignArea, AreaToPoints,
    AreaUnion, PointInsideArea, PointInsidePolygon,
    ColorToArgb, ColorToAhsv, ArgbToColor, AhsvToColor,
    LinearGradientBrush, RadialGradientBrush, HatchBrush, TextureBrush,
    ViewBlur, ViewShadow, ViewGlow, ViewBloom, ViewShader, ViewRotate, ViewSmooth, Window, ShowCursor,
    BitmapResize, BitmapSampleAndHold, BitmapSize, BitmapSave, BitmapCreate,
    BitmapArrayFromBitmap, BitmapBounds, BitmapGetPixel, DrawToBitmap, Svg, MeasureText,
    BitmapGetAt, BitmapSetPixel, ModulePropertiesGui, DspCode, MonoToPoly, PolyToMono,
    SignalAnalyser, MonoToGraph, MonoToGraph2, MonoToGraph4, MonoToGraph8, Tick, MonoToFloat, AudioIn, AudioOut,
    MultilineString, ArrayBuilder, ArrayGet, AppendArray, ArraySection, RotateArray, ArrayFixedSize,
    StringExtract, StringSplit, StringFind, StringReplace, StringOverride, StringArrayFind, StringArraySplit,
    StringArrayToString, StringToStringArray, SpecialCharacters,
    AsciiToString, StringToAscii, StringToHex, HexToString, BinaryToHex, HexToBinary, HexToInt, IntToHex,
    StringCharset, FormatString,
    StreamInverse, StreamReciprocal, StreamSquareRoot, StreamMin, StreamMax, StreamSign, StreamToDouble, DoubleToStream,
    Array, ArrayHistory, ArrayQueue, ArrayStack,
    ArrayInterpolatedGet, ArrayLoop, ArraySplitter, FloatArrayDraw, FloatArrayResample,
    FloatArrayToPointArray, PointArrayToFloatArray, MinFloatArray, MaxFloatArray, ArrayNorm,
    ArrayFft, ArrayInverseFft, MagnitudePhaseToRealImaginary, RealImaginaryToMagnitudePhase,
    SortArray, ArraySum, ArrayLength,
    Min, Max,
    Equal, LessThanOrEqual, GreaterThanOrEqual,
    EqualTrigger, LessThanTrigger, LessThanOrEqualTrigger, GreaterThanTrigger, GreaterThanOrEqualTrigger,
    Round, Floor, Ceiling, Truncate, Power, Logarithm, Ln, Log2, Log10,
    And, Or, Xor, Not, IntNot, ShiftLeft, ShiftRight, IfThenElse, Select, LastSwitch,
    BoolToFalseTrigger, BoolToTrueTrigger, Changed, SampleAndHold,
    TriggerDiv, TriggerDelta,
    Indexer, Loop,
    CounterAdvanced,
    AfterLoad, AfterDuplicate, AboutToClose, TriggerThread,
    Sin, Cos, Tan, SinInverse, CosInverse, TanInverse, Atan2, Sinh, Cosh, Tanh,
    Hypotenuse, Reciprocal, Sign,
    FrequencyToHz, FrequencyToPitch, HzToFrequency, HzToPitch, PitchToFrequency, PitchToHz,
    FromDecibel, ToDecibel, Inverse, NoteNameToInt, IntToNoteName,
    Evaluator, FloatSlide, RandomNumber, TeachingGoal, GoalProgress,
    StreamToGreen, ModuleInputProperty, ModuleOutputProperty,
    Selector, Multiplexer, IntTransition,
    IndexSelector, PointToFloat,
    TextLoad, TextSave,
    SystemFolders, SystemFonts,
    Time, CustomTimer,
    FloatArrayToFloatMem, IntArrayToFloatMem, IntArrayToIntMem, MemCreate, MemArrayGetAt, MemInfo,
    MemToFloatArray, MemMinMax, MemToMemArray, MemToMipmap, MemCopy, MemSection, MemReadByte, MemArraySetAt, MemWriteByte,
    MidiAftertouch, MidiBuffer, MidiChannelFilter, MidiCC, MidiEvent, MidiNote, MidiNoteFilter,
    MidiPitchBend, MidiSplit, MidiStatusFilter, MidiToNotes, MidiNoteEvent, MidiStringToSysex, MidiSysexToString,
    MidiIn, MidiOut, MidiInDevices, MidiOutDevices, MidiInSelect, MidiOutSelect,
    MidiCCStream, MidiAftertouchStream, MidiPitchBendStream,
    NotesPitchFilter, NotesVelocityFilter, NotesChannelFilter, Voices, NotesToVoices, VoicesToPoly,
    PolyChannelId, PolyVoiceId, PolyNoteId, PolyTag, PolyToMonoMin, PolyToMonoMax, PolyToMonoHop, PolyToMonoSelective,
    VstMidiIn, VstMidiOut,
    PresetManager, PresetFloat, PresetBool, PresetString, PresetArray,
    DelayCompensation, VstEditorOpen, VstHostInfo,
    PresetToText, PresetFromText, PresetShowOrder, PresetBypass,
    PresetLoad, PresetSave, VstInput, VstPluginInfo, Vst3AuxChannels,
    DelayOneSample, DelayDoubleOneSample, Pack, PackDouble, Unpack, UnpackDouble,
    Delay, DoubleDelay,
    DeZipper, FloatDeZipper, Glide, Ramp,
    Sawtooth, DoubleSawtooth,
    Sine, DoubleSine, Triangle, DoubleTriangle,
    Impulse,
    BiquadFilterCoeff, DoubleBiquadFilterCoeff,
    FilterCoefficients, DoubleFilterCoefficients,
    BiquadFrequencyResponse, BiquadPhaseResponse,
    StreamCounter, DoubleStreamCounter,
    StreamSine, DoubleStreamSine, StreamCosine, DoubleStreamCosine, StreamTangent, DoubleStreamTangent,
    StreamEqualTo, StreamLess, StreamLessOrEqualTo, StreamGreater, StreamGreaterOrEqualTo,
    DoubleStreamEqualTo, DoubleStreamLess, DoubleStreamLessOrEqualTo, DoubleStreamGreater, DoubleStreamGreaterOrEqualTo,
    StreamSampleAndHold, DoubleSampleAndHold,
    StreamPitchToFreq, DoublePitchToFreq, SampleRate,
    StreamSelect, DoubleSelect,
    FloatArrayRead,
    SamplePosition, DoubleSamplePosition, PpqPosition, DoublePpqPosition,
    BarStartPosition, DoubleBarStartPosition, StreamTempo, DoubleTempo,
    StreamTimeSignature, DoubleTimeSignature, StreamIsPlaying,
    VstOutput,
    ClearAudio, DirectSoundIn, DirectSoundOut, AsioIn, AsioOut,
    AudioDevices, AudioInputsOutputs, AudioSelect, DirectSoundInDevices, DirectSoundInSelect, SetSampleRate, OpenAsioSettings,
    EnvelopeControl,
    LoadWaveFile, SaveWaveFile, WaveFileArray, WaveTable,
    WaveRead, WaveReadHop, WaveArrayRead, WaveTableRead, WaveWriteMono, WaveWriteStereo,
    MonoToFrame,
    FileDialog, FolderDialog, MessageBox, FindFiles, GenerateGuid, SchematicFilePath,
    ComPort, ComPorts,
    GridToPixel, GridToPixelArea, PixelToGrid, PixelToGridArea, TooltipHelp, RedrawLimiter, RedrawsSuspended,
    PanelBackgroundColor, FocusMode, FullScreen, PanelZoom,
    KeyDown, KeyUp, IsKeyPressed,
    IsPlaying, OfflineMode, Tempo, TimeSignature, StartFolder, Settings, SettingsStore, StateMachine,
    BasicCounter, ExeQuit, WebUrl, ShellExecute, MacAddresses, HardDiskSerial, ColorDialog, ColorMatrix, PenAttributes, RoundRectangle,
    HttpTextRequest, HttpImageRequest, HttpFileRequest, FontEmbed, ModuleWirelessOutput, ModuleWirelessLink, Matrix, MouseCapture, SfzParser, NetworkClient, NetworkServer, AudioStreamIn, VideoStreamIn, MidiSequencerTrack, StreamSequencerTrack, Ruby, Assembler,
};

[[nodiscard]] constexpr bool isDspEditor(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::DspCode || kind == PrimitiveKind::Assembler;
}

[[nodiscard]] constexpr stream::DspLanguage dspLanguage(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::Assembler ? stream::DspLanguage::Assembler : stream::DspLanguage::Code;
}

[[nodiscard]] constexpr bool isStreamDelay(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::DelayOneSample || kind == PrimitiveKind::DelayDoubleOneSample ||
           kind == PrimitiveKind::Delay || kind == PrimitiveKind::DoubleDelay;
}

[[nodiscard]] constexpr bool isStreamOscillator(PrimitiveKind kind) noexcept
{
    using enum PrimitiveKind;
    return kind == Sawtooth || kind == DoubleSawtooth || kind == Sine || kind == DoubleSine ||
           kind == Triangle || kind == DoubleTriangle;
}

[[nodiscard]] constexpr bool hasExpandableInputs(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::ArrayBuilder || kind == PrimitiveKind::Indexer ||
           kind == PrimitiveKind::Selector || kind == PrimitiveKind::Matrix;
}

[[nodiscard]] constexpr std::size_t firstExpandableInput(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::Selector || kind == PrimitiveKind::Matrix ? 1 : 0;
}

[[nodiscard]] constexpr bool hasExpandableOutputs(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::Multiplexer || kind == PrimitiveKind::Matrix;
}

[[nodiscard]] constexpr bool ownsPanel(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::ModuleGui || kind == PrimitiveKind::ModulePropertiesGui ||
           kind == PrimitiveKind::Window;
}

[[nodiscard]] constexpr bool isModuleEndpoint(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::ModuleInput || kind == PrimitiveKind::ModuleOutput;
}

[[nodiscard]] constexpr bool isPropertyEndpoint(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::ModuleInputProperty || kind == PrimitiveKind::ModuleOutputProperty;
}

[[nodiscard]] constexpr bool isIndexedRouter(PrimitiveKind kind)
{
    return kind == PrimitiveKind::Selector || kind == PrimitiveKind::Multiplexer;
}

[[nodiscard]] constexpr bool isPolyBridge(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::MonoToPoly || kind == PrimitiveKind::PolyToMono ||
           kind == PrimitiveKind::PolyToMonoMin || kind == PrimitiveKind::PolyToMonoMax ||
           kind == PrimitiveKind::PolyToMonoHop || kind == PrimitiveKind::PolyToMonoSelective;
}

[[nodiscard]] constexpr bool isModuleWirelessOutput(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::ModuleWirelessOutput || kind == PrimitiveKind::ModuleWirelessLink;
}

[[nodiscard]] constexpr bool usesWirelessLabel(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::WirelessInput || kind == PrimitiveKind::WirelessOutput ||
           kind == PrimitiveKind::ModuleWirelessOutput;
}

[[nodiscard]] constexpr bool isPointPlot(PrimitiveKind kind) noexcept
{
    return kind == PrimitiveKind::Dots || kind == PrimitiveKind::DrawPointLines || kind == PrimitiveKind::DrawPolygon;
}

enum class PrimitiveBehavior {
    ValueBox, Binary, TriggerButton, Counter, Blocker, Switch, UnaryMath, Compare, MakePoint, ModuleGui, AreaGeometry,
    ParseColorMatrix, ColorMatrixSet, ApplyColorMatrix, PenInfo, EmbedFonts, SplitColor, MakeColor, MakePen, MakeSolid, MakeBrush, MakeFont, FontAttributes,
    MakeStringFormat, StringFormatAttributes, LoadBitmap, LoadBitmapArray, CreateBus, ExtractBus, Module,
    SendWireless, ReceiveWireless, ModuleInput, ModuleOutput, DrawView, DropList, EditControl, PopupList, DropFiles,
    MouseArea, ParseMouseMessage, MouseMove, MouseInfo, MouseDrag, DragFiles, ShowCursor,
    TransformView, ViewAttributes, SetGuiArea, ViewLoop, ViewCache, Redraw, ViewEffect, Window,
    BitmapOperation, BitmapCanvas, BitmapMetadata, MeasureText, DspCode, PolyBridge,
    CaptureSignal, SampleMono, Tick, AudioEndpoint, AudioControl, FileUtility, SystemAction, HttpTransfer, SerialDevice, PanelUtility, PresentationControl, KeyboardInput, BuildArray, GetArrayElement, AppendArrays, TransformArray,
    ProcessString, StreamMath, StreamConversion, StreamRouting, StoreArray, ProcessArray, GreenLogic, StateMachineLogic,
    ProcessTrigger, NoteConversion, GeneralProcessing, PropertyInput, PropertyOutput, IndexSelector, SplitPoint,
    TextFile, SystemInfo, SystemSettings, PersistSettings, SampleTime, Timer, MemoryOperation, MidiOperation, MidiDevice, MidiStream, NotesFilter, VoiceRouting, VoiceSignal, HostMidi,
    ManagePresets, PresetParameter, PluginHost, PresetUtility, WaveFile, MatrixRouting, MouseCaptureControl, ParseSfz, NetworkTransfer, MediaSource, SequencerTrack, RubyCode,
};

struct Port {
    std::string_view name;
    std::optional<green::Type> type; // Empty for a template connector.
};

struct Primitive {
    PrimitiveKind kind;
    std::string_view name;
    std::string_view symbol;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    PrimitiveBehavior behavior;
    std::optional<green::ArithmeticOperation> operation = std::nullopt;
    std::optional<green::UnaryOperation> unaryOperation = std::nullopt;
    std::optional<math::PitchConversion> pitchConversion = std::nullopt;

    [[nodiscard]] bool hasTemplateConnectors() const;
};

[[nodiscard]] constexpr bool supportsStreamMath(const Primitive& spec) noexcept
{
    return spec.behavior == PrimitiveBehavior::StreamMath ||
           ((spec.behavior == PrimitiveBehavior::UnaryMath || spec.behavior == PrimitiveBehavior::Binary) &&
            !spec.inputs.front().type && spec.operation != green::ArithmeticOperation::Modulus &&
            spec.operation != green::ArithmeticOperation::Hypotenuse &&
            spec.unaryOperation != green::UnaryOperation::FromDecibel &&
            spec.unaryOperation != green::UnaryOperation::ToDecibel);
}

[[nodiscard]] std::optional<green::Type> numericStreamFamily(green::Type type);
[[nodiscard]] std::span<const Primitive> primitives();
[[nodiscard]] const Primitive& primitive(PrimitiveKind kind);

inline constexpr yellow::Area defaultPanelBounds{0.0F, 0.0F, 16.0F, 10.0F};
inline constexpr std::int32_t maxDspCaptureFrames = 16384;

struct ModuleConnector {
    std::string name;
    std::optional<green::Type> type;
    int endpoint;

    bool operator==(const ModuleConnector&) const = default;
};

enum class DropListLayout { AutoColumns, Scroll, FixedColumns };

struct DropListMenuEntry {
    std::string text;
    int optionIndex = -1;
    int submenu = -1;
};

struct DropListContents {
    std::vector<std::string> options;
    std::vector<std::vector<DropListMenuEntry>> menus{{}};
};

struct RubyMessageBox {
    green::RubyValue callbackId;
    std::span<const std::string_view> buttons;
    std::size_t defaultButton;
};

struct AppDialogRequest {
    std::uint64_t token;
    std::vector<int> ownerPath;
    int node;
    PrimitiveKind kind;
    std::string title;
    std::string folder;
    std::string filter;
    std::string name;
    std::string suffix;
    std::string message;
    int type = 0;
    int icon = 0;
    yellow::Color initialColor;
    std::uint8_t alpha = 255;
    std::optional<RubyMessageBox> rubyMessageBox;
};

struct RubyDropListSource {
    int input;
    green::RubyValue callbackId;
};

struct DropListRequest {
    std::uint64_t token;
    std::vector<int> ownerPath;
    int owner;
    yellow::View view;
    green::Point position;
    DropListContents contents;
    int checkedIndex;
    DropListLayout layout = DropListLayout::AutoColumns;
    int rowsPerColumn = 0;
    PrimitiveKind kind = PrimitiveKind::DropList;
    int disabledIndex = -1;
    std::optional<RubyDropListSource> ruby;
};

struct DropListSelection {
    std::int32_t index;
    std::string text;
};

struct EditControlState {
    std::optional<std::string> text;
    std::uint64_t session = 0;
    bool pendingTextPulse = false;
};

struct DropFile {
    std::string path;
    std::string name;
    bool directory;
};

enum class FileDropPhase { Preview, Drop };

struct FileDrag {
    green::StringArray paths;
    green::Point position;
};

struct DropFilesState {
    FileDrag lastDrop;
    std::optional<FileDrag> preview;

    [[nodiscard]] const FileDrag& current() const { return preview ? *preview : lastDrop; }
};

struct MouseDragAccumulator {
    green::Point anchorPosition;
    green::Point anchorValue;
    green::Point scale;
};

struct MouseDragState {
    std::array<green::Point, 3> positions;
    std::optional<yellow::PanelControlId> source;
    std::optional<MouseDragAccumulator> accumulator;
};

enum class FileDragMode { Copy, Move, Link };

struct DragFilesRequest {
    std::uint64_t token;
    green::StringArray paths;
    FileDragMode mode;
};

struct CursorVisibility {
    bool hidden = false;
    bool hold = false;
    std::uint64_t revision = 0;
};

struct MouseDragEffects {
    bool started = false;
    bool resetPointer = false;
    bool accumulating = false;
    std::optional<yellow::MouseCursor> cursor;
    std::optional<DragFilesRequest> fileDrag;
};

struct LoopValue {
    green::Value value;
    std::int32_t index;
};

struct ViewCacheFrame {
    yellow::View view;
    yellow::Area panelBounds;
    double scale;
    yellow::CachedDrawCommand command;
};

struct ViewCacheState {
    std::uint64_t topology = 0;
    std::uint64_t clears = 0;
    // The same node can be drawn on multiple panels and inside repeating branches.
    std::map<std::pair<yellow::PanelControlId, std::vector<std::size_t>>, ViewCacheFrame> frames;
};

struct PanelRedraw {
    bool wholePanel = false;
    yellow::ClipRegion areas;
};

struct PanelPaint {
    yellow::Area bounds;
    double rasterScale;
    std::uint64_t topology;
    std::vector<yellow::PanelDrawing> drawings;
};

inline constexpr int defaultWindowWidth = 320;
inline constexpr int defaultWindowHeight = 200;
inline constexpr float windowPixelsPerUnit = 8;
inline constexpr int maximumWindowExtent = (1 << 24) - 1;

struct WindowState {
    int width = defaultWindowWidth;
    int height = defaultWindowHeight;
    bool requested = false;
    bool visible = false;
};

struct BitmapCanvasState {
    std::int32_t width;
    std::int32_t height;
    float gridStep;
    bool rendering = false;
};

enum class ModulePanelKind { Front, Properties };
enum class PropertiesVisibility { Closed, Open, Pinned };
enum PanelAnchor : std::uint8_t { AnchorLeft = 1, AnchorTop = 2, AnchorRight = 4, AnchorBottom = 8 };

struct DspCodeState {
    std::string draft;
    std::string source;
    stream::DspProgram program;
};

using TickTime = std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<double>>;

enum class TickWake { Configure, Timer, Display };

struct TickState {
    float manualRate = 25.0f;
    bool displaySync = false;
    float scheduledRate = 0.0f;
    std::optional<TickTime> deadline;
};

struct TimerState {
    std::int32_t intervalMs;
    std::optional<TickTime> deadline;
};

struct SequenceMidiState {
    std::vector<std::pair<TickTime::duration, green::MidiMessage>> take;
    std::size_t next = 0;
    std::map<std::pair<int, int>, green::MidiMessage> held;
};

struct MidiState {
    struct ScheduledNote {
        TickTime deadline;
        green::NoteEvent note;
    };

    std::array<std::optional<green::Value>, 4> values;
    std::vector<green::MidiMessage> buffer;
    std::vector<green::NoteEvent> held;
    std::vector<green::NoteEvent> sustained;
    std::array<bool, 16> sustain{};
    std::vector<ScheduledNote> scheduled;
    std::optional<std::int32_t> streamController;
    std::array<std::optional<std::uint8_t>, 16> streamCc;
    struct NoteRoute {
        green::NoteEvent source;
        std::vector<green::NoteEvent> layers;
    };
    std::vector<NoteRoute> noteRoutes;
};

struct MidiDeviceSelection {
    std::vector<std::string> ports;
    std::optional<std::int32_t> requestedIndex;
    std::int32_t reportedIndex = -1;
    bool reportedUnavailable = false;
};

struct MidiDeviceChoice {
    std::string id;
    std::string name;
    bool selected;
    bool available;
    bool connected;
};

struct TickReadout {
    float rate;
    bool wired;
    bool displaySync;
};

struct TickSchedule {
    std::optional<TickTime> deadline;
    bool needsDisplay = false;
    bool fired = false;
    std::exception_ptr error;
};

struct DeferredTriggerDispatch {
    bool fired = false;
    std::exception_ptr error;
};

struct EvaluatorCache {
    std::string expression;
    std::optional<stream::DspProgram> program;
};

[[nodiscard]] green::Type dspConnectorType(stream::DspPortType type);
[[nodiscard]] bool canSupplyDspInput(green::Type source, green::Type input);

using ArrayStorage = std::variant<std::deque<std::int32_t>, std::deque<float>,
                                  std::deque<std::string>, std::deque<green::Point>>;

struct MemoryCache {
    std::optional<green::Memory> conversion;
    std::optional<green::MemoryArray> array;
    std::shared_ptr<const std::array<green::FloatArray, 4>> extrema;
};

struct WaveFileState {
    int rate = 0;
    int status = 0;
    green::IntArray rates;
    bool excludeSamples = false;
};

struct SerialPortState;
struct StateMachineMap;

inline constexpr std::size_t maxEmbeddedFontBytes = 16 * 1024 * 1024;

struct FontRegistration {
    virtual ~FontRegistration() = default;
};

struct EmbeddedFonts {
    std::shared_ptr<const std::vector<std::string>> data;
    std::shared_ptr<const FontRegistration> registration;
};

struct StateMachineState {
    std::shared_ptr<const StateMachineMap> map;
    std::size_t active = 0;
    std::int32_t reported = 0;
};

struct MatrixConnection {
    int input;  // Schematic connector index, including the fixed Connections input.
    int output;
    bool operator==(const MatrixConnection&) const = default;
};

struct MatrixState {
    std::vector<MatrixConnection> connections;
    std::vector<int> lastEventInputs;
    std::uint64_t revision = 0;
};

struct SchematicNode {
    int id;
    PrimitiveKind kind;
    double x;
    double y;
    std::optional<green::Value> localValue;
    std::vector<std::uint64_t> inputTriggers;
    std::vector<std::uint64_t> outputTriggers;
    std::optional<green::Type> typeOverride;
    std::vector<std::string> channelNames;
    std::string moduleName;
    std::string wirelessLabel;
    std::string label;
    bool panelHidden = false;
    bool showPanelInParent = true;
    bool panelUnlocked = false;
    PropertiesVisibility propertiesVisibility = PropertiesVisibility::Closed;
    green::Point panelPosition;
    green::Point propertiesPosition;
    yellow::Area panelBounds = defaultPanelBounds;
    std::uint8_t panelAnchors = AnchorLeft | AnchorTop;
    std::optional<green::Point> anchorReference;
    std::vector<ModuleConnector> moduleInputs;
    std::vector<ModuleConnector> moduleOutputs;
    std::optional<green::Type> boundaryType;
    int boundaryIndex = 0;
    std::optional<DropListSelection> dropListSelection;
    std::optional<EditControlState> editControl;
    std::optional<std::int32_t> popupListIndex;
    std::optional<DropFilesState> fileDrag;
    bool fileDropSucceeded = false;
    yellow::MouseMessage lastMouseMessage;
    std::optional<MouseDragState> mouseDrag;
    std::optional<LoopValue> viewLoopValue;
    std::optional<ViewCacheState> viewCache;
    std::optional<WindowState> window;
    std::optional<BitmapCanvasState> bitmapCanvas;
    std::shared_ptr<const SvgDocument> svgDocument;
    PanelRedraw redraw;
    std::optional<PanelPaint> panelPaint;
    // Imported FlowStone code boxes expose Green source-string connectors.
    // They follow the data ports so existing DSP/Ruby connector indices stay stable.
    bool codeSourcePorts = false;
    bool codeSourceDirty = false;
    [[nodiscard]] std::optional<int> codeSourcePort(bool output) const;
    std::optional<DspCodeState> dspCode;
    std::optional<RubyCodeState> rubyCode;
    std::vector<green::Value> capturedSignals;
    std::optional<TickState> tick;
    std::optional<TimerState> timer;
    mutable std::optional<std::array<std::int32_t, 8>> localTime;
    mutable MemoryCache memoryCache;
    WaveFileState waveFile;
    std::optional<MidiState> midi;
    std::optional<MidiDeviceSelection> midiSelection;
    std::shared_ptr<SerialPortState> serialPort;
    std::optional<PresetManagerState> presetManager;
    std::optional<PresetParameterState> presetParameter;
    std::array<int, 2> legacyPresetInfo{};
    mutable std::shared_ptr<stream::VoiceBank> voiceBank;
    std::uint64_t historyId = 0;
    std::optional<ArrayStorage> arrayStorage;
    std::optional<LoopValue> greenLoopValue;
    std::optional<std::int32_t> arrayDrawIndex;
    std::array<green::FloatArray, 2> arrayTransformResult;
    int lastSwitchInput = 0;
    // Imported native routers retain populated sockets and their routing policy.
    std::optional<int> flowStoneRoutingBranches;
    std::optional<MatrixState> matrix;
    std::optional<NetworkState> network;
    std::optional<SfzState> sfz;
    std::optional<MediaState> media;
    mutable std::shared_ptr<MediaAudioBuffer> mediaSamples;
    mutable std::shared_ptr<stream::SequenceAudioTrack> sequenceAudio;
    std::optional<SequenceMidiState> sequenceMidi;
    std::int32_t triggersSinceDivision = 0;
    std::optional<TickTime> previousTriggerTime;
    bool loopRunning = false;
    bool counterAscending = true;
    bool holdingClose = false;
    StateMachineState stateMachine;
    EmbeddedFonts embeddedFonts;
    std::uint64_t httpRequestToken = 0;
    mutable std::optional<EvaluatorCache> evaluator;
    std::optional<std::uint32_t> randomState;
    std::optional<TickTime> slideDeadline;
    std::int32_t slideInterval = 0;
    std::optional<double> goalSeconds;
    // FlowStone's generic comparisons require every paired array element to match.
    bool compareArrayElements = false;

    [[nodiscard]] std::vector<Port> inputPorts() const;
    [[nodiscard]] std::vector<Port> outputPorts() const;
};

struct SchematicLink {
    int source;
    int target;
    int input;
    int output = 0;

    auto operator<=>(const SchematicLink&) const = default;
};

struct SignalReadout {
    std::optional<green::Value> value;
    std::uint64_t triggers;
};

struct PresentationState {
    std::optional<std::vector<int>> panelPath;
    std::uint64_t fullScreenToggles = 0;
    float requestedScale = 1;
    bool fullScreen = false;
    float scale = 1;
};

struct ModuleHelpText {
    std::string title;
    std::string body;
};

struct ModulePanel {
    yellow::View view;
    bool borderVisible;
    yellow::View parentView;
    green::Point position{};
    std::uint8_t anchors = AnchorLeft | AnchorTop;
};

enum class LinkNotification { Deliver, Silent };

enum class WindowType { Fixed, Resizable, AppWindow };

struct WindowPanel {
    ModulePanel panel;
    int width;
    int height;
    WindowType type;
    std::string title;
};

class Schematic {
public:
    using BitmapLoader = std::function<yellow::Bitmap(std::string_view path)>;
    using TextMeasurer = std::function<yellow::Area(std::string_view, yellow::Area,
                                                    const yellow::Font&, const yellow::StringFormat&)>;

    struct BitmapBackend {
        std::function<void(std::string_view path, std::string_view encoder, const yellow::Bitmap&)> save;
        std::function<yellow::Bitmap(const BitmapCanvasState&, const yellow::View&,
                                    const yellow::Bitmap& background, const std::vector<yellow::PanelDrawing>&)> render;
        std::function<SvgDocument(std::string_view path)> loadSvg;
    };

    struct TextFileBackend {
        std::function<std::string(std::string_view path)> load;
        std::function<void(std::string_view path, std::string_view text)> save;
        std::function<std::string(std::string_view path, std::size_t limit)> loadBounded;
    };

    struct SystemInfoBackend {
        std::function<std::string(std::int32_t id)> folder;
        std::function<green::StringArray()> fonts;
        std::function<std::chrono::system_clock::time_point()> wallClock;
        std::function<TickTime()> monotonicClock;
        std::function<bool(std::string_view url)> openUrl;
        std::function<bool(std::string_view program, std::string_view arguments, bool background)> launchProgram;
        std::function<green::StringArray()> macAddresses;
        std::function<std::string()> diskSerial;
        std::function<void()> quitApplication;
        std::function<void(HttpRequest)> sendHttp;
        std::function<std::shared_ptr<NetworkConnection>(const NetworkSettings&)> openNetwork;
        std::function<std::shared_ptr<MediaPlayback>(const MediaSettings&)> openMedia;
        std::function<std::shared_ptr<const FontRegistration>(const std::vector<std::string>&)> registerFonts;
        RubyBackend ruby;
    };

    struct SettingsStoreBackend {
        std::function<std::optional<std::string>(std::string_view group, std::string_view key)> read;
        std::function<void(std::string_view group, std::string_view key, std::string_view value)> write;
    };

    explicit Schematic(BitmapLoader bitmapLoader = {}, BitmapBackend bitmapBackend = {},
                       TextMeasurer textMeasurer = {}, TextFileBackend textFiles = {},
                       SystemInfoBackend systemInfo = {});
    Schematic(const Schematic&) = delete;
    Schematic& operator=(const Schematic&) = delete;

    [[nodiscard]] const std::vector<SchematicNode>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<SchematicLink>& links() const noexcept { return links_; }
    [[nodiscard]] std::uint64_t triggers() const noexcept { return triggers_; }
    [[nodiscard]] std::uint64_t busLayoutRevision() const noexcept { return busLayoutRevision_; }
    [[nodiscard]] std::uint64_t panelGeometryRevision() const { return rootGraph().panelGeometryRevision_; }

    [[nodiscard]] std::uint64_t redrawRevision() const { return rootGraph().redrawRevision_; }

    [[nodiscard]] bool canPlace(PrimitiveKind kind) const noexcept;
    [[nodiscard]] Schematic* module(int id);
    [[nodiscard]] const Schematic* module(int id) const;
    void setModuleName(int id, std::string name);
    void setModulePortName(int id, std::string name);
    void setModulePortType(int id, std::optional<green::Type> type);
    void setWirelessLabel(int id, std::string label);
    [[nodiscard]] std::string wirelessName(int id) const;
    [[nodiscard]] bool wirelessConnected(int id) const;

    int addNode(PrimitiveKind kind, double x, double y,
                std::optional<green::Value> value = std::nullopt);
    void moveNode(int id, double x, double y);
    void setDspDraft(int id, std::string source);
    void setRubyDraft(int id, std::string source);
    void runRuby(int id);
    void applyRubyPorts(int id);
    void editRubyPort(int id, bool output, int index, RubyPortEdit edit, int value, std::string name = {});
    void resizeRubyBox(int id, RubyBoxSize size);
    void stopRuby(int id);
    [[nodiscard]] std::vector<std::vector<double>> captureDsp(int id, std::size_t frames, std::size_t blockSize) const;
    class AudioPatch;
    [[nodiscard]] AudioPatch prepareAudio(std::size_t blockSize) const;
    [[nodiscard]] AudioPatch preparePluginAudio(int moduleId, std::size_t blockSize) const;
    [[nodiscard]] PluginAudioLayout pluginAudioLayout(int moduleId) const;
    [[nodiscard]] PluginExportInfo pluginExportInfo(int moduleId) const;
    void setPluginAuxStates(int moduleId, std::span<const int> inputs, std::span<const int> outputs);
    // Synchronize Green controls on the editing thread. False means the host should replace the patch.
    [[nodiscard]] bool refreshAudioControls(AudioPatch& patch) const;
    void setLiveAudio(const AudioPatch* patch);
    [[nodiscard]] bool publishAudioFrames();
    // Empty optional means the signal is outside the running audio graph.
    [[nodiscard]] std::optional<std::vector<double>> liveDspTrace(int id, int port, bool output) const;
    // Published waveforms are snapshots of the last capture and are cleared on graph rebuild.
    [[nodiscard]] std::span<const double> dspTrace(int id, int port, bool output) const;
    int makeModule(std::span<const int> selection);
    void removeNode(int id);
    // The link is installed before its pulse. A failed cascade leaves the link installed.
    void connect(SchematicLink link, LinkNotification notification = LinkNotification::Deliver);
    [[nodiscard]] bool canSuggestLink(SchematicLink link) const;
    void disconnectLink(SchematicLink link);
    void disconnectInput(int id, int input);
    void disconnectNode(int id);
    void setTemplateType(int id, std::optional<green::Type> type);
    [[nodiscard]] std::optional<green::Type> inputType(int id, int input) const;
    [[nodiscard]] std::optional<green::Type> outputType(int id, int output = 0) const;
    [[nodiscard]] static bool supportsTemplateType(PrimitiveKind kind, green::Type type);
    [[nodiscard]] bool editable(int id) const;
    [[nodiscard]] green::Value readValue(int id, int output = 0) const;
    [[nodiscard]] SignalReadout inspectInput(int id, int input) const;
    [[nodiscard]] SignalReadout inspectOutput(int id, int output = 0) const;
    [[nodiscard]] SignalReadout inspectLink(SchematicLink link) const;
    [[nodiscard]] ModuleHelpText readModuleHelp(int id) const;
    [[nodiscard]] std::optional<yellow::Color> presentationBackground() const;
    [[nodiscard]] const PresentationState& presentation() const;
    void setPresentationPanel(std::optional<std::vector<int>> path);
    bool reportPresentation(std::span<const int> path, bool fullScreen, float scale);
    bool sendKeyEvent(const KeyboardEvent& event);
    void clearPressedKeys() noexcept;
    [[nodiscard]] ModulePanel readModulePanel(int id) const;
    [[nodiscard]] ModulePanel readPanel(int id) const;
    [[nodiscard]] std::vector<WindowPanel> requestedWindows() const;
    bool reportWindowVisible(int id, bool visible);
    bool reportWindowSize(int id, int width, int height);
    [[nodiscard]] std::optional<ModulePanel> modulePanel(ModulePanelKind kind) const;
    [[nodiscard]] std::optional<ModulePanel> frontPanel() const { return modulePanel(ModulePanelKind::Front); }
    [[nodiscard]] std::vector<yellow::PanelDrawing> panelDrawings(const yellow::View& view) const;
    [[nodiscard]] std::vector<yellow::PanelDrawing> renderPanel(const yellow::View& view, double rasterScale = 1);
    [[nodiscard]] bool hasDrawingBehavior(PrimitiveBehavior behavior) const;
    void setPanelHidden(int id, bool hidden);
    void setPanelUnlocked(int id, bool unlocked);
    void setPropertiesVisibility(int id, PropertiesVisibility visibility);
    void movePanelItem(int moduleId, std::span<const int> itemPath, green::Point position,
                       ModulePanelKind surface = ModulePanelKind::Front, ModulePanelKind item = ModulePanelKind::Front);
    void resizePanel(int moduleId, float width, float height, ModulePanelKind surface = ModulePanelKind::Front);
    void setValue(int id, green::Value value);
    void fireTrigger(int id);
    [[nodiscard]] bool hasDeferredTriggers() const;
    // Call on the owning control thread between cascades. New pulses wait for another turn.
    [[nodiscard]] DeferredTriggerDispatch dispatchDeferredTriggers();
    [[nodiscard]] std::size_t arraySplitterOutputCount(int id) const;
    void setTickRate(int id, float rate);
    void setTickDisplaySync(int id, bool enabled);
    [[nodiscard]] TickReadout readTick(int id) const;
    // Addresses the entire schematic, including modules outside the current viewport.
    [[nodiscard]] TickSchedule advanceTicks(TickTime now, TickWake wake);
    // Hardware adapters remain on the control side, independent of the Green and Stream runtimes.
    // Called by a host on the schematic control thread.
    void setPluginHostCallbacks(PluginHostCallbacks callbacks);
    void setPluginHostInfo(std::optional<PluginHostInfo> info);
    // Publish transport to Green on the schematic control thread. Pass {} to restore standalone defaults.
    void setPluginHostTransport(const stream::HostTransport& transport);
    void setPluginEditorOpen(bool open);
    void setRedrawsSuspended(bool suspended);
    [[nodiscard]] bool redrawsSuspended() const noexcept;
    [[nodiscard]] int pluginLatencySamples() const;
    [[nodiscard]] std::vector<PluginParameter> pluginParameters() const;
    bool receivePluginParameter(const PluginParameterAddress& address, float value);
    void receiveHostMidi(const green::MidiMessage& message);
    void setHostMidiOutput(std::function<void(const green::MidiMessage&)> output);
    void setMidiDeviceBackend(std::shared_ptr<MidiDeviceBackend> backend);
    void setSerialDeviceBackend(std::shared_ptr<SerialDeviceBackend> backend);
    [[nodiscard]] bool hasOpenSerialPorts() const;
    [[nodiscard]] bool pollSerialDevices();
    [[nodiscard]] bool hasOpenNetworkConnections() const;
    [[nodiscard]] bool pollNetworkConnections();
    [[nodiscard]] bool hasOpenMedia() const;
    [[nodiscard]] bool pollMedia();
    [[nodiscard]] bool hasSequencerTracks() const;
    [[nodiscard]] stream::SequenceMode sequencerMode() const;
    void setSequencerMode(stream::SequenceMode mode, TickTime now);
    bool advanceSequencer(TickTime now);
    [[nodiscard]] bool refreshMidiDevices(bool scan = true, bool notify = true);
    [[nodiscard]] bool pollMidiDevices();
    [[nodiscard]] bool watchesMidiDevices() const;
    [[nodiscard]] bool listensToMidi() const;
    [[nodiscard]] std::vector<MidiDeviceChoice> midiDeviceChoices(int id) const;
    void setMidiDeviceSelected(int id, std::string port, bool selected);

    void setAudioControlCallbacks(std::function<bool(const AudioDeviceSetup&)> apply,
                                  std::function<void()> asioSettings = {});
    void setAudioDevices(std::vector<AudioDeviceInfo> devices);
    void setAudioChannels(green::StringArray inputs, green::StringArray outputs);
    [[nodiscard]] AudioDeviceSetup audioDeviceSetup() const;

    [[nodiscard]] double sampleRate() const;
    void setSampleRate(double rate);
    bool setAsioBufferSizes(AsioBufferSizes sizes);
    void setSystemSetting(int id, int index);
    void setSettingsStoreBackend(SettingsStoreBackend backend);
    void loadBitmap(int id, std::string_view path);
    void setSchematicFilePath(std::string filePath);
    [[nodiscard]] const AppDialogRequest* pendingAppDialog() const;
    [[nodiscard]] bool hasPendingHttpRequest(const HttpRequest& request) const;
    bool finishHttpRequest(const HttpRequest& request, HttpResponse response);
    bool finishAppDialog(std::uint64_t token, bool accepted, std::string path = {},
                         std::optional<yellow::Color> selectedColor = {});
    bool finishRubyMessageBox(std::uint64_t token, std::string_view result);
    [[nodiscard]] const std::optional<DropListRequest>& pendingDropList() const;
    void finishDropList(std::uint64_t token, std::optional<int> selection);
    bool openPopupList(int id);
    bool openIndexSelector(int id, green::Point position);
    [[nodiscard]] std::optional<yellow::PanelControlId> takeEditControlStart();
    std::uint64_t beginEditControl(int id, std::uint64_t rubySession = 0);
    bool changeEditControl(int id, std::uint64_t session, std::string text);
    bool finishEditControl(int id, std::uint64_t session);
    bool receiveFileDrop(int id, std::span<const DropFile> files, green::Point position, FileDropPhase phase);
    bool moveFileDrag(int id, green::Point position);
    bool leaveFileDrag(int id);
    bool sendMouseMessage(int id, const yellow::MouseMessage& message, MouseDragEffects* effects = nullptr);
    bool rubyMouseContains(int id, int input, green::Point position);
    [[nodiscard]] std::optional<yellow::MouseCursor> rubyMouseCursor(int id, int input, green::Point position);
    bool sendRubyMouseMessage(int id, int input, const yellow::MouseMessage& message);
    void cancelRubyMouseDrag(int id, int input);
    bool cancelMouseDrag(const yellow::PanelControlId& source);
    bool releaseMouseCapture();
    bool beginDragFiles(std::uint64_t token);
    void finishDragFiles(std::uint64_t token, bool dropped);
    void rebaseMouseDrag(const yellow::PanelControlId& source, green::Point position);
    bool movePanelPointer(int panelId, green::Point position);
    [[nodiscard]] CursorVisibility cursorVisibility() const;

private:
    friend class Session;
    friend class desktop::SchematicFile;
    Schematic(BitmapLoader bitmapLoader, Schematic* parent, int moduleId);

    struct WirelessSource {
        const Schematic* graph;
        int id;
        bool operator==(const WirelessSource&) const = default;
    };
    using WirelessSources = std::vector<WirelessSource>;

    [[nodiscard]] WirelessSources wirelessSources(int receiver) const;
    [[nodiscard]] std::optional<green::Value> readWireless(int id, green::Type type) const;
    void publishWireless(WirelessSource source);
    void refreshConnections(bool notify = true, std::optional<int> newReceiverOutput = {});
    void refreshCodeSources();
    bool refreshingCodeSources_ = false;
    void refreshWirelessRoutes(bool notify, std::optional<WirelessSource> newReceiverOutput);

    struct PortTypes {
        std::vector<std::optional<green::Type>> inputs;
        std::vector<std::optional<green::Type>> outputs;

        bool operator==(const PortTypes&) const = default;
    };

    struct NodePorts {
        std::vector<std::optional<green::InputId>> inputs;
        std::vector<std::optional<green::OutputId>> outputs;
        PortTypes types;
    };

    struct DspSource {
        const Schematic* graph;
        int node;
        int port;
        bool input = false;
        std::optional<std::size_t> midiChannel;
    };
    struct FloatArrayReadBinding {
        const Schematic* graph;
        int node;
        std::shared_ptr<stream::FloatArrayTable> table;
        void refresh() const;
    };
    struct MatrixRouteBinding {
        const Schematic* graph;
        int node;
        std::vector<int> wiredInputs;
        std::shared_ptr<stream::RouteMixTable> table;
        std::optional<std::uint64_t> revision;
        void refresh();
    };
    struct WaveBufferBinding {
        const Schematic* graph;
        int node;
        std::shared_ptr<stream::WaveBuffer> buffer;
        void refresh() const;
    };
    [[nodiscard]] std::optional<DspSource> resolveDspSource(int id, int output, const Schematic* host = nullptr) const;
    [[nodiscard]] std::optional<std::int32_t> voiceIndex(int id) const;
    [[nodiscard]] int routerSelection(const SchematicNode& entry) const;
    [[nodiscard]] static std::optional<int> routedDataInput(PrimitiveKind kind, int selected, int output);
    void captureSignals(SchematicNode& entry);
    void sampleMono(SchematicNode& entry);
    enum class DspRender { Outputs, Inputs, Audio, Plugin };
    struct DspRenderGraph;
    [[nodiscard]] DspRenderGraph buildDspGraph(int id, std::size_t portCount, DspRender render) const;
    void findAudioOutput(std::optional<DspSource>& output) const;
    void clearAudio();
    AudioControlState audioControls_;
    void notifyAudioControls(PrimitiveKind kind);
    [[nodiscard]] green::Value readAudioControl(const SchematicNode& entry, int port) const;
    void receiveAudioControl(SchematicNode& entry, int input);
    [[nodiscard]] AudioPatch compileAudioPatch(DspRenderGraph graph, std::size_t blockSize) const;
    [[nodiscard]] std::optional<std::size_t> pluginAudioChannel(const SchematicNode& entry) const;
    [[nodiscard]] const SchematicNode* localPluginConfig(PrimitiveKind kind) const;
    [[nodiscard]] std::string localExportFolder() const;
    [[nodiscard]] std::string readStartFolder(int id) const;
    void notifyStartFolderPreviews();
    void receivePluginHost(SchematicNode& entry, int input);
    std::array<green::IntArray, 2> pluginAuxStates_;
    [[nodiscard]] std::vector<std::vector<double>> renderDsp(int id, std::size_t frames, std::size_t blockSize,
        bool captureInputs, std::string* compiledCode = nullptr, std::span<float> timings = {}) const;
    struct DspTraceKey {
        std::vector<int> path;
        int node;
        int port;
        bool input = false;
        auto operator<=>(const DspTraceKey&) const = default;
    };
    [[nodiscard]] std::optional<DspTraceKey> dspTraceKey(int id, int port, bool output) const;
    struct AudioMonitor;
    [[nodiscard]] std::shared_ptr<const AudioMonitor> audioMonitor() const;
    [[nodiscard]] std::optional<std::vector<double>> liveDspTrace(int id, int port, bool output, std::size_t frames) const;
    [[nodiscard]] std::optional<std::vector<std::vector<double>>> captureLiveMono(int id, int channels, std::size_t frames) const;
    mutable std::weak_ptr<const AudioMonitor> liveAudio_;
    mutable std::map<DspTraceKey, std::vector<double>> dspTraces_;
    struct MonoPreview;
    std::map<std::pair<std::vector<int>, int>, std::shared_ptr<MonoPreview>> monoPreviews_;
    std::optional<TickTime> tickDispatchTime_;
    double sampleRate_ = 44100.0;
    TickTime goalStart_{std::chrono::steady_clock::now()};
    std::uint64_t audioTopology_ = 0;
    mutable std::shared_ptr<std::atomic<std::uint64_t>> audioResetRequests_;

    static void applyDspCode(std::span<const std::pair<Schematic*, int>> targets, std::string source);
    [[nodiscard]] const SchematicNode& node(int id) const;
    [[nodiscard]] SchematicNode& node(int id);
    [[nodiscard]] bool wouldCreateFeedback(SchematicLink link) const;
    enum class ConnectorSide { Input, Output };
    [[nodiscard]] bool reachesConnector(int node, int port, ConnectorSide side, int target, int targetPort) const;
    void validatePropertyRoutes() const;
    void reparent(Schematic& parent, int moduleId);
    [[nodiscard]] green::Value readInput(int id, int input) const;
    [[nodiscard]] green::Value transformArray(int id, PrimitiveKind kind, green::Type type) const;
    [[nodiscard]] bool hasValueInput(int id, int input) const;
    [[nodiscard]] TickReadout readTick(const SchematicNode& entry) const;
    void advanceTicks(TickTime now, TickWake wake, TickSchedule& result);
    void advanceSlide(SchematicNode& entry, TickTime now, TickWake wake, TickSchedule& result);
    void advanceTimer(SchematicNode& entry, TickTime now, TickWake wake, TickSchedule& result);
    void collectTimerDeadlines(TickSchedule& schedule) const;
    void receiveTimerTrigger(SchematicNode& entry, int input);
    [[nodiscard]] std::array<std::int32_t, 8> sampleLocalTime() const;
    [[nodiscard]] green::Value readNoteConversion(const SchematicNode& entry) const;
    [[nodiscard]] green::Value readGeneralProcessing(const SchematicNode& entry, int output) const;
    void receiveGeneralPulse(SchematicNode& entry, int input);
    void collectGoals(std::vector<const SchematicNode*>& goals) const;
    void publishGoalProgress(bool reset);
    void receiveWindowTrigger(SchematicNode& entry, int input);
    [[nodiscard]] yellow::View windowView(const SchematicNode& entry) const;
    void appendRequestedWindows(std::vector<WindowPanel>& windows) const;
    [[nodiscard]] bool setWindowSize(SchematicNode& entry, int width, int height);
    [[nodiscard]] bool setWindowVisible(SchematicNode& entry, bool visible);
    [[nodiscard]] yellow::View moduleParentView(const SchematicNode& entry) const;
    [[nodiscard]] yellow::Area anchoredPanelArea(const SchematicNode& entry, const yellow::View& parent,
                                                green::Point position) const;
    [[nodiscard]] yellow::View panelView(const SchematicNode& mgui, const yellow::View& parentView) const;
    [[nodiscard]] Schematic& findPanelItem(int moduleId, std::span<const int> path,
                                          ModulePanelKind surface, ModulePanelKind item);
    void setPanelAnchors(int panel, std::uint8_t anchors);
    [[nodiscard]] std::optional<yellow::GraphDrawCommand> readGraphDrawing(const SchematicNode& node,
                                                                          yellow::Area area) const;
    [[nodiscard]] std::optional<yellow::GraphDrawCommand> readPointPlot(const SchematicNode& node,
                                                                         yellow::Area area) const;
    [[nodiscard]] std::optional<yellow::BitmapDrawCommand> readBitmapDrawing(const SchematicNode& node) const;
    [[nodiscard]] yellow::PaintTool readBrush(const SchematicNode& entry) const;
    [[nodiscard]] green::Value brushInputDefault(const SchematicNode& entry, int input) const;
    [[nodiscard]] green::Value readAreaGeometry(const SchematicNode& entry, int output) const;
    [[nodiscard]] green::Value readStringOperation(const SchematicNode& entry, int output) const;
    [[nodiscard]] green::Value readStringConversion(const SchematicNode& entry) const;
    [[nodiscard]] green::Value readMemory(const SchematicNode& entry, int output) const;
    void receiveMemoryTrigger(SchematicNode& entry, int input);
    [[nodiscard]] green::Value readWaveFile(const SchematicNode& entry, int output) const;
    void receiveWaveFile(SchematicNode& entry, int input);
    void createWaveTable(SchematicNode& entry);
    void receiveMidiTrigger(SchematicNode& entry, int input);
    void routeNoteLayers(SchematicNode& entry, int input);
    [[nodiscard]] green::Value readVoiceRouting(const SchematicNode& entry) const;
    void receiveVoiceRouting(SchematicNode& entry, int input);
    void receiveVoiceSignal(SchematicNode& entry, int input);
    void receiveHostMidiTrigger(SchematicNode& entry, int input);
    struct PresetParticipant {
        Schematic* graph;
        SchematicNode* node;
    };
    [[nodiscard]] std::optional<PresetParticipant> presetManagerOnInput(int id);
    [[nodiscard]] std::vector<PresetParticipant> presetParticipants(std::span<const int> managerPath);
    [[nodiscard]] green::Value presetValueForProgram(const SchematicNode& entry, int program) const;
    [[nodiscard]] std::string presetParameterName(const SchematicNode& entry) const;
    [[nodiscard]] bool presetParameterAutomatable(const SchematicNode& entry) const;
    [[nodiscard]] bool presetParameterStored(const SchematicNode& entry) const;
    [[nodiscard]] double presetParameterOrder(const SchematicNode& entry) const;
    void receivePresetUtility(SchematicNode& entry, int input);
    [[nodiscard]] PresetTable capturePresetTable(PresetParticipant manager, std::span<const PresetParticipant> participants,
                                                 int first, int last, std::vector<std::string> names);
    std::string applyPresetTable(PresetParticipant manager, std::span<const PresetParticipant> participants,
                                 PresetTable table, int first, int limit, int column, bool positional = false);
    [[nodiscard]] green::Value readPresetManager(const SchematicNode& entry, int output) const;
    [[nodiscard]] green::Value readPresetParameter(const SchematicNode& entry, int output) const;
    [[nodiscard]] green::Value readPluginHost(const SchematicNode& entry, int output) const;
    [[nodiscard]] int presetProgramCount(const SchematicNode& entry) const;
    [[nodiscard]] green::Value defaultPresetParameter(const SchematicNode& entry) const;
    [[nodiscard]] green::Value currentPresetParameter(const SchematicNode& entry) const;
    void receivePresetManager(SchematicNode& entry, int input);
    void receivePresetParameter(SchematicNode& entry, int input);
    void changePresetParameter(SchematicNode& entry, green::Value value, bool edit, bool record);
    void collectPluginParameters(std::vector<PluginParameter>& parameters) const;
    void notifyPrimitiveOutputs(PrimitiveKind kind, std::optional<std::size_t> output = std::nullopt);
    void refreshPluginLatency();
    PluginHostCallbacks pluginHostCallbacks_;
    stream::HostTransport pluginHostTransport_;
    bool pluginEditorOpen_ = false;
    bool hostRedrawsSuspended_ = false;
    unsigned presetRedrawDepth_ = 0;
    void publishRedrawSuspension(bool previous);
    void withPresetRedrawsSuspended(const std::function<void()>& action);
    [[nodiscard]] green::Value readPanelUtility(const SchematicNode& entry) const;
    void receivePanelUtility(SchematicNode& entry);
    PresentationState presentation_;
    [[nodiscard]] Schematic* presentationOwner(std::span<const int> path);
    [[nodiscard]] bool inPresentation() const;
    [[nodiscard]] green::Value readPresentationControl(PrimitiveKind kind) const;
    void receivePresentationControl(const SchematicNode& entry, int input);
    std::bitset<256> pressedKeys_;
    bool dispatchKeyEvent(const KeyboardEvent& event);
    [[nodiscard]] bool isKeyPressed(std::string_view key) const;
    std::optional<int> reportedPluginLatency_;
    std::function<void(const green::MidiMessage&)> hostMidiOutput_;
    [[nodiscard]] float readMidiStream(const SchematicNode& entry,
                                       std::optional<std::size_t> midiChannel = {}) const;
    void receiveMidiDeviceTrigger(SchematicNode& entry, int input);
    [[nodiscard]] green::Value readMidiDevice(const SchematicNode& entry, int output) const;
    using MidiDeviceOwner = std::pair<std::vector<int>, int>;
    struct MidiDeviceLease {
        MidiDeviceOwner owner;
        std::unique_ptr<MidiInputPort> input;
        std::unique_ptr<MidiOutputPort> output;
    };
    struct MidiDeviceList {
        std::vector<MidiDeviceInfo> ports;
        std::map<std::string, MidiDeviceLease, std::less<>> leases;
        std::set<std::string, std::less<>> unavailable;
    };
    struct MidiDeviceState {
        std::shared_ptr<MidiDeviceBackend> backend;
        std::array<MidiDeviceList, 2> lists;
        bool watching = false;
        bool scanned = false;
        bool refreshing = false;
        bool refreshRequested = false;
    };
    MidiDeviceState midiDevices_;
    std::shared_ptr<SerialDeviceBackend> serialBackend_;
    std::vector<SerialDeviceInfo> serialPorts_;
    bool refreshingSerialPorts_ = false;
    void refreshSerialPorts(bool notify);
    void receiveSerialDevice(SchematicNode& entry, int input);
    void setSerialOpen(SchematicNode& entry, bool open);
    void logSerial(SchematicNode& entry, std::string_view line);
    bool pollSerialPort(SchematicNode& entry);
    [[nodiscard]] green::Value readSerialDevice(const SchematicNode& entry, int output) const;
    void publishMidi(SchematicNode& entry, green::MidiMessage message, int output = 0);
    void advanceMidiNotes(SchematicNode& entry, TickTime now, TickSchedule& result);
    [[nodiscard]] green::Value readSystemInfo(const SchematicNode& entry, int output) const;
    void refreshSystemFonts() const;
    [[nodiscard]] yellow::Area measureText(int id) const;
    [[nodiscard]] yellow::View transformView(const SchematicNode& entry) const;
    [[nodiscard]] green::Value readViewAttribute(const SchematicNode& entry, int output) const;
    void receiveSetGuiArea(SchematicNode& entry, int input);
    void receiveRedraw(SchematicNode& entry, int input);
    bool requestRedraw(const yellow::View& view, std::optional<yellow::Area> area);
    void clearViewCache(SchematicNode& entry, const yellow::PanelControlId* panel = nullptr);
    static void updatePanelPaint(std::vector<yellow::PanelDrawing>& previous,
                                 std::vector<yellow::PanelDrawing> fresh,
                                 const yellow::ClipRegion& dirty, yellow::Area bounds);
    [[nodiscard]] const Schematic* panelOwner(const yellow::View& view) const;
    void applyPanelArea(int panel, yellow::Area area);
    struct DrawingPass;
    [[nodiscard]] bool tracingDrawRoutes() const;
    void planPanelDrawings(const yellow::View& target, DrawingPass& pass) const;
    [[nodiscard]] std::vector<yellow::PanelDrawing> evaluatePanel(const yellow::View& view, double rasterScale,
                                                                  SchematicNode* panel, bool publish) const;
    void appendRubyDrawing(const SchematicNode& entry, int input, const yellow::View& view,
                           std::vector<yellow::PanelDrawing>& drawings, bool publish) const;
    void appendDrawing(const SchematicNode& entry, yellow::View view,
                       std::vector<yellow::PanelDrawing>& drawings) const;
    [[nodiscard]] yellow::View readDrawBoundary(const SchematicNode& entry, yellow::DrawBoundaryKind kind) const;
    [[nodiscard]] yellow::ViewEffect readViewEffect(const SchematicNode& entry) const;
    [[nodiscard]] green::Value readViewLoop(const SchematicNode& entry, int output,
                                           green::Value fallback) const;
    [[nodiscard]] std::optional<yellow::DrawCommand> readShape(const SchematicNode& node, yellow::Area area) const;
    [[nodiscard]] PortTypes resolveTypes(const SchematicNode& node,
        const std::vector<std::vector<green::Type>>& sources, std::optional<green::Type> hint) const;
    NodePorts makePorts(green::Runtime& runtime, const SchematicNode& node, PortTypes types);
    void receiveTrigger(int id, int input);
    void updateRubyContext(const SchematicNode& entry) const;
    void installRubyCode(SchematicNode& entry, RubyCodeState code,
                         std::optional<bool> outputSide = {}, std::span<const int> oldToNew = {});
    void receiveRuby(SchematicNode& entry, int input);
    void dispatchRuby(SchematicNode& entry, const RubyAction& action);
    void publishRuby(SchematicNode& entry, RubyReply reply, TickTime now, bool mayChangeState = true);
    [[nodiscard]] TickTime rubyNow() const;
    [[nodiscard]] double rubySeconds(TickTime now) const;
    void clearRubyEvents(const std::shared_ptr<RubyInstance>& owner);
    void clearRubyBranch(const SchematicNode& entry);
    void advanceRubyEvents(TickTime now, TickSchedule& result);
    void collectRubyDeadline(TickSchedule& result);
    struct ScheduledRubyEvent {
        std::weak_ptr<RubyInstance> owner;
        std::vector<int> path;
        int node;
        RubyAction action;
    };
    [[nodiscard]] std::pair<Schematic*, SchematicNode*> rubyTarget(const ScheduledRubyEvent& event);
    void pruneRubyEvents();
    [[nodiscard]] bool compareInputs(const SchematicNode& entry) const;
    [[nodiscard]] green::Value readGreenLogic(const SchematicNode& entry, green::Type type, int output) const;
    [[nodiscard]] std::string readIfThenElse(int id) const;
    void receiveGreenLogicTrigger(SchematicNode& entry, int input);
    void primeGreenLogic();
    [[nodiscard]] std::vector<MatrixConnection> matrixConnections(const SchematicNode& entry) const;
    [[nodiscard]] green::Value readMatrix(const SchematicNode& entry, green::Type type, int output) const;
    void receiveMatrixTrigger(SchematicNode& entry, int input);
    void refreshMatrix(SchematicNode& entry, bool notify);
    void refreshMatrices(bool notify);
    void primeStateMachines();
    void primeEmbeddedFonts();
    void receiveFontEmbed(SchematicNode& entry);
    [[nodiscard]] green::Value readStateMachine(const SchematicNode& entry, int output) const;
    void receiveStateMachine(SchematicNode& entry, int input);
    void receiveTriggerToolPulse(SchematicNode& entry, int input);
    void runNumericLoop(SchematicNode& entry);
    void receiveAdvancedCounterPulse(SchematicNode& entry, int input);
    [[nodiscard]] green::Value readStoredArray(const SchematicNode& entry, int output, green::Type type) const;
    void receiveStoredArrayTrigger(SchematicNode& entry, int input);
    [[nodiscard]] green::Value readArrayTool(const SchematicNode& entry, int output) const;
    [[nodiscard]] green::FloatArray readBiquadResponse(int id, bool phase) const;
    void receiveArrayToolTrigger(SchematicNode& entry, int input);
    void runArrayLoop(SchematicNode& entry);
    [[nodiscard]] green::Value readArrayLength(int id) const;
    void receiveArrayDrawTrigger(SchematicNode& entry, int input);
    [[nodiscard]] green::Value readDropList(const SchematicNode& entry, int output) const;
    [[nodiscard]] std::vector<std::string> indexSelectorOptions(int id) const;
    [[nodiscard]] green::Value readIndexSelector(const SchematicNode& entry, int output) const;
    void receiveIndexSelectorTrigger(SchematicNode& entry, int input);
    void receiveDropListTrigger(SchematicNode& entry, int input);
    void createRubyDropList(SchematicNode& entry, int input, green::RubyValue callbackId, RubyDropListOptions options);
    void clearRubyDropList(int id);
    [[nodiscard]] green::Value readPopupList(const SchematicNode& entry, int output) const;
    [[nodiscard]] yellow::PopupListCommand readPopupListDrawing(const SchematicNode& entry, yellow::Area area) const;
    [[nodiscard]] DropListSelection popupListSelection(const SchematicNode& entry,
                                                       const std::vector<std::string>& options) const;
    void receivePopupListTrigger(SchematicNode& entry, int input);
    void publishPopupListSelection(SchematicNode& entry, std::int32_t index);
    [[nodiscard]] green::Value readEditControl(const SchematicNode& entry, int output) const;
    [[nodiscard]] yellow::EditControlCommand readEditControlDrawing(const SchematicNode& entry, yellow::Area area) const;
    void receiveEditControlTrigger(SchematicNode& entry, int input);
    [[nodiscard]] SchematicNode* findEditControl(int id);
    void createRubyEdit(SchematicNode& entry, int input, green::RubyValue callbackId, RubyEditOptions options);
    void dispatchRubyEdit(SchematicNode& entry, bool finished, const green::RubyValue& callbackId,
                          std::string_view text);
    void appendRubyEditControls(const SchematicNode& entry, int input, const yellow::View& view,
                                std::vector<yellow::PanelDrawing>& drawings) const;
    [[nodiscard]] green::Value readDropFiles(const SchematicNode& entry, int output) const;
    [[nodiscard]] SchematicNode* findDropFiles(int id);
    void publishFileDrag(SchematicNode& entry, std::optional<FileDrag> next, bool dropped);
    [[nodiscard]] std::optional<yellow::MouseRegion> readMouseRegion(const SchematicNode& entry) const;
    using MouseAreas = std::map<yellow::PanelControlId, yellow::MouseAreaCommand*>;
    [[nodiscard]] std::optional<yellow::PanelControlId> findMouseAreaSource(int id) const;
    void applyMouseInfo(MouseAreas& areas) const;
    [[nodiscard]] green::Value readMouseDrag(const SchematicNode& entry, int output) const;
    [[nodiscard]] SchematicNode* findRubyMouseNode(int id, int input);
    [[nodiscard]] std::optional<yellow::MouseCursor> hitRubyMouse(const SchematicNode& entry, int input,
                                                               const yellow::View& view, green::Point position,
                                                               green::Point local, bool queryCursor = false) const;
    [[nodiscard]] std::optional<yellow::MouseCursor> queryRubyMouse(int id, int input, green::Point position,
                                                                 bool queryCursor);
    [[nodiscard]] CursorVisibility nativeCursorVisibility() const;
    [[nodiscard]] std::int64_t rubyCursorBalance() const;
    void receiveMouseMessage(SchematicNode& entry);
    void receiveMouseDrag(SchematicNode& entry, int input);
    void receiveMouseCapture(SchematicNode& entry, int input);
    [[nodiscard]] bool capturesMouseArea(const yellow::PanelControlId& source) const;
    std::optional<yellow::PanelControlId> mouseCaptureOwner_;
    std::shared_ptr<stream::SequenceTransport> sequenceTransport_ = std::make_shared<stream::SequenceTransport>();
    TickTime rubyClockStart_{};
    std::optional<TickTime> rubyDispatchTime_;
    std::multimap<TickTime, ScheduledRubyEvent> rubyEvents_;
    TickTime sequenceStart_{};
    TickTime::duration sequenceDuration_{};
    [[nodiscard]] TickTime sequenceNow() const;
    [[nodiscard]] TickTime::duration sequenceElapsed(TickTime now) const;
    [[nodiscard]] std::shared_ptr<stream::SequenceAudioTrack> sequencerAudio(const SchematicNode& entry) const;
    void receiveSequenceMidi(SchematicNode& entry);
    void receiveNetwork(SchematicNode& entry, int input);
    void receiveMedia(SchematicNode& entry, int input);
    bool pollMediaPlayback(SchematicNode& entry);
    [[nodiscard]] green::Value readMedia(const SchematicNode& entry, int output) const;
    [[nodiscard]] std::shared_ptr<MediaAudioBuffer> mediaAudio(const SchematicNode& entry) const;
    [[nodiscard]] MediaOptions mediaOptions(const SchematicNode& entry) const;
    [[nodiscard]] green::Value readNetwork(const SchematicNode& entry, int output) const;
    bool pollNetworkConnection(SchematicNode& entry);
    void receiveSfz(SchematicNode& entry, int input);
    [[nodiscard]] green::Value readSfz(const SchematicNode& entry, int output) const;
    [[nodiscard]] green::Value readSfzColumn(const SchematicNode& entry, int column) const;
    void receiveShowCursor(int id);
    [[nodiscard]] green::Value readDragFiles(const SchematicNode& entry, int output) const;
    void receiveDragFilesTrigger(SchematicNode& entry, int input);
    void rebaseMouseDrags(const yellow::PanelControlId& source, green::Point position);
    bool clearMouseDrags(const yellow::PanelControlId& source);
    bool publishMouseMove(const yellow::View& target, const yellow::MouseMessage& message);
    struct RuntimeBuild {
        std::unique_ptr<green::Runtime> runtime;
        std::map<int, NodePorts> ports;
    };
    using RuntimeBuilds = std::map<const Schematic*, RuntimeBuild>;
    static void resolveDspStreams(RuntimeBuilds& builds);
    [[nodiscard]] RuntimeBuild buildRuntime(bool keepTypes);
    void wireRuntime(RuntimeBuild& build);
    void installRuntime(RuntimeBuild build);
    void rebuildRuntime(bool keepTypes = false, bool commit = true);
    void rebuildConnectedRuntimes(bool commit = true, bool forceWholeTree = false);
    [[nodiscard]] std::string nextModulePortName(bool input, int except) const;
    [[nodiscard]] const SchematicNode* propertyBoundary(const SchematicNode& property) const;
    [[nodiscard]] const SchematicNode* propertyWriter(const SchematicNode& input) const;
    void setPropertyPortName(int id, std::string name);
    void updateModulePorts(bool input, int removed = -1);
    static void setModulePortTypes(std::span<const std::pair<Schematic*, int>> targets,
                                   std::optional<green::Type> type);
    void validateLink(SchematicLink link) const;
    void checkConnection(SchematicLink link);
    void checkTemplateType(int id, std::optional<green::Type> type);
    void eraseNodes(std::span<const int> ids);
    void eraseLinks(const std::function<bool(SchematicLink)>& matches);
    void rebuildAfterRemoval();
    [[nodiscard]] std::map<int, NodePorts> resolvePorts(const RuntimeBuilds* staged = nullptr) const;
    void wire(green::Runtime& runtime, const std::map<int, NodePorts>& ports, SchematicLink link);
    struct BusSource {
        const Schematic* graph;
        int id;
    };
    struct BusPulse {
        BusSource source;
        int input;
    };
    [[nodiscard]] Schematic& rootGraph();
    [[nodiscard]] const Schematic& rootGraph() const;
    [[nodiscard]] std::optional<BusSource> findBusSource(int extractor) const;
    [[nodiscard]] int busChannelInput(const BusSource& source, std::string_view name) const;
    [[nodiscard]] std::optional<green::Type> busChannelType(const BusSource& source,
        std::string_view name, const std::map<int, NodePorts>& building, bool staged = false) const;
    bool refreshBusTree();
    void refreshBusPorts();
    void fireBusChannel(SchematicNode& entry, int input);
    void receiveBusPulse(SchematicNode& entry);
    void fireOutputs(int id);
    void fireOutput(SchematicNode& entry, std::size_t index);
    void fireLifecycleTriggers(PrimitiveKind kind, std::span<SchematicNode> entries, std::exception_ptr& error);
    void armCloseHolds(std::exception_ptr& error);
    [[nodiscard]] bool hasCloseHolds() const;
    [[nodiscard]] yellow::Bitmap decodeBitmap(std::string_view path) const;
    void storeBitmap(int id, yellow::Bitmap bitmap);
    void loadBitmapIntoNode(int id, std::string_view path);
    void storeBitmapArrayEntry(SchematicNode& entry, std::int32_t index, yellow::Bitmap bitmap);
    void receiveBitmapOperation(SchematicNode& entry, int input);
    void receiveSvg(SchematicNode& entry, int input);
    void renderBitmap(SchematicNode& entry, int input);
    [[nodiscard]] yellow::View bitmapView(const SchematicNode& entry) const;
    [[nodiscard]] green::Value readBitmapMetadata(const SchematicNode& entry, int output) const;

    std::vector<SchematicNode> nodes_;
    std::vector<SchematicLink> links_;
    std::map<SchematicLink, std::uint64_t> linkTriggers_;
    std::unique_ptr<green::Runtime> runtime_;
    BitmapLoader bitmapLoader_;
    BitmapBackend bitmapBackend_;
    TextMeasurer textMeasurer_;
    std::shared_ptr<SchematicLocation> location_;
    std::deque<AppDialogRequest> appDialogs_;
    [[nodiscard]] const Schematic* appDialogOwner(const AppDialogRequest& request) const;
    void createRubyMessageBox(SchematicNode& entry, green::RubyValue callbackId, RubyMessageBoxOptions options);
    void clearRubyMessageBoxes(int id);
    void receiveFileUtility(SchematicNode& entry, int input);
    void receiveSystemAction(SchematicNode& entry, int input);
    void receiveHttpRequest(SchematicNode& entry, int input);
    [[nodiscard]] green::Value readHttpResponse(const SchematicNode& entry, int output) const;
    [[nodiscard]] const Schematic* httpRequestOwner(const HttpRequest& request) const;
    void receiveColorMatrix(SchematicNode& entry);
    [[nodiscard]] green::Value readParsedColorMatrix(const SchematicNode& entry, int output) const;
    [[nodiscard]] green::Value readFileUtility(const SchematicNode& entry, int output) const;
    [[nodiscard]] float readSystemSetting(const SchematicNode& entry) const;
    void receiveSettingsStore(SchematicNode& entry, int input);
    SettingsStoreBackend settingsStore_;
    TextFileBackend textFiles_;
    SystemInfoBackend systemInfo_;
    mutable std::optional<green::StringArray> systemFontNames_;
    Schematic* parent_;
    std::vector<int> path_;
    std::map<int, WirelessSources> wirelessRoutes_;
    std::map<int, std::unique_ptr<Schematic>> modules_;
    std::map<int, NodePorts> ports_;
    int nextId_ = 1;
    std::uint64_t syncGroup_ = 0;
    std::uint64_t triggers_ = 0;
    struct DeferredTrigger {
        std::vector<int> path;
        int node;
    };
    std::deque<DeferredTrigger> deferredTriggers_; // FIFO at the root, shared by all module levels.
    std::uint64_t busLayoutRevision_ = 0;
    std::uint64_t panelGeometryRevision_ = 0;
    std::optional<DropListRequest> dropListRequest_; // One pending popup at the root.
    std::optional<yellow::PanelControlId> editControlStart_;
    std::uint64_t drawingTopology_ = 0;
    std::uint64_t redrawRevision_ = 0;
    mutable std::uint64_t nextCacheRevision_ = 0;
    mutable DrawingPass* drawingPass_ = nullptr;
    std::optional<BusPulse> busPulse_; // Stored at the root during synchronous delivery.
    struct HiddenCursor {
        yellow::PanelControlId source;
        bool hold;
    };
    std::optional<HiddenCursor> hiddenCursor_; // The active hide request is owned by the root.
    std::uint64_t cursorVisibilityRevision_ = 0;

    struct MouseDispatch {
        const yellow::MouseMessage& message;
        yellow::PanelControlId source;
        MouseDragEffects drag;
    };
    MouseDispatch* mouseDispatch_ = nullptr;
    struct DragFilesSession {
        std::uint64_t token;
        yellow::PanelControlId owner;
        bool dragging = false;
    };
    [[nodiscard]] Schematic* dragFilesOwner(const DragFilesSession& session);
    std::optional<DragFilesSession> dragFilesSession_; // One native drag per schematic tree.
    std::uint64_t nextDragFilesToken_ = 0;
};

}
