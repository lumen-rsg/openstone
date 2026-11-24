#pragma once

#include "green/Point.hpp"
#include "yellow/Bitmap.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace openstone::yellow {

// Position and signed extent in the view's local grid units.
struct Area {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;

    bool operator==(const Area&) const = default;
};

using ClipPolygon = std::vector<green::Point>;
using ClipRegion = std::variant<std::vector<Area>, std::vector<ClipPolygon>>;

struct ViewMapping {
    green::Point origin;
    float scale = 1.0F;
    double rotation = 0; // Clockwise degrees.
    // Nonoverlapping pieces in panel coordinates. A disengaged clip uses the full panel.
    std::optional<ClipRegion> clip;

    [[nodiscard]] green::Point toPanel(green::Point local) const;
    [[nodiscard]] green::Point mapVector(green::Point local) const;
    [[nodiscard]] green::Point fromPanel(green::Point position) const;
    [[nodiscard]] Area mapArea(Area local) const;
    [[nodiscard]] bool fullyClipped() const;
    [[nodiscard]] bool contains(green::Point local) const;

    bool operator==(const ViewMapping&) const = default;
};

struct PanelControlId {
    std::vector<int> ownerPath;
    int node;

    auto operator<=>(const PanelControlId&) const = default;
};

enum class DrawBoundaryKind { Loop, Cache, Effect };

struct DrawBoundary {
    PanelControlId id;
    DrawBoundaryKind kind;

    bool operator==(const DrawBoundary&) const = default;
};

// Node IDs are local to each schematic. Zero denotes a missing panel.
struct View {
    int panel = 0;
    int parentPanel = 0;
    Area bounds;
    bool mouseMoves = false;
    std::vector<int> schematicPath;
    std::vector<int> parentSchematicPath;
    std::vector<std::size_t> drawOrder; // Link order along the route from the drawing surface.
    ViewMapping mapping;
    std::vector<DrawBoundary> drawRoute; // Used when finding drawing routes.

    [[nodiscard]] bool isSamePanel(const View& target) const
    {
        return panel != 0 && panel == target.panel && schematicPath == target.schematicPath;
    }

    [[nodiscard]] bool attachesTo(const View& target) const
    {
        return target.panel != 0 && parentPanel == target.panel && parentSchematicPath == target.schematicPath;
    }

    bool operator==(const View&) const = default;
};

enum class MouseEventKind : std::uint8_t {
    None, Move, Drag, ButtonDown, ButtonUp, Click, DoubleClick, Wheel,
};

enum class MouseButton : std::uint8_t { None, Left, Middle, Right, Back, Forward };

// Signed horizontal and vertical wheel rotation in eighths of a degree.
struct MouseWheelDelta {
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const MouseWheelDelta&) const = default;
};

struct MouseMessage {
    MouseEventKind kind = MouseEventKind::None;
    green::Point position; // Local coordinates in grid units.
    MouseButton button = MouseButton::None; // Button events and drags.
    MouseWheelDelta wheelDelta; // Wheel events only.

    bool operator==(const MouseMessage&) const = default;
};

struct Color {
    std::uint32_t argb = 0; // Straight alpha in 0xAARRGGBB order.

    bool operator==(const Color&) const = default;
};

using ColorChannels = std::array<std::int32_t, 4>; // Alpha first, then RGB or HSV.

[[nodiscard]] ColorChannels colorToArgb(Color color) noexcept;
[[nodiscard]] ColorChannels colorToAhsv(Color color) noexcept;
[[nodiscard]] Color colorFromArgb(ColorChannels channels) noexcept;
[[nodiscard]] Color colorFromAhsv(ColorChannels channels) noexcept;

struct ColorMatrix {
    static constexpr int size = 5;

    // A row vector [R, G, B, A, 1] multiplies the matrix on its left.
    std::array<std::array<float, size>, size> coefficients{{
        {1, 0, 0, 0, 0},
        {0, 1, 0, 0, 0},
        {0, 0, 1, 0, 0},
        {0, 0, 0, 1, 0},
        {0, 0, 0, 0, 1},
    }};

    bool operator==(const ColorMatrix&) const = default;
};

[[nodiscard]] Color applyColorMatrix(const ColorMatrix& matrix, Color color);
[[nodiscard]] Bitmap applyBitmapColorMatrix(const ColorMatrix& matrix, const Bitmap& bitmap);

enum class LineStyle : std::uint8_t { Solid, Dash, Dot, DashDot, DashDotDot };

struct PenStroke {
    Color color{0xFF000000};
    float width = 0.1F; // Grid units. Zero denotes a hairline.
    LineStyle style = LineStyle::Solid;

    bool operator==(const PenStroke&) const = default;
};

struct SolidBrush {
    Color color{0xFF000000};

    bool operator==(const SolidBrush&) const = default;
};

enum class BrushWrap : std::uint8_t { Tile, FlipX, FlipY, FlipXY, Clamp };
enum class RadialShape : std::uint8_t { Ellipse, Rectangle };

enum class HatchStyle : std::uint8_t {
    Horizontal, Vertical, ForwardDiagonal, BackwardDiagonal, Cross, DiagonalCross,
    Percent05, Percent10, Percent20, Percent25, Percent30, Percent40, Percent50,
    Percent60, Percent70, Percent75, Percent80, Percent90,
    LightDownwardDiagonal, LightUpwardDiagonal, DarkDownwardDiagonal, DarkUpwardDiagonal,
    WideDownwardDiagonal, WideUpwardDiagonal, LightVertical, LightHorizontal,
    NarrowVertical, NarrowHorizontal, DarkVertical, DarkHorizontal, DashedDownwardDiagonal,
    DashedUpwardDiagonal, DashedHorizontal, DashedVertical, SmallConfetti, LargeConfetti, ZigZag, Wave,
    DiagonalBrick, HorizontalBrick, Weave, Plaid, Divot, DottedGrid, DottedDiamond, Shingle,
    Trellis, Sphere, SmallGrid, SmallCheckerBoard, LargeCheckerBoard, OutlinedDiamond, SolidDiamond,
};

struct LinearGradientBrush {
    Color start{0xFF000000};
    Color end{0xFF000000};
    Area area;
    float angle = 0; // Clockwise degrees from the positive X axis.
    BrushWrap wrap = BrushWrap::Tile;

    bool operator==(const LinearGradientBrush&) const = default;
};

struct RadialPath {
    std::vector<green::Point> points;
    // Empty types denotes the native polygon encoding. Otherwise these are the
    // original GDI+ Start/Line/Bezier bytes, including close/marker/dash flags.
    std::vector<std::uint8_t> types;
    bool winding = false;

    bool operator==(const RadialPath&) const = default;
};

inline constexpr std::size_t maxRadialPathPoints = 4096;
void validateRadialPath(const RadialPath& path);
[[nodiscard]] Area radialPathBounds(const RadialPath& path);

struct RadialGradientBrush {
    Color start{0xFF000000};
    Color end{0xFF000000};
    Area area;
    RadialShape shape = RadialShape::Ellipse;
    std::optional<green::Point> center; // Local View coordinates. Empty uses the Area center.
    green::Point focus; // Horizontal and vertical scales from zero through one.
    std::optional<RadialPath> boundary;
    // Retain the native count and order, including repeated/transparent colors.
    std::vector<Color> surroundColors;

    bool operator==(const RadialGradientBrush&) const = default;
};

struct HatchBrush {
    Color foreground{0xFF000000};
    Color background{0x00FFFFFF};
    HatchStyle style = HatchStyle::Horizontal;

    bool operator==(const HatchBrush&) const = default;
};

struct TextureBrush {
    Bitmap bitmap;
    BrushWrap wrap = BrushWrap::Tile;

    bool operator==(const TextureBrush&) const = default;
};

using PaintTool = std::variant<PenStroke, SolidBrush, LinearGradientBrush, RadialGradientBrush, HatchBrush, TextureBrush>;

enum class ShapeKind : std::uint8_t { Arc, Ellipse, Line, Pie, Rect };

struct DrawCommand {
    ShapeKind shape;
    Area area;
    PaintTool tool;
    float startAngle = 0.0F;
    float sweepAngle = 0.0F;
    float cornerRadius = 0.0F;
};

[[nodiscard]] LineStyle parseLineStyle(std::string_view text);
[[nodiscard]] std::string_view lineStyleName(LineStyle style);
[[nodiscard]] BrushWrap parseBrushWrap(std::string_view text);
[[nodiscard]] std::string_view brushWrapName(BrushWrap wrap);
[[nodiscard]] RadialShape parseRadialShape(std::string_view text);
[[nodiscard]] HatchStyle parseHatchStyle(std::string_view text);
[[nodiscard]] std::string_view hatchStyleName(HatchStyle style);

struct FontStyle {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikeout = false;

    bool operator==(const FontStyle&) const = default;
};

struct Font {
    std::string face = "Arial";
    float size = 1.0F; // Grid units.
    FontStyle style;
    std::string alternateFace;
    // Additional ordered fallbacks retained from native Font records.
    std::vector<std::string> fallbackFaces;

    bool operator==(const Font&) const = default;
};

[[nodiscard]] FontStyle parseFontStyle(std::string_view text);
[[nodiscard]] std::string fontStyleName(const FontStyle& style);

enum class HorizontalAlignment : std::uint8_t { Left, Center, Right };
enum class VerticalAlignment : std::uint8_t { Top, Center, Bottom };

struct StringFormat {
    HorizontalAlignment horizontal = HorizontalAlignment::Left;
    VerticalAlignment vertical = VerticalAlignment::Top;
    bool wrap = true;
    bool rightToLeft = false;
    bool verticalText = false;
    bool showControls = false;
    bool includeTrailingSpaces = false;
    bool wholeLinesOnly = false;
    bool clip = true;
    bool noFitBlackBox = false;
    bool noFontFallback = false;

    bool operator==(const StringFormat&) const = default;
};

inline constexpr std::uint32_t stringFormatFlagMask = 0x7c27;
[[nodiscard]] StringFormat stringFormatFromFlags(std::uint32_t flags);
[[nodiscard]] std::uint32_t stringFormatFlags(const StringFormat& format) noexcept;

struct TextCommand {
    Area area;
    Color color;
    std::string text;
    Font font;
    StringFormat format;
    bool pointOrigin = false;
};

struct EditControlCommand {
    PanelControlId id;
    Area area;
    Font font;
    Color textColor;
    Color editBackground;
    std::string text;
    bool multiline;
    bool password;
    std::uint64_t rubySession = 0;
};

struct PopupListCommand {
    PanelControlId id;
    Area area;
    Font font;
    Color textColor;
    Color background;
    std::string text;
    bool hasOptions;
};

struct DropFilesCommand {
    PanelControlId id;
    Area area;
    bool draggingOver;
};

struct MousePolygon {
    green::Point minimum;
    green::Point maximum;
    std::vector<green::Point> vertices; // In the View's local grid units.

    [[nodiscard]] bool contains(green::Point position) const;
};

using MouseRegion = std::variant<Area, MousePolygon>;

// The ordinal values are FlowStone's Mouse Info cursor IDs.
enum class MouseCursor : std::uint8_t {
    Default, IBeam, Arrow, HandPointer, Move, PointerAdd, PointerDelete, SmallPointer,
    ResizeHorizontal, ResizeVertical, OpenHand, ClosedHand, Crosshair, Forbidden, Hidden,
    ResizeNorthEastSouthWest, ResizeNorthWestSouthEast, ResizeAll, SplitHorizontal, SplitVertical,
    Help, Wait, Cross, Hand,
};

[[nodiscard]] std::optional<MouseCursor> parseMouseCursor(std::string_view text);

struct MouseHoverInfo {
    std::optional<MouseCursor> cursor;
    std::string tooltip;
};

struct MouseAreaCommand {
    PanelControlId id;
    MouseRegion region;
    bool mouseMoves;
    MouseHoverInfo hover;
    bool captured = false;
    int rubyInput = -1;
};

struct BitmapDrawCommand {
    Bitmap bitmap;
    Area sourcePixels; // Top-left origin in the bitmap.
    Area area; // Destination in the View's grid units.
    ColorMatrix matrix;
    std::uint8_t alpha = 255;
};

enum class GraphStyle : std::uint8_t { Dots, Lines, Curve, PointCurve };

struct GraphVertex {
    double x;
    double y;
    bool startsRun; // A new path after the beginning of the array or a missing sample.
};

struct GraphDrawCommand {
    Area area;
    PaintTool tool;
    GraphStyle style = GraphStyle::Lines;
    float radius = 0; // Grid units, used for dots.
    std::vector<GraphVertex> vertices; // Grid units relative to the top-left of the Area.
    bool closed = false; // A single contour with at least three distinct vertices.
    bool clipToArea = true; // Plot bounds clip data, while Ruby paths use the View's clip.
};

struct PanelDrawing;

struct CachedDrawCommand {
    Area bounds;
    std::uint64_t revision;
    std::shared_ptr<const std::vector<PanelDrawing>> drawings;
};

struct ViewBlur {
    green::Point size{9, 9}; // Odd kernel sizes in reference pixels.
    green::Point sigma;      // Zero selects the automatic standard deviation.
    std::uint8_t channels = 0xF; // Bits in RGBA order.
};

enum class ShadowMix { Interpolate, Add, Subtract };

struct ViewShadow {
    green::Point distance{0.25F, 0.25F};
    green::Point blur{0.5F, 0.5F};
    float alpha = 1;
    std::optional<Color> color;
    ShadowMix mix = ShadowMix::Interpolate;
    bool shadowOnly = false;
};

struct ViewGlow {
    green::Point blur{1, 1};
    Color color{0xFFFFFFFF};
    float strength = 1;
    bool glowOnly = false;
};

struct ViewBloom {
    green::Point blur{1.5F, 1.5F};
    float threshold = 0.65F;
    float strength = 1;
};

struct ViewShader {
    std::string path;
    float time = 0;
    float amount = 1;
};

using ViewEffect = std::variant<ViewBlur, ViewShadow, ViewGlow, ViewBloom, ViewShader>;

struct EffectDrawCommand {
    Area bounds;
    float viewScale;
    double rotation;
    ViewEffect effect;
    std::shared_ptr<const std::vector<PanelDrawing>> drawings;
};

struct PanelDrawing {
    std::variant<DrawCommand, TextCommand, BitmapDrawCommand, GraphDrawCommand,
                 EditControlCommand, PopupListCommand, DropFilesCommand, MouseAreaCommand, CachedDrawCommand,
                 EffectDrawCommand> command;
    std::vector<std::size_t> order;
    ViewMapping mapping;
};

[[nodiscard]] StringFormat parseStringFormat(std::string_view style, std::string_view horizontal,
                                            std::string_view vertical);
[[nodiscard]] std::string stringFormatStyleName(const StringFormat& format);
[[nodiscard]] std::string_view alignmentName(HorizontalAlignment alignment);
[[nodiscard]] std::string_view alignmentName(VerticalAlignment alignment);

}
