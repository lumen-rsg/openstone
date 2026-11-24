# OpenStone manual

[Project overview and build instructions](../README.md)

This manual describes the current prototype. Compatibility notes distinguish measured behavior from work still awaiting native validation.

- [Editor and library](#editor-and-library)
- [Architecture](#architecture)
- [MIDI devices](#midi-devices)
- [Presets and plug-in host](#presets-and-plug-in-host)
- [Ruby Code](#ruby-code)
- [Headless runtimes, DSP Code, and Assembler](#headless-runtimes)

## Editor and library

The editor opens at **Main**. Click or drag in a breadcrumb’s minimap to reposition that schematic’s viewport. Drag and scroll on the canvas to pan and zoom. With the canvas focused, the arrow keys pan, **+** and **−** zoom, and **0** resets the view.

Use **File → Save** to write an OpenStone schematic (`.ostone`). **Open** restores nodes, wires, nested modules, embedded bitmaps, and DSP code. New, Open, and closing the editor offer to save unsaved edits.

Shift-click nodes or Shift-drag a selection box, then drag any selected node to move the group together. Use the **Edit** menu or the platform Undo and Redo shortcuts with the canvas focused. A drag and any auto-links it creates are one step, as is a Ctrl-drag that cuts several wires. Undo **Make Module** to return the nodes and their original wiring to the parent schematic. Opening a file or starting a new project clears the history.

Copy and paste a selection with the canvas focused or from its context menu. Pasting preserves the node spacing and links within the selection. The new nodes are selected and the paste is one undo step.

Cut stores the selected nodes and their internal links on the clipboard. Undo Cut restores the original nodes in place, including their incoming and outgoing links.

Press **Ctrl+D** (**Command+D** on macOS) or choose **Duplicate** from the context menu. The copies appear slightly down and to the right, keep their spacing and internal wiring, and are selected together. Undo removes the duplicates in one step. The clipboard keeps its previous contents.

Right-click a selection and open **Align and Distribute**. Align creates a row or column through the center of the last selected node. Distribute works with three or more nodes and gives them equal edge-to-edge gaps along the chosen axis. It keeps the outer nodes in place when there is room and expands crowded selections toward the right or down. Each action takes one undo step.

Search the library by name or symbol, and filter by group and connector type. Press Ctrl+F (Command+F on macOS) to focus search. Click an entry to insert it in the center, or drag it onto the canvas. With the canvas focused, **D** places an Int, **Q** a Multiline String, **F** a Float, **S** a String, **B** a Boolean, **T** a Trigger, and **M** a Module. Inside a module, **G**, **I**, and **O** place MGUI, Module Input, and Module Output. Placement uses the pointer position when it is over the canvas. [FlowStone User Guide, pp. 36 and 62](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/downloads/FLowstone_UserGuide.pdf)

Drag from an output to an input, or click the two connectors in order. Hold Ctrl and click or drag across wires to delete them. Right-clicking a wire also offers **Delete Link**. Select nodes and press Delete or Backspace to remove the selection. **Wiring → Straight wires** changes the display and hit testing. **Auto-link nearby ports** previews a connection when an output and a free compatible input are close and aligned. Pause over the alignment or release the dragged node to make the link. [FlowStone User Guide, pp. 49–50](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/downloads/FLowstone_UserGuide.pdf)

Pause over a wire or connector to see its current value and trigger count. A wire shows its value after conversion, while an input shows the combined value from all incoming wires. Trigger-only connections show the pulse count.

Adding a link delivers one trigger along the new wire after it is connected, even when the value at the input stays the same. The destination can then propagate the trigger through its outputs.

Add, Subtract, Multiply, and Divide use template connectors. They infer their types from the wires, and Add also joins strings. Right-click a template connector to pick a type from the connector icons. The gray **Template** icon restores automatic typing. Int division truncates toward zero. A zero divisor returns zero for Int and positive infinity for Float.

**Abs** takes the magnitude of each number, including each coordinate of a Point and each element of a numeric array. **Point** combines Float **X** and **Y** inputs. **Modulus** produces the remainder of Int or Float division. Its sign follows the dividend, and OpenStone returns zero for an Int zero divisor and NaN for a Float zero divisor.

**Less Than** and **Greater Than** have template inputs and a Boolean output. Right-click an input to pin both operands to Int, Float, or String. In Template mode, two Strings compare by text and mixed Int and Float operands compare numerically. New OpenStone nodes use lexicographic array ordering. Imported FlowStone comparisons use the paired-element rules documented in the [importer notes](../docs/compatibility/flowstone-fsm-import.md).

Trigger Blocker exposes the current upstream value without forwarding incoming pulses. Trigger Switch uses a template signal connector and a Boolean **Open** input. The signal value can be read at the output, and its pulses pass through when **Open** is true. Changing **Open** by itself sends no pulse.

**Tick** emits Trigger pulses at the inline **TPS** rate, which defaults to 25. Connect a value to the **TPS** input to drive the rate from the schematic. Fractional rates work, and zero pauses the manual clock. Toggle **Display Sync** to emit one pulse per presented Qt Quick frame from the editor or a visible module window. Toggle it off to resume the saved or wired rate. Manual timing skips missed pulses when the control thread wakes late.

**Module** owns a separate schematic. Add one from the library, then double-click its body or choose **Enter module**. Edit its name and press Return to update the breadcrumb. Click a breadcrumb to return to that level. Each schematic keeps its own viewport. [FlowStone Module guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=58)

Shift-click several nodes or Shift-drag a selection box, then right-click the selection and choose **Make Module**. The shortcut is **Ctrl+M** (**Command+M** on macOS). OpenStone keeps the nodes’ relative positions and their internal wires, and creates one Template port for each wire crossing the new module boundary. The port starts with the wire’s source type and adapts when rewired. Set a type inside the module to pin it. Wireless links continue to use the module hierarchy to find matches. [FlowStone User Guide, p. 65](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=65)

To build an interface by hand, enter a module and place **Module Input** or **Module Output** from the library. Edit the name and press Return, then use the type button or right-click the endpoint's connector. The matching connector appears on the enclosing module. Endpoints are ordered top to bottom, then left to right for ties; moving an endpoint keeps its links attached. Changing the type keeps compatible links, and deleting an endpoint removes its connector and attached links.

The module port picker includes the FlowStone Alpha connectors from Mem and MemArray through Preset. MIDI, Notes, Voices, and Preset carry typed Green values and use Green trigger delivery. MIDI and Notes inputs accept multiple sources and read the source that fired. The editor records matching Mem and Stream family links for DSP graph compilation.

Right-click a node to cut, copy, paste, rename, delete, or disconnect its wires. A Module’s menu also lets you enter it and show or hide its front panel. Right-click open canvas space to paste there.

To create a synchronized module, copy a Module and choose **Paste Synchronize**. Changes inside any instance propagate to the others, including changes to nested schematics. Each instance has its own placement, viewport, and live trigger state. **Synchronize All** matches identical module definitions anywhere in the project. **Synchronize Painter** lets you choose matches one at a time and stays active as you navigate. Press Esc to exit it. Select a synchronized Module and use Tab or Shift+Tab to cycle through its copies. Use **Unsynchronize** to detach one copy or **Unsynchronize All** to dissolve the group. [FlowStone User Guide, pp. 84–86](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=84)

**MGUI** can be placed inside a module. Adding it shows a front panel on the Module node in its parent schematic. **Hide panel** collapses the node without changing the panel layout, and **Show panel** restores it.

Select a module to show its action strip. **P** enables or opens its properties panel and **G** adds its front panel. The minimize and maximize controls change the module’s display on the schematic. The eye controls its visibility on an enclosing panel. **Add to Toolbox** saves a reusable module in the application’s toolbox and lets you enter a group. Right-click a saved module in the Library to rename, move, or remove it. **Focus mode** opens the front panel in its own window and hides the editor. Press Escape or close the focused window to return. [FlowStone Module guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=58) [FlowStone Focus Mode](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=87)

Use the padlock on a selected module to edit its panel. Drag the corner grip to resize it from its initial 16 × 10 grid units. To show a child panel on an enclosing panel, wire the enclosing MGUI’s **View** output to a **Wireless Output**, then wire a matching **Wireless Input** to the child MGUI’s **View** input. Click a dashed child outline to show its anchors. One selected edge keeps that margin during a resize. Selecting both opposite edges stretches the child along that axis. Child drags snap to the grid. Hold Ctrl while dragging for pixel steps at the current zoom. Panel positions are independent of schematic positions. The first MGUI in a module supplies the front panel shown on the Module node. [FlowStone front panel guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=68)

MGUI provides a yellow **View** output representing the drawing surface. Its **View** input accepts a parent panel. Its Green **Mouse** and **Border** inputs control hover reporting and the panel’s editor border, respectively.

**Module Properties GUI** (**Props**) gives a module a second, editor-only panel. The first Props in the module supplies the panel. Click **P** on the Module node to open it in place. Resize it with its own corner grip. Clicking away closes it. Use **Pin properties** in the module menu to keep it open. [FlowStone User Guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=65)

Wire the **Props** View output to drawing or input primitives to build the panel. To place a control module on it, send that View through a **Wireless Output** named **Properties** and connect the matching **Wireless Input** to the control’s MGUI or Props View input. Drag the control on the panel to position it. The **Mouse** input enables hover movement events on the properties View. [FlowStone properties panel guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=82) [FlowStone Module Properties GUI reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=280)

Wire MGUI’s **View** output to **Arc**, **Ellipse**, or **Line** and supply an **Area** and a **Paint Tool**. Chain the **View** output to another drawing primitive to paint it on top. Arc uses the Area as elliptical bounds and takes **StartA** and **SweepA** in degrees, starting at 9 o’clock and moving clockwise for positive values. Ellipse uses a **Pen** for the outline or a **Solid** brush for the fill. Line uses a **Pen** and runs from `(x, y)` to `(x + width, y + height)`, so signed extents control the direction. [FlowStone drawing guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=124)

**Pie** uses the same angle convention as Arc and closes the sector at its center. **Rect** takes **Corner** as a radius in grid units. Use zero for square corners. Pie and Rect accept a **Pen** for outlines or a **Solid** brush for fills. **Text** draws a String inside its Area using **Color**, **Font**, and **SF** (String Format).

**GraphDots** and **LineGraph** plot a Float Array on a View. **Top Y** and **Bottom Y** set the displayed range (default 1 and −1). Samples span the Area in order, with even spacing unless **Log X** is enabled. **GraphDots** uses a Solid brush for filled dots or a Pen for outlines, with **Radius** in grid units. **LineGraph** uses a Pen and connects consecutive samples with straight segments, or a smooth curve if **Curve** is enabled. Non-finite samples are gaps, and drawing is clipped to the Area. [FlowStone Graph Dots and Graph Lines reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=170)

**Dots** plots a Point Array using the X and Y in each point. **Left X** and **Right X** default to −1 and 1. **Top Y** and **Bottom Y** default to 1 and −1. The dots are clipped to the **Area**. Use a Solid brush for filled dots or a Pen for outlines. **Radius** is in grid units and defaults to 0.15.

**Draw Point Lines** (PLines) uses the same Point Array and axis bounds as Dots. It connects points in array order with a Pen and clips the stroke to the **Area**. Enable **Curve** for a smooth path through the points. Non-finite coordinates break the path. [FlowStone Point Array Lines reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=380)

**Draw Polygon** uses a Point Array and the same four axis bounds as Draw Point Lines. It closes the boundary automatically and clips it to the **Area**. Connect a Pen for an outline or a Solid brush for an even-odd fill. **Curve** smooths the entire boundary, including the closing edge. A polygon needs at least three distinct vertices with finite coordinates.

**DropList** opens a menu on a visible View when **Open** receives a trigger. **List** is a comma-separated String. **X** and **Y** place the top-left corner in View grid units. **Check** marks a zero-based option index, and **Style** accepts `AutoCol` (the default), `Scroll`, or a positive number of rows per column. Choosing an option updates **Selected String** and **Selected Index** together. Escape or an outside click pulses **Canceled** and keeps the last selection. **Number of Options** follows the current List. [FlowStone Drop List Control reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=118)

Use `>>, Menu name, <<` to start a submenu, `>>` to end it, and `-----` to insert a separator. For example, `Open, >>, Export, <<, PNG, JPEG, >>, -----, Close` puts PNG and JPEG under Export. DropList trims whitespace around each entry and assigns indices to selectable items in the order they appear in **List**, including those inside submenus. **Check**, **Selected Index**, and **Number of Options** use that same sequence.

**Edit Control** places an editable field in an **Area** on a **View**. **Font** supplies the typeface, **Text** sets the text color, and **Back** sets the background while editing. **Set** replaces the text. Click the field or fire **Start** to focus it. **Is Editing** follows focus, and **Current Text** is readable as you type. Enable **Trig** to pulse **Current Text** once when a changed edit finishes. Tab, Escape, and moving focus away finish an edit. Return finishes a single-line edit and inserts a newline when **Multi** is enabled. **Pwd** masks the field and uses single-line input. [FlowStone Edit Control reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=121)

**Popup List** displays the selected option in an **Area** on a **View**. Press and hold the control, drag to highlight an option, and release to select it. Tab to the control and press Space or Return to open it from the keyboard. **List** accepts comma-separated options, **Rows** caps the visible rows (eight by default), and **Set** selects an item by its zero-based index. Invalid **Set** values leave the selection as it is. An empty list outputs an empty String and index `-1`. **Font** supplies the typeface, **Text** sets the text color, and **Back** sets the open list background. [FlowStone Popup List Control reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=386)

**Drop Files** accepts paths dragged from the system file manager into an **Area** on a **View**. **Ext** is a case-insensitive comma-separated list of file extensions. An empty **Ext** accepts any file extension, and **Dir** allows directories. **Max** defaults to one and limits the whole batch. During an accepted drag, **Paths**, **Mouse X**, and **Mouse Y** report the preview in View grid units. Leaving restores the previous drop. Releasing an accepted batch updates those outputs, clears **Dragging Over**, and pulses **File(s) Dropped**.

**Wireless Output** sends to a **Wireless Input** in a descendant module with the same name and connector type. Enter a name in either node and press Return. **WL-Link** sends with a String **Name** input, and **Wireless** receives with a String **Name** input. A name field and a Name input can match each other. A matching sender in a nearer ancestor takes precedence. [FlowStone Wireless Links guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=53)

**Float to Area (F2A)** combines the Float inputs **X**, **Y**, **Width**, and **Height** into a rectangle in a View's local grid units. An Area input can also accept a String of four comma-separated numbers. Area values can travel through Trigger Blocker and Trigger Switch. View and Area take one value source per input and share the Green runtime's trigger delivery. [FlowStone User Guide, pp. 120–124](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=120)

**Area to Float (A2F)** splits those four values. **Area From Points (P2A)** accepts two corners in either order, and **Area to Points (A2P)** outputs the top-left and bottom-right corners. **Area Center Point** returns the midpoint. **Area Around Center Point (C2A)** centers an Area with the supplied width and height. Connect a single dimension for a square.

**Align Area** positions Src relative to Ref using horizontal and vertical alignment strings such as `left`, `center`, `right`, `top`, and `bottom`. **Area Union** returns a bounding rectangle around its inputs. **Point Inside Area** includes the rectangle's edges. **Point Inside Polygon** closes a Point Array automatically and uses the even-odd crossing rule.

**Mouse Messages** carry an event kind and a Point in local grid units. Button events and drags carry a button. Wheel events carry signed horizontal and vertical rotation in eighths of a degree. The shared runtime stores a value separately from firing a trigger, so identical consecutive events can each deliver a trigger. Trigger Blocker and Trigger Switch support the yellow **M** connector. Wire the Mouse Messages output of a mouse area to **Mouse LDbl**, **Mouse LDown**, **Mouse LUp**, **Mouse RDbl**, **Mouse RDown**, **Mouse RUp** or **Mouse Wheel**. The button primitives emit the position in the View's grid units. Mouse Wheel emits the signed vertical angle delta as a Float, with 120 units per standard wheel step. **Mouse Move** connects directly to a View. Enable the MGUI Mouse input to receive pointer movement.

**Rectangular Mouse Area** takes a **View** and **Area**. **Polygonal Mouse Area** takes a **View** and a Point Array on **Pts**. Its points are vertices in View grid units. The polygon closes automatically and accepts pointer events inside its even-odd fill and along its edges. Both primitives output **Mouse Messages**. For drawing on the same panel, branch the View to the drawing primitive as well as the mouse area.

**Mouse Info** attaches to the **Mouse Messages** output of a Mouse Area and configures the cursor and tooltip for that region. **Cursor** accepts a name such as `pointer`, `ibeam`, or `handopen`, or an ID from 0 through 23. An empty or unknown cursor leaves the inherited cursor in place. Qt supplies the nearest native shape for FlowStone-specific cursor variants. **Tip** shows after the pointer rests over the region. An empty Tip disables the tooltip. Cursor and tooltip work with the MGUI Mouse input off.

**Show Cursor** controls the pointer across OpenStone's windows. **Show** defaults to true. If **Hold** is true when **Show** turns false, OpenStone remembers the screen position and restores it when **Show** turns true on platforms that support pointer repositioning. Mouse events can still travel through the schematic while the pointer is hidden. The desktop releases the hidden cursor when OpenStone loses focus.

**Mouse Drag** takes a Mouse Area's **Mouse Messages** and tracks a left-button gesture, including moves and release outside the area. **Mouse Down**, **Position**, and **Mouse Up** report Points in View grid units. **Dragging** goes true at press and false at release or cancellation. **Cursor** uses the Mouse Info names and IDs during the drag. **Reset** returns the system pointer to its original screen position after a release on platforms that support pointer repositioning.

**Mouse Drag Accumulate** uses the same capture controls and reports an accumulator Point clamped to `0–1` on each axis. **Scale** defaults to `(0.05, 0.05)` per View grid unit. Negate either axis to invert it. The value is calculated from the start of the gesture. A Scale change during capture starts a new baseline at the current value. **Set** changes the current accumulator silently while idle. A hidden-cursor drag can recenter at the window edge on platforms that support pointer repositioning.

**Drag Files** starts a native file drag when a Mouse Area sends a left-button press. **Files** takes a String Array of existing file paths. You can connect a String containing one path per line or use the **Paths** output of **Drop Files**. **Mode** offers one action to the destination, with `copy` as the default and `move` and `link` also available. **Dragging** tracks the native drag. **Dropped Successfully** reports whether the destination accepted the action.

**Window** provides a View in its own native window and can be placed at any schematic level. Connect a Boolean to **Open**, then connect the Window's View output to drawing primitives. **Width (px)** and **Height (px)** set the client size in logical pixels and default to 320 × 200. **Type** selects a fixed-size window (0), a resizable window (1), or an independent application window (2). **Title** updates the window title. The **Open** output reports whether the window is shown and goes false when the user closes it. Send a new true input event to reopen it. A native resize updates the View at eight logical pixels per grid unit. **Set GUI Area** can also resize the window's client area.

**View Section** offsets the drawing origin (X and Y) and clips to the requested W and H, in View grid units. An unwired W or H inherits that dimension. **View Scale** changes the size of one grid unit by Ratio and reports the corresponding local width and height. **View Size** returns those two values, and **View Area** returns the same extent as an Area starting at (0, 0).

**View Rotate** rotates the downstream View around **X** and **Y** in incoming View grid units. **Angle** is in degrees clockwise. The View size stays the same, and pointer positions are reported in the rotated View's coordinates. **View Smooth** passes the View through. Its **AntiAlias** and **ClearType** Boolean inputs are accepted for compatibility.

**Clip** combines an Area with the current clipping region. Mode 0 intersects, 1 unites, 2 keeps the new Area outside the current region, 3 removes the new Area from the current region, and 4 keeps the nonoverlapping parts of both. The clip belongs to the downstream View branch. Hardware rendering follows rotated clip geometry. The software renderer uses its bounding rectangle.

**GUI Area** reports the actual module panel's position and size. **Set GUI Area** applies the supplied Area to that panel when **Set** fires. X and Y position an embedded panel in its parent, and W and H resize the panel.

**View Loop** draws the branch once for each value from Start up to End, with End excluded. Step defaults to 1. A negative Step walks down from Start. Start, Step, End, and Value share an inferred or selected Int or Float type. Iteration starts at 0.

**View Array Loop** draws once for each item in an Int, Float, String, or Point Array. Rev walks the array backward and Index remains the item's original zero-based index. Both loops publish Step before drawing each iteration and Finished when the pass ends. NoLim uses the longer drawing time limit. Wire ordinary drawing primitives to the loop's View output.

**View Cache** captures the drawing branch connected to its View output into a transparent texture. The first draw builds the texture and unrelated panel redraws reuse it. Send a trigger to **Clear** to rebuild the branch using current values on the next draw. Changes to the View geometry or zoom and edits to schematic wiring also refresh the cache.

View effects capture the drawing connected to their **View** output. For example, wire MGUI → View Shadow → Rect to shadow the rectangle while leaving sibling branches independent. Effects can be chained and use the incoming View's bounds and clip. The desktop compositor runs them on the GPU. The software renderer shows the underlying drawing.

**View Blur** uses a separate Gaussian kernel for each axis. **Size** is a Point with odd kernel sizes from 1 to 65, with even sizes rounded up and a default of `(9, 9)`. **Sigma** specifies the two standard deviations. Zero derives the deviation from the kernel size. Size and Sigma use eight reference pixels per grid unit. **Chans** selects channel letters such as `A` or `RGB`, with `ARGB` as the default. [FlowStone View grid units](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=170)

**View Shadow** takes a **Distance** Point for the XY offset, a **Blur** Point for the two radii, and **Alpha** from 0 to 1. Distance and Blur use View grid units. The shadow uses the source color until a Color is connected. **ColMix** accepts `interpolate`, `add`, or `sub` and uses the Color's alpha to control the tint. **ShOnly** draws just the shadow.

**View Glow** draws a colored halo using the source alpha, with per-axis **Blur** in grid units, a **Color**, **Strength**, and **Glow Only**. **View Bloom** extracts highlights above a **Threshold** from 0 to 1, blurs them, and adds them back with the given **Strength**. Bloom also takes its **Blur** in grid units.

**View Shader** takes the path to a local Qt `.qsb` fragment shader package on **Shader**. Use a String box for the path. **Time** in seconds and **Amount** are graph-driven Float uniforms, defaulting to 0 and 1. A shader can use the captured `source` sampler, the texture size `resolution` in pixels, and the `time` and `amount` uniforms. The source passes through when Shader is empty or the file is missing, corrupt, or a different shader stage. [Qt ShaderEffect reference](https://doc.qt.io/qt-6/qml-qtquick-shadereffect.html)

To try View Shader, save this GLSL as `invert.frag`, then run `qsb --glsl "100 es,120,150" --hlsl 50 --msl 12 -o invert.frag.qsb invert.frag` with Qt Shader Tools. Wire the resulting file path to **Shader**. [Qt QSB Manual](https://doc.qt.io/qt-6/qtshadertools-qsb.html)

```glsl
#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Parameters {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 resolution;
    float time;
    float amount;
};
layout(binding = 1) uniform sampler2D source;
void main() {
    vec4 pixel = texture(source, qt_TexCoord0);
    fragColor = vec4(mix(pixel.rgb, vec3(pixel.a) - pixel.rgb, amount), pixel.a) * qt_Opacity;
}
```

**Redraw** passes the View through and requests a repaint of its MGUI panel when **DoIt** fires. **Redraw Area** repaints the supplied Area in the incoming View's coordinates. Wiring either primitive to a View gives that panel explicit redraw control. Draw requests passing through View Cache refresh that cache on the repaint. [FlowStone User Guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=131)

**Color** stores an unsigned 32-bit `0xAARRGGBB` value. Alpha ranges from 0 (transparent) to 255 (opaque). Click a Color node's swatch to adjust hue, saturation, brightness, or alpha, or enter `#AARRGGBB` directly. Apply sends a trigger when the color changes. A connected Color input supplies the swatch and output. Hover over a Color connector or wire to see the swatch and channel values.

**Color to ARGB** and **ARGB to Color** split and assemble four integer channels. The constructor uses the low byte of each input. **Color to AHSV** and **AHSV to Color** carry Alpha, Hue in degrees, Saturation, and Value. Hue wraps around the color wheel, and Saturation and Value use the 0–255 scale. The splitter rounds HSV channels to integers, and the constructor truncates the calculated RGB channels. [FlowStone Component Reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=85)

**Color Matrix** carries 25 floating-point coefficients. **Color Matrix Set** copies its input matrix and updates the selected row and column from 0 through 4. It starts with the identity matrix and a Value input of 1. An invalid index or a non-finite Value leaves the input matrix intact. **Apply Color Matrix** lets you preview the transform with a Color swatch before using a bitmap. It multiplies the normalized row vector `[R, G, B, A, 1]` by the matrix, clamps the four output channels, and rounds each to the nearest byte. Row 4 adds offsets to red, green, blue, and alpha. The fifth column is stored, and the four Color channels come from the first four columns. [Microsoft color matrix reference](https://learn.microsoft.com/en-us/windows/desktop/gdiplus/-gdiplus-using-a-color-matrix-to-transform-a-single-color-use)

**Paint Tool** carries a **Pen** stroke or a brush fill. Pen takes a Color, a Float width in grid units, and a String style. It accepts `solid`, `dash`, `dot`, `dashdot`, and `dashdotdot`, ignoring case and surrounding whitespace. An empty or unrecognized style gives a solid stroke. Pen defaults to 0.1 grid units, uses that width for negative or non-finite inputs, and treats zero as a hairline. Solid takes a Color and preserves its alpha. Hover over a Paint Tool connector or wire to preview it. [FlowStone Component Reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=331)

**Linear Gradient Brush** blends **Col1** into **Col2** across an **Area** in View grid units. At 0 degrees the colors run left to right. The **Angle** increases clockwise. **Radial Gradient Brush** blends **Col1** at the center into **Col2** at the boundary. **Shape** accepts `ellipse` or `rect`. Wire **Ctr X** or **Ctr Y** to position the center in the View, or leave both disconnected to use the center of the Area. **H-Focus** and **V-Focus** expand the region of center color on each axis from 0 to 1. [FlowStone User Guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=173)

**Hatch Brush** repeats a pattern with foreground and background Colors. **Style** takes a name such as `horizontal`, `diagonalcross`, or `largecheckerboard`, or a GDI+ hatch ID from 0 to 52. **Texture Brush** fills with the pixels from a Bitmap and preserves its alpha. Linear Gradient Brush and Texture Brush accept `tile` (default), `flipx`, `flipy`, `flipxy`, or `clamp` on **Wrap**. Connect any of these brushes to the **Tool** input of a filled drawing primitive. [GDI+ hatch styles](https://learn.microsoft.com/en-us/windows/win32/api/gdiplusenums/ne-gdiplusenums-hatchstyle)

**Font** carries the requested Face, a Float size in grid units, style flags, and an optional alternate face (**AltF**). Font defaults to Arial, 1 grid unit, and normal style. Style strings can combine `bold`, `italic`, `underline`, and `strike` in any order. **FontAttr** exposes the requested Face, Size, and Style as three outputs. Hover over a Font connector or link to preview the font. The preview uses the alternate face if the requested face is unavailable and the system font if neither is installed. [FlowStone Font reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=162)

**String Format** combines a Style string with horizontal (`left`, `center`, `right`) and vertical (`top`, `center`, `bottom`) alignment. Style accepts `normal` or combinations of `righttoleft`, `nowrap`, and `vertical`. The default is top-left alignment with wrapping and horizontal, left-to-right text. **StrFmtAttr** outputs Style, Horz, and Vert as strings. Hover over a String Format connector or link to inspect the active settings. [FlowStone String Format reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=468)

**Measure Text** returns the Area occupied by text inside **MaxArea**, in grid units. It uses the **Font** and **SF** settings and the same layout as **Text**, including wrapping, alignment, and vertical text. The result includes its position within MaxArea. Empty text or an unwired MaxArea gives an empty Area. Qt and the installed fonts determine the measured dimensions. [FlowStone Measure Text reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=247)

**Bitmap** stores decoded RGBA pixels with straight alpha. Choose **Open image…**, or connect a String file path and pulse **Load**. The Bitmap, Width, and Height outputs update together. A failed load keeps the last image, and **Clear** resets the image and both dimensions. The original file is only needed when loading. Hover over a Bitmap connector or link for a thumbnail and pixel dimensions. The desktop loader uses Qt's installed image decoders. [FlowStone Bitmap reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=51)

**SVG** loads the local file at **Path** when **Load** fires and renders it to a transparent Bitmap. Unwired **Width (px)** and **Height (px)** use the SVG's natural size. Wired dimensions are at least one pixel and round up for the Bitmap. Each axis accepts `fit` to fill the requested extent, `aspect` to follow the opposite axis's scale, or `center` to keep the native scale. If both axes use `aspect`, the artwork keeps its native scale and is centered. Size and mode changes redraw the loaded document. **Success** fires after a load publishes the Bitmap. **Error** reports failed loads or renders and leaves the last successful image available. [Qt SVG renderer](https://doc.qt.io/qt-6/qsvgrenderer.html)

**Bitmap Area** draws a crop of a Bitmap: **Src** is in image pixels from the top left, and **Dest** is in View grid units. The optional **Matrix** changes the crop’s colors. [FlowStone Bitmap Area reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=53)

**Bitmap Draw** stretches the whole image to fit its **Area**. **Alpha** goes from 0 (invisible) to 255 (full image opacity, the default); the bitmap keeps its own per-pixel transparency. **Bitmap Draw Transform** also draws the whole image, with an optional **CM** (Color Matrix). Chain their **View** ports with other drawing nodes to set the paint order. [FlowStone Bitmap Draw reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=59)

**Bitmap Array from File** loads a Bitmap at a zero-based **Index**. Connect a String to **Path** and pulse **Load**, or choose **Open image…** to load a file at the current index. Loading into a populated slot replaces that image. Skipped slots remain empty. A successful load updates the array before firing **Load complete**. A failed load leaves the array intact, and **Clear** empties it. The hover readout shows populated indices and pixel dimensions. [FlowStone Bitmap Array reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=57)

**Bitmap Size** reports Width and Height as Ints. **Bitmap Bounds** returns an Area at the pixel origin `(0, 0)` with those dimensions. **Get Pixel** reads straight ARGB at zero-based **X** and **Y** when **Get** fires. Coordinates outside the image return transparent black. [FlowStone Get Pixel reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=167)

**Bitmap Resize** applies a positive **Scale** when **Resize** fires and publishes the new bitmap before **Resized**. **Bitmap Sample and Hold** retains the image from the most recent **Sample** pulse. **Bitmap Array from Bitmap** stores the current image at **Index** on **Set**, replaces occupied slots, and empties the array on **Clear**. The two array builders and Bitmap Sample and Hold expose **Enc** for compatibility with schematic persistence. OpenStone currently keeps these images in memory. [FlowStone bitmap reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=55)

**Bitmap Get At** copies the image at a zero-based **Index** when **Get** fires. A gap or out-of-range index publishes an empty Bitmap. **Done** fires after either result. [FlowStone Bitmap Get At reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=61)

**Set Pixel** writes straight ARGB from **Color** into the connected Bitmap at zero-based **X** and **Y** when **Set** fires. Other links carrying that image can read the edit immediately. A View shows it on its next repaint. Coordinates outside the image and an unwired Color are ignored. [FlowStone Set Pixel reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=421)

**Bitmap Save** writes the image when **Save** fires and emits **Saved** after the file is committed. Provide a destination **Path** and optionally an explicit **Enc** format. Blank **Enc** uses the file extension, or PNG when the path has no extension. Available formats depend on the installed Qt image writers. [FlowStone Bitmap Save reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=64)

**Create Bitmap** paints the drawing chain connected to its **View** output into a transparent image. Pulse **Create** to use the current **W** and **H** in pixels, then pulse **Redraw** to repaint that size. **Draw to Bitmap** paints its View chain over a copy of the source image each time **Draw** fires. On both nodes **Step** is pixels per View grid unit and defaults to 8. [FlowStone Bitmap Create reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=58) [FlowStone Draw to Bitmap reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=117)

**Bus Create** and **Bus Extract** group named channels onto one neutral Bus wire. Connect a String of comma-separated names to **Names** on each node. Create adds an input for each name. Extract adds outputs in the requested order and matches the names exactly, including case. Blank names are skipped and duplicate names use their first occurrence. Reordering either list keeps existing wires with their names. The Bus carries the current Green and Yellow connector types and Trigger channels. Each channel keeps its own value and pulses. Hover over the Bus wire to inspect its channels. [FlowStone Bus reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=72)

## Architecture

- `src/editor` is a Qt-free C++23 library with schematic editing, Green primitive definitions, and viewport commands.
- `src/green` is a Qt-free C++23 runtime that delivers Green and Yellow values and triggers.
- `src/stream` is a Qt-free C++23 DSP runtime for Mono, Mono4, MonoDouble, MonoDouble4, and Poly with a compiled sample schedule.
- `src/yellow` defines the rendering-independent View, Area, Mouse Messages, Color, Color Matrix, Paint Tool, Font, String Format, Bitmap, and Bitmap Array values.
- `src/desktop` adapts the session and schematic to QML. Qt Quick renders the grid, nodes, and wires.

The status bar shows the active Qt graphics API and the number of trigger emissions in the current schematic.

## MIDI Devices

Open **Processing → MIDI** and use the device picker on **MIDI In** or **MIDI Out** to select one or more ports. Each port belongs to one node in its direction. The schematic saves the picker's port identities and reconnects available ports when reopened.

**MIDI In Devices** and **MIDI Out Devices** expose the count, names, and availability in enumeration order. Wire a zero-based index to **MIDI In Select** or **MIDI Out Select** to pick a port in the graph. An index of `-1` disconnects the node. A selected port retains its identity if enumeration order changes.

The desktop RtMidi adapter parses device input and queues complete MIDI 1.0 messages. The editor dispatches the messages as Green triggers on its control thread. Host and plug-in adapters can use their own device transport through the Qt-free `MidiDeviceBackend` interface.

### MIDI stream controls

Wire a MIDI source to **MIDI Control Change (Stream)**, **MIDI Aftertouch (Stream)**, or **MIDI Pitch Bend (Stream)** in the MIDI category. **Aftertouch** and **Pitch Bend** hold their latest matching MIDI 1.0 values on Mono outputs. Use **MIDI Channel Filter** upstream to select a channel.

- **Control Change** outputs Poly. Each voice uses the chosen controller's latest value on its own MIDI channel, mapped from 0–127 to Min–Max. A channel uses Default until a matching message arrives. OpenStone retains the Mono output on nodes saved with the older implementation.
- **Aftertouch** maps MIDI channel pressure from 0–127 to 0–1.
- **Pitch Bend** maps the 14-bit value to −1 through +1, with MIDI center at 0.

The stream reads the held value at audio block boundaries.

### Notes and voices

Wire **MIDI In → Midi to Notes → Notes to Voices**. Connect **Voices** to the other input of **Notes to Voices**, then connect its output to **Voices to Poly**. Its Freq, Pitch, Vel, Gate and Trig outputs can drive a Poly DSP block. Use **Poly to Mono** to mix that block into **Audio Out**. To keep release tails, wire the envelope's Poly Boolean completion signal to **Done**. Gate drops on release and Done retires the voice after its final sample.

The Notes filters take inclusive Low/High arrays for pitch, normalized velocity or zero-based MIDI channel. Overlapping layers can duplicate and tag a note. Its release follows the same routes as its attack. **Voices** defaults to 32 slots and supports up to 256, with controls for retaining and retriggering stolen notes. The audio thread reads voice state at block boundaries.

The Poly ID and Tag primitives expose per-voice metadata. The Min and Max mixers reduce active voices. **Poly to Mono Hop** holds the sum between updates and uses a power-of-two interval up to 4096 samples, rounding down. **Poly to Mono Selective** selects a zero-based group of four voice slots. The Voice Index on **Poly to Mono** selects one slot.

**VST-M In** and **VST-M Out** use the Qt-free schematic host interface. An adapter supplies complete MIDI 1.0 messages through `receiveHostMidi` and receives them through `setHostMidiOutput`, on the schematic control thread. These ports are available for integration with a future plug-in export target.

## Presets and plug-in host

In **Audio → Presets & Plug-in Host**, wire the **Preset Manager** output to the **Preset Bus** inputs of **Preset Float**, **Preset Bool**, **Preset String**, and **Preset Array**. This connector can also travel through a module boundary or a named Wireless Link. Connect an Int to **NPrgs** to set the program count and an Int to **Prog** to select a zero-based program. **Names** accepts a comma- or newline-separated list. The manager's **After** trigger fires after its linked parameters recall their values.

Unlocked programs store parameter edits as they occur. Enable **Lock** to discard unsaved edits when switching away and pulse **Save** to keep the current edits. **Clear** and **Clear All** restore parameter defaults for the current program or every program. Schematic files preserve program names, the selected program, and the parameters' working and stored values.

The Qt-free host interface lists automatable Float, Bool and Array parameters with `pluginParameters()`, sorted by **Order**. Hosts apply normalized values with `receivePluginParameter()`. Local changes can return through the recording callback. **Delay Compensation** reports nonnegative latency in samples. **VST Editor Open** and **VST Host Info** read the state supplied by the host. All of these calls use the schematic control thread. An exported plug-in adapter is a separate integration.

Connect the Preset Manager output to the **Manager** input of **Preset To Text** or **Preset From Text**. These nodes transfer all configured programs by default. Enable **Curr** to export the active program or load the first column into the active slot. Parameters match by unique name, including ones connected through a module or Wireless Link. Loading skips unknown names, leaves omitted parameters alone, and uses the manager's existing program count. Use the String output with **Text Save** or feed **Text Load** into the import node for file access.

Ordinary text uses the tab-delimited Preset Table Format. Tables that contain embedded tabs or newlines use an OpenStone header and escape those characters. The reader also accepts the legacy **Order** column, with parameter order continuing to come from the schematic. **Preset Show Order** lists parameters in **Order** order on Get. **Grouped** lists automatable parameters first.

**Preset Bypass** appears to the host as a Boolean parameter marked for bypass. It stays independent of program recall. Wire its Boolean output into the schematic's audio routing to select the bypass path.

**Preset Save** and **Preset Load** use the same Manager connection. Wire a file path to **Filename** and pulse **Save** or **Load**. Save writes an FXB bank by default and an FXP program with **Curr** enabled. Load restores a bank into the configured slots. Loading an FXP, or enabling **Curr** on an FXB load, imports a single program into the selected slot. Load reports the file's version and active program and can check a four-character **Fx ID** and a version range.

Standard parameter files use normalized values in preset **Order**. Banks that contain text, preset-only values, or varying array lengths use an OpenStone state chunk and match parameters by name. Opaque chunks from other plug-ins and FlowStone's legacy load options are currently unsupported.

Place **VST Plugin Info** and **VST3 Aux Channels** directly inside the module you want to export. A future export adapter can read the metadata via `pluginExportInfo(moduleId)`. Specify how many of the final Mono inputs and outputs are auxiliary with **Ins** and **Outs**. The host reads the layout through `pluginAudioLayout(moduleId)` and publishes activation flags with `setPluginAuxStates()`. The primitive emits each changed list with `1` for an active channel and `0` for an inactive channel.

**Is Playing**, **Offline Mode**, **Tempo**, and **Time Signature** in **Presets & Plug-in Host** report Green transport data. Publish a `stream::HostTransport` snapshot through `setPluginHostTransport()` on the schematic control thread. Changed outlets send a pulse, and **Stop** fires on a playing-to-stopped transition. Pass `{}` to restore standalone defaults of stopped playback, realtime processing, 120 BPM, and 4/4. Stream transport nodes continue to use the snapshot passed to the audio callback.

The host compiles a selected module with `preparePluginAudio(moduleId, blockSize)` and renders its Mono outputs through `processHost()`. Mono module inputs map to host channels in module port order. **VST Input** can tap a host channel by zero-based index and reads as silence in desktop audio. The host supplies an empty buffer for each inactive channel.

## Ruby Code

Find **Ruby** in the library. The starter node doubles a Float from its input. Connect a Float and a readout, click **Run Ruby**, then change the input to see the output. The node shows the last expression, printed text, watch values, and Ruby errors. **Stop** pauses event delivery. The desktop app starts a shared CRuby process when the first node runs. It looks for Ruby on the system path, or at the path in `OPENSTONE_RUBY_EXECUTABLE`.

Drag the grip below either connector column down to add ports or up to remove ports. Click a name to edit it. Right-click a connector to change its type, insert or remove a port, or move it up or down. Drag the bottom-right grip to resize the Ruby box. Visual port edits and box size are saved with the schematic and are undoable. A connector layout edit stops the script. Click **Run Ruby** to start it with the new layout.

You can also declare ports with comments above the source. **Apply Ports** configures those connectors from the header. **Use Header Ports** switches a visually configured node back to the comment layout. Supported types include Trigger, Float, Int, Boolean, String, Int Array, Float Array, String Array, MIDI, Ruby Value, Color, and Bitmap. View is available as an input.

```ruby
# input Float value
# input Trigger send
# output Float result

def event(i, v, t)
  output "result", @value * 2 if i == "send" && @value
end
```

Ruby uses an independent `RubyEdit` instance per node. `event(i, v, t)` receives the connector, value, and elapsed schematic time in seconds. A Trigger has a Ruby value of `nil`. Read inputs through `@ins`, `@in`, or a named variable such as `@value`. Send values with `output value` or `output connector, value`. Use `watch label, value` to show intermediate values. OpenStone calls `init` after evaluating the source and before restoring saved state. Schematics save the source and applied connector layout. Click **Run Ruby** to start the node after reopening. [FlowStone Ruby component guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=133)

Declare a `# output RubyValue packet` on the sender and a `# input RubyValue packet` on the receiver. Connect the ports and run both nodes. When the sender calls `output "packet", object`, the receiver gets the live Ruby object in its event handler. Arrays, hashes, and instances of custom classes keep their identity, so both scripts see mutations. Saving preserves Ruby Value wiring. The objects live in the running interpreter and are recreated by the scripts after reopening.

### Ruby event scheduling

Use `output connector, value, timestamp` to send a value later or `input connector, value, timestamp` to post an event to an input on the same node. Use a connector name or a zero-based index. `scheduleMethod name, *args, timestamp` queues a method call. `time` and the third argument to `event` use seconds since the schematic opened. For example, `output "result", 42, time + 0.5` schedules delivery half a second later. Output calls without a timestamp still send immediately.

Equal timestamps deliver in submission order across the schematic. A callback receives its scheduled time even when the editor wakes late. `clearEvents` removes the calling node's pending inputs, outputs, and method calls. Events queued after that call still run. Stopping or replacing a Ruby script clears its pending events.

OpenStone delivers scheduled Ruby events on the editor's timer. FlowStone also supports sample-synchronous delivery with its audio clock. [FlowStone scheduling guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=146)

### Ruby state

Define `saveState` to return the data your node needs to keep and `loadState(value)` to restore it. Saving a schematic calls the hook even if the node was stopped after running. Opening reads the saved state, and **Run Ruby** evaluates the script and then calls `loadState` with that value. Running the node again starts from its last saved state.

~~~ruby
def saveState
  [@count, @label]
end

def loadState(state)
  @count, @label = state
end
~~~

State uses Ruby Marshal and supports scalars, arrays, hashes, shared references, and compatible named classes. Define custom classes before the hook runs. [FlowStone state management guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=152) [Ruby Marshal reference](https://docs.ruby-lang.org/en/3.3/Marshal.html)

### Drawing from Ruby

Add a `View` input to a Ruby node and connect a View from **Module GUI**. After **Run Ruby**, the panel calls `draw(view)` when it paints. Use `draw(connector, view)` to distinguish multiple View inputs by name or index. Shape bounds use `[x, y, width, height]` in local grid units. A `Brush` fills the shape and a `Pen` outlines it.

~~~ruby
# input View canvas

def draw(view)
  view.drawRectangle(Brush.new(Color.new(155, 76, 174)), [1, 1, 6, 4])
  view.drawEllipse(Pen.new(Color.new(32, 40, 52), 0.2), [2, 2, 4, 2])
end
~~~

Use `drawLine(pen, point1, point2)` for a single segment or `drawLines(pen, points)` for an open polyline. `drawPolygon(instrument, points)` closes the last edge. `drawRoundRect(instrument, rect, corner)` takes a rectangle and a corner radius in grid units. Each point is an `[x, y]` pair. Use a Pen for an outline or a Brush for a polygon or rounded rectangle fill. [FlowStone Ruby shapes and lines](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=159)

Use `drawString(text, font, format, location, brush)` to draw a label. Pass `[x, y]` to anchor unwrapped text at a point or `[x, y, width, height]` to lay out text in a rectangle. Coordinates and font size use local grid units. Pass `nil` for top-left placement or create a `StringFormat` and call `setAlignment` and `setLineAlignment` with `"near"`, `"center"`, or `"far"`. OpenStone handles the flags for right-to-left text, vertical text, visible control characters, trailing spaces, no wrap, whole lines only, and no clip. [FlowStone Ruby text reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=163)

~~~ruby
# input View canvas

def draw(view)
  format = StringFormat.new
  format.setAlignment "center"
  format.setLineAlignment "center"
  view.drawString("Output", Font.new("Arial", 1.2, "bold"), format,
                  [0, 0, view.width, view.height], Brush.new(Color.new(245)))
end
~~~

Use `view.measureString(text, font, format, location)` in `draw` to get a Ruby hash with `"bounds"`, `"width"`, `"height"`, `"chars"`, and `"lines"`. It uses the same font layout and grid units as `drawString`. Pass a point to measure an unwrapped label or a rectangle to measure the text that fits under the supplied format. Character counts use Unicode code points and count a CRLF as one line break. [FlowStone text measurement reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=164)

`view.drawBitmap(bitmap, location, alpha)` draws a Bitmap input. Pass `[x, y]` to draw at its natural size or `[x, y, width, height]` to fit a rectangle. Optional alpha runs from 0 to 255 and defaults to 255. The bitmap's `width` and `height` use eight reference pixels per grid unit. Use `widthPixels` and `heightPixels` for pixel dimensions. Use `pen.setDashStyle("dot")` for a dotted stroke. [FlowStone Ruby bitmap reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=166)

Use `view.width` and `view.height` for the local View size. Put Green output and scheduling calls in Ruby event handlers. [FlowStone drawing guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=156)

Call `redraw "canvas"` from an event or scheduled method to repaint the named View, or call `redraw` to use the first View input. For a partial repaint, use `redraw "canvas", [x, y, width, height]` in that View's local grid coordinates. OpenStone maps and clips the area to the panel and retains the previous paint outside it. Ruby Views repaint automatically until the node first requests a redraw. [FlowStone Ruby redraw reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=168)

### Mouse input from a Ruby View

Define `isInMousePoint(x, y)` to accept input at a point with `true` or `1`. Use `mouseLDown` and `mouseLUp` for left clicks, and `mouseRDown` and `mouseRUp` for right clicks. Mouse callbacks take local grid coordinates and accept an optional leading connector reference when several Views are connected. Enable **Mouse** on Module GUI to receive `mouseMove` while the pointer hovers. [FlowStone Ruby interaction guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=198)

~~~ruby
# input View canvas
# output Float X

def isInMousePoint(x, y)
  x < 6 && y < 4
end

def mouseCursor(x, y)
  "resizeew"
end

def mouseLDown(x, y)
  captureMouse
  showCursor false
  output "X", x
end

def mouseMoveCaptured(x, y)
  output "X", x
end

def mouseLUp(x, y)
  showCursor true
  releaseMouse
end
~~~

Use `getViewSize(0)` to get the width and height of a connected View from a callback. Use `isKeyPressed("SHIFT")` to query the held key state. A captured release calls `mouseLUpCaptured` or `mouseRUpCaptured` when defined, with the regular release callback as a fallback.

Capture keeps drag and release callbacks on the original View even when the pointer leaves it. OpenStone also clears the capture when the button comes up or the editor loses the grab. [FlowStone mouse capture reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=201)

Return a cursor name or numeric ID from `mouseCursor`. The editor queries it over the hit region even when the **Mouse** input is off. `showCursor(false)` decrements the visibility counter and `showCursor(true)` increments it. The cursor is hidden below zero. OpenStone clears a captured gesture's cursor changes on release or cancellation. [FlowStone cursor reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=204)

### In-place Ruby text editing

Call `createEdit(view, id, [x, y, width, height], text, font, textColor, background, multiline)` to open a field on a View input. Use its name, index, or connector reference. The rectangle uses local grid coordinates. The arguments after it are optional. Pass a `Font.new(face, size, style)` and `Color` values to style the field, or use `nil` for defaults.

~~~ruby
# input View canvas
# output String value

@title = "Untitled"

def isInMousePoint(x, y)
  x < 6 && y < 2
end

def mouseLDown(x, y)
  createEdit "canvas", :title, [0, 0, 6, 2], @title
end

def editChanged(id, text)
  watch id, text
end

def editFinished(id, text)
  @title = text
  output "value", text
end
~~~

`editChanged(id, text)` receives the current text after each change. Use `editFinished(id, text)` to commit the final text on Tab or focus loss. Enter also commits a single-line field. Both callbacks are optional and receive the ID passed to `createEdit`. [FlowStone in-place editing reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=206)

### Ruby drop lists

Call `createDropList(view, id, [x, y], items, checked, style, disabled)` to open a list at a point in the selected View's local grid coordinates. Pass the items as a comma-separated string. The last three arguments are optional. `checked` and `disabled` are zero-based item indices. Use `"scroll"` for a single scrolling column, `"autocol"` for automatic columns, or a positive number for the number of items per column. [FlowStone Ruby drop lists](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=207) [FlowStone drop list layout reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=118)

~~~ruby
# input View canvas
# output String selected

def isInMousePoint(x, y)
  x < 6 && y < 2
end

def mouseLDown(x, y)
  createDropList "canvas", :wave, [x, y], "Sine,Triangle,Square", 0, "autocol"
end

def dropListFinished(id, index, name)
  output "selected", name
end
~~~

`dropListFinished(id, index, name)` runs once when an item is selected and receives the original Ruby ID. Dismissing the menu leaves the previous selection in place.

### Ruby message boxes

Call `messageBox(id, text, title, type)` to open a dialog. The title and type are optional. Type accepts a button arrangement, an icon, and a default button in any order. The arrangements are `okcancel`, `retrycancel`, `yesnocancel`, `yesno`, and `abortretryignore`. The default arrangement is a single OK button. `messageBoxFinished(id, result)` receives the original Ruby ID and a lowercase button name. [FlowStone Ruby message box reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=208)

~~~ruby
# input Trigger ask
# output String answer

def event
  messageBox :replace, "Replace the current value?", "Confirm", "yesnocancel question defbutton2"
end

def messageBoxFinished(id, result)
  output "answer", result if id == :replace
end
~~~

Escape reports `"cancel"` if Cancel is available, or `"ok"` in an OK-only box. Choose a button to close a Yes/No or Abort/Retry/Ignore box. [Windows message box keyboard behavior](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-messagebox)

### Playing sounds from Ruby

`playSound("Sounds/chime.wav")` plays a WAV once while Ruby continues. Pass `"sync"` as the second argument to wait for the sound to finish, or `"loop"` to repeat it. `playSound(nil)` stops playback. Relative paths use the saved schematic's directory or the current working directory for an unsaved schematic. Playback uses the desktop's default audio device. [FlowStone sound playback reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=210)

~~~ruby
# input Trigger play
# output Trigger finished

def event
  playSound "Sounds/chime.wav", "sync"
  output "finished", nil
end
~~~

A synchronous call holds the Ruby callback and editor until the sound finishes.

### Finding the schematic folder from Ruby

`schematicLocation` returns the absolute folder containing the saved schematic and reflects Save As on the next Ruby callback. It returns `""` in a new, unsaved OpenStone project. A standalone export runtime can supply the executable path, and hosted plug-ins use the installed plug-in folder supplied by the host adapter. [FlowStone release notes](https://dsprobotics.com/versions.html), [FlowStone Ruby utility methods](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=212)

### MIDI in Ruby

Declare a `# input MIDI message` or `# output MIDI message` port to route MIDI through a Ruby script. An incoming channel message's `to_array` returns `[status, channel, data1, data2]` with channels numbered 1 through 16. A System Exclusive message returns `[hex, byte_count]`. Use `MIDI.new(status, channel, data1, data2)` to create a channel message or `MIDI.new("F0 7D 01 F7")` to create a SysEx message. A MIDI output can send immediately or at a scheduled time. [FlowStone Ruby MIDI reference](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=219)

~~~ruby
# input Trigger play
# output MIDI notes

def event(i, value, t)
  output "notes", MIDI.new(0x90, 1, 60, 100)
  output "notes", MIDI.new(0x80, 1, 60, 0), t + 0.25
end
~~~

## Headless runtimes

```sh
cmake -S . -B build/headless -G Ninja -DOPENSTONE_BUILD_DESKTOP=OFF
cmake --build build/headless
ctest --test-dir build/headless --output-on-failure
```

### Mono streams

Use `stream::Graph` to connect Mono input buffers, fixed gains, sums, and one-sample delays. `compile` takes the output taps in channel order and schedules the graph reachable from those taps. A sum combines its sources in link order. Unwired inputs read as zero. Each frame publishes the old value of every delay, computes gains and sums, and then updates delay state. A feedback cycle needs a delay or staged DSP Code state. [FlowStone DSP stages](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=245)

`Runtime::process` takes equally sized channel spans. It preserves delay state across calls and accepts different frame counts on successive calls. `reset` restores the delays to zero. To run the Mono feedback fixture alone, use `ctest --test-dir build/headless -R '^stream_runtime$' --output-on-failure`. It feeds a single impulse into `y[n] = x[n] + 0.5*y[n-1]` and compares the full response across fixed and varying block sizes.

### Mono4 streams

Use `Graph::compileMono4` to run the same stream graph on four channels at once. Each `Mono4` buffer element contains one sample from each channel in pack order. `addGain` applies a fixed scalar to every lane. `addMultiply` takes two streams and multiplies corresponding lanes. The feedback test compares each lane with an independent Mono render at several block sizes.

### MonoDouble streams

Use `Graph::compileMonoDouble` for a single channel with double-precision samples, gain coefficients, and delay state. The feedback test uses an impulse of `1 + 2^-40` and checks the exact decay across the same block sizes as Mono.

Use `Graph::compileMonoDouble4` to process four double-precision channels together, in the same lane order as Mono4. The staggered feedback test compares all four lanes with separate MonoDouble renders across fixed and varying block sizes.

### DSP Code

In the editor, hover over a DSP port or wire to see its waveform and final sample. The swatch updates live for signals in the running Audio Out graph and otherwise shows the last offline capture. **Capture…** records up to 128 recent samples on each rendered signal, including upstream ports. Poly and packed swatches display lane 0. Hovering never starts a render.

`streamin` and `streamout` adapt to Mono, Poly, or Mono4 wires. Their double-precision versions adapt to MonoDouble, PolyDouble, or Mono4Double. Connected generic ports share the processing mode through DSP Code blocks and module connectors. Removing the last typed link restores the grey Stream type. A section that needs both Mono and Poly uses a converter between them.

Multiple wires on one numeric stream inlet sum sample by sample, including at module boundaries. Packed streams sum corresponding lanes. Boolean streams combine with OR. A comb filter can wire its dry input and scaled feedback directly into the same delay inlet. Pack and Unpack can route one lane to the next within the same sample. If an audio path feeds a signal into itself, the editor's DSP scheduler inserts a one-sample delay to break the cycle. An existing delay or staged output already breaks feedback on that path.

**Mono to Graph** outputs a Float Array per channel for 1, 2, 4, or 8 Mono inputs. When its signals are routed to the running Audio Out, **Get** takes the latest **NS** samples from a shared time window. A disconnected channel and the portion before the first live samples are silent. For an offline patch, **Get** starts a fresh render. **Signal Analyser** runs one preview Poly voice from its initial state on **Recalc**. Its outputs are the samples, optimized LLVM IR for the rendered DSP blocks, and elapsed render time per sample (nanoseconds, including render dispatch). The interpreter reports its backend in place of LLVM IR. Both primitives accept **NS** from 1 to 16384 and default to 512.

**Mono to Float** publishes one Green Float per **Get** trigger. Connect **Tick** to **Get**, and wire **Sample** to a Float box or hover over the output to read the held value. For a signal in the running Audio Out graph, Get reads the latest live sample. Otherwise the editor starts an offline 48 kHz preview on the first Get and advances it by up to two seconds per trigger.

**Selector** and **Multiplexer** route a single input or output using a zero-based Green Int index. Their data connectors can carry any supported connector type. For DSP, OpenStone prepares the selected route and skips code used only by disconnected branches. Changing the index prepares a fresh DSP graph and switches the running render at an audio buffer boundary. An unwired selection produces silence. [Selector reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=420) · [Multiplexer reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=301)

### Assembler

Find **Assembler** under **Streams & DSP**. Paste a FlowStone-style Assembler block and click **Compile**, or leave the editor to apply it. Connector declarations create the node's ports. OpenStone saves the assembly source and uses the active DSP backend to run it. The following example halves the input.

```text
streamin in;
streamout out;
float factor = 0.5;

movaps xmm0, in;
mulps xmm0, factor;
movaps out, xmm0;
```

Use named stream connectors or `float`, `int`, `bool`, and `double` locals as operands in `xmm0` through `xmm15`. Each XMM register holds 128 raw bits, viewed as four 32-bit words by PS/integer instructions or two 64-bit words by PD instructions. `movaps` and `movapd` copy the same 16 bytes across either view, including named memory with a different declared element type. `movss` and `movd` preserve adjacent bytes on partial stores. A four-lane Double buffer uses two XMM windows, such as `in0[0]` and `in0[2]`. Control-flow joins and local calls retain register initialization independently of the last instruction view. [Intel packed-move reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf#page=47) Write registers before reading them in each stage. Use locals for state shared between stages or between samples. Instructions run in stage 2 by default. Explicit stages 0 through 3 follow the DSP Code timing described below.

Supported instructions include packed moves, arithmetic, minimum and maximum, square root, comparisons, shuffling, bitwise operations, and float/integer conversion. `movss` and `movd` move a low 32-bit lane, and `pmuludq` packs two unsigned products. `pslld` and `psrld` take a byte immediate or an XMM or memory operand. The unsigned count in the low 64 bits applies to all four lanes. Counts of 32 or more yield zero. `rcpps` uses a full-precision reciprocal, so its low bits can differ from x86 hardware.

Assembler SSE arithmetic uses round-to-nearest with ties to even. DSP rendering isolates the caller's floating-point environment and restores its rounding, flush/default-NaN controls, and exception flags afterward. Moves preserve raw subnormal and signaling-NaN words at graph boundaries. Arithmetic applies its own denormal flushing. NaN sign, payload selection, quieting, and invalid-result words are explicit in both backends. The current policy matches the saved FlowStone Alpha 5483 x64 captures on Windows ARM; it does not establish identical results on every physical Intel or AMD CPU. The raw-word regressions cover `addps`, `subps`, `mulps`, `divps`, `sqrtps`, `minps`, `maxps`, `cvtps2dq`, `cvttps2dq`, and the seven corresponding double arithmetic/min/max/sqrt operations. [Floating-point capture details](../tests/fixtures/flowstone/README.md#sse-floating-point-edge-cases)

`cvtpd2dq` rounds two doubles to signed 32-bit integers using nearest-even rounding; `cvttpd2dq` truncates toward zero. Both clear the upper two integer lanes and return `0x80000000` for invalid conversions, including NaNs, infinities and out-of-range results. `shufpd` selects one double from the destination register and one from its second operand using the low two immediate bits. It preserves raw NaN, subnormal and signed-zero bits. Register, named-memory and register-indirect operands run through both backends. [Intel conversion reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf#page=337) · [Intel shuffle reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf#page=619) · [Bundled SSE2 fixtures](../tests/fixtures/flowstone/README.md#sse2-double-conversions-and-shuffle)

Scalar instructions use `eax`, `ebx`, `ecx`, and `edx`. Register reads require a preceding write on every path. Scalar access to a named operand uses the raw 32-bit word at lane 0. Signed and equality branches use flags from `cmp`, `comiss`, or integer arithmetic. `jge` (alias `jnl`) takes when SF=OF; `jle` (alias `jng`) takes when ZF=1 or SF differs from OF. They use signed flags, including overflow correction after arithmetic, and do not modify flags. Label branches can target an earlier or later instruction within the same stage. `push` and `pop` use a stage-local stack. `rdtsc` returns monotonic host-clock ticks in `edx:eax`.

`comiss xmmN, xmmM` (or a named, indexed, or Mem-indirect memory source) compares the low raw Float word and preserves both operands. Its ZF/PF/CF results are 000 for greater, 001 for less, 100 for equal, and 111 for unordered (either NaN). Signed zeros compare equal. It uses the same subnormal flushing as the other SSE comparisons. The signed-less flag state clears because SF and OF clear. Use `ja`/`jb`/`jbe`/`jae` for carry-based float relations and `jp`/`jnp` for unordered/ordered. `je`/`jz` and `jne`/`jnz` test ZF; unordered therefore takes `je`. `jg` still follows signed EFLAGS rules, so use `ja` for floating-point greater-than. [Intel COMISS reference, section 5-84](https://www.intel.com/content/dam/develop/external/us/en/documents/319433-024-697869.pdf)

`cmp`, `add`, and `sub` now produce 32-bit unsigned carry/borrow and even parity of the wrapped result's low byte. CMP tests a subtraction without changing its operands. `inc` and `dec` update parity, zero and signed-branch state while preserving both the value and availability of carry. A carry branch after either still needs an earlier carry producer. DEC wraps `0` to `0xFFFFFFFF`; decrementing `0x80000000` yields `0x7FFFFFFF` with signed overflow reflected in signed branches. Both accept a numeric register or writable memory operand. [DEC flag contract](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf) Unsigned branches use carry independently of signed overflow; signed branches retain SF/OF behavior at wraparound. [Intel status-flag definitions](https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf)

Scalar AND/OR clear carry and update parity. Nonzero SHL/SHR use the last shifted-out bit for carry and the result's low byte for parity; zero-count shifts preserve flags. Moves, stack operations and packed SSE operations preserve them. Flag producers must be valid on every incoming path. Opaque Mem reference null checks and address arithmetic keep their existing portable reference semantics and do not expose native numeric carry/parity. SIMD exception-status/trap emulation remains outside this flag model. [Integer flag tests](../tests/fixtures/flowstone/README.md#integer-carry-and-parity-flags), [COMISS tests](../tests/fixtures/flowstone/README.md#scalar-comiss-flags-and-branches)

`jmp` and `jnz` also accept signed byte displacements from the end of the branch instruction. For example, `jmp 2;` skips a two-byte `mov eax,edx;`. The compiler uses FlowStone Alpha's x86-64 instruction widths for supported register, immediate, and named-memory forms. The target must fall on an instruction boundary in the same stage. All backward branches require a reachable target with compatible register and stack state. The stage returns control to the audio graph if it attempts a 4,097th backward branch in one sample. [FlowStone Assembler reference](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/downloads/FLowstone_UserGuide.pdf#page=250)

Call a local subroutine directly with `call adjust;` or bind its label with `mov eax,adjust;` and use `call eax;`. The address-register spellings such as `rax` work too. Branches can assign different local labels to a register before `call eax` or `jmp eax`. The target is selected at runtime. Every incoming path must provide a local label. A subroutine can branch within its body and call other local subroutines. Call targets can appear earlier or later in the same Assembler section and must have unique labels. Recursive routines can make multiple calls. The depth limit is 128 frames, with a separate limit of 4,096 calls within recursive chains per stage per sample. Exceeding either limit ends the stage for that sample and keeps writes already made. A local subroutine can have multiple `ret` instructions. Each resumes its immediate caller, which can read registers written on every returning path. Balance each subroutine's pushes and pops. The compiler shares recursive bodies with compatible register and stack state at the same depth and limits compilation to 4,096 nested body expansions per Assembler block. A labeled `jmp` or register `jmp eax` inside a local subroutine can transfer to another routine in the same section after balancing pushes and pops. Selected targets also survive register copies, push/pop, subroutine returns, and scalar `mov` stores to and loads from named local memory. Packet lanes and local arrays retain targets per cell. Computed table loads require every candidate cell to contain a saved local target. An invalid table index ends the stage for that sample. Mem-backed cells can save local targets at fixed or runtime-selected byte offsets, including `mov [eax+ebx*4],ecx;`. Indexed writes can initialize an empty table or replace existing targets. Saved entries persist across samples and buffers while the same Mem remains bound. To copy an entry, load it with `mov ecx,[eax+ebx*4];` and store it with `mov [eax+edx*4],ecx;`. The destination retains its own saved target if the source is later overwritten. Copies can be chained through further indexed cells, including on later samples and across buffers. To swap two entries, load both targets into temporary registers before storing them into the opposite indexed cells. `call [eax]`, `jmp [eax]`, and indexed forms such as `call [eax+ebx*4]` read a saved target directly from the addressed cell and preserve the address and index registers. A runtime-selected offset must identify a cell that actually received a saved local target. Numeric writes invalidate overlapping target cells, including writes through an aliased Mem input. Binding different storage clears its target tracking. Empty or short buffers end the stage for that sample. The target's `ret` resumes the original caller without adding a call frame. Tail-jump cycles use the backward-branch limit. Jump past subroutine definitions in the stage so execution enters them through a call or a tail jump. A `ret` outside a subroutine ends the stage. [FlowStone Component Reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=41)

Assembler also supports ESI, EDI and EBP as independent 32-bit numeric registers, with RSI, RDI and RBP as pointer/local-target aliases under the same rules as RAX–RDX. Their state follows the existing per-stage initialization and provenance checks. The eight legacy byte aliases keep their original EAX–EDX parents. [Native evidence and tests](../docs/compatibility/flowstone-assembler-extra-gprs.md).

`streamdoublein` and `streamdoubleout` are available to Assembler, along with their explicit MonoDouble and PolyDouble variants. Generic double ports also adapt to four-lane double streams. Use `movapd` or `movupd` for double moves. Double arithmetic includes `addpd`, `subpd`, `mulpd`, `divpd`, `minpd`, `maxpd`, and `sqrtpd`. Raw moves such as `movaps` follow the source packet width. Assembler operands must agree on packet width.

`cmppd` accepts predicates 0 through 7 and the corresponding named aliases from `cmpeqpd` through `cmpordpd`. The result is a raw binary64 mask with every bit set for true and every bit clear for false. Use a double move and a double output to preserve the mask on a stream.

`cmpgtps` and `cmpgtpd` follow FlowStone Alpha’s predicate-6 aliases (`cmpnleps` and `cmpnlepd`): each lane is true when the destination is greater than the source **or either operand is NaN**. Equal values, including any pairing of signed zeros, are false. True sets every bit of the 32-bit or 64-bit lane; false clears every bit. Register, named-memory and indirect sources work with an initialized XMM destination. [Alias mapping and mask tests](../tests/fixtures/flowstone/README.md#greater-than-comparison-aliases)

`cvtdq2pd`, `cvtps2pd`, and `cvtpd2ps` convert the low two lanes and clear the upper lanes.

`idiv divisor` divides signed EDX:EAX by one signed 32-bit register or memory operand. EAX receives the quotient truncated toward zero; EDX receives the dividend-signed remainder. Both dividend words must be initialized. The divisor is captured before either result is written, including EAX/EDX aliases. Arithmetic flags become undefined and require a new producer before a conditional branch.

Zero divisors and out-of-range quotients stop that Assembler stage without writing either result register. `runtime.dspFault(block.id)` reports the first fault kind and absolute sample frame since `reset()`. Earlier instruction effects remain; later samples can recover. Buffer processing still succeeds for valid buffers, so arithmetic faults use this separate diagnostic instead of a host exception. [Signed-division tests](../tests/fixtures/flowstone/README.md#signed-idiv-and-divide-faults)

`mov` accepts AL/BL/CL/DL and AH/BH/CH/DH for byte memory loads, register copies, immediates and stores. Low aliases select bits 7:0; high aliases select bits 15:8 of their initialized numeric parents. Other parent bits and EFLAGS stay unchanged, including copies such as `mov ah,al`. Stores such as `mov [eax+edx*4+3],ch` update exactly one byte and preserve neighboring memory and the source parent. Writable named storage and loaded wave data also support byte stores; inputs and descriptor tables stay protected. Other byte instructions remain separate operand forms. [Byte-register tests](../tests/fixtures/flowstone/README.md#high-byte-mov-registers)

`add`, `sub`, and `cmp` also accept the eight byte aliases, byte immediates and memory operands whose byte width is specified by a register operand. Results wrap at eight bits; ADD/SUB preserve the other parent bits or neighboring bytes, and CMP leaves both operands unchanged. Carry/borrow, zero, parity and signed branch conditions use eight-bit arithmetic, including signed overflow. For example, adding one to AL in `0x123400FF` produces `0x12340000` with carry and zero set. [Byte arithmetic tests](../tests/fixtures/flowstone/README.md#byte-add-sub-and-cmp)

Assembler now binds the verified FlowStone engine constants `_DONE` (+1), `_DMONE` (−1), `_DABS` (clear-sign mask), and `_DSIGN` (sign-only mask) as read-only Double packets when they are undeclared. Their exact bits were checked against both Alpha 5483 executables. Five unchanged Double kernels now compile and render without injected declarations, bringing the strict audit from 44 to 49 accepted entries. Explicit variables retain their existing meaning. [Evidence and tests](../docs/compatibility/flowstone-assembler-engine-constants.md)

Byte `idiv` accepts all eight low/high register aliases and explicit `byte [ptr]` memory operands. It divides signed AX by a signed byte, truncates toward zero, writes the quotient to AL and the dividend-signed remainder to AH, and preserves EAX bits 31:16 and EDX. It captures AL/AH divisors and address indices before writing. A zero divisor or quotient outside -128..127 stops the stage with the existing divide-fault diagnostic and commits neither result. Arithmetic flags become unavailable. Unsized memory divisors retain the existing 32-bit form. [Byte-divide tests](../tests/fixtures/flowstone/README.md#byte-idiv-division)

`inc` and `dec` accept all eight low/high byte aliases. They wrap the selected byte, preserve the remaining parent bits and incoming carry, and update zero, parity and signed branch conditions using eight-bit overflow. For example, `inc ah` changes `0x12347F56` to `0x12348056`. Carry remains unavailable if no valid producer preceded the step. Unqualified memory steps retain their existing 32-bit width. [Byte-step tests](../tests/fixtures/flowstone/README.md#byte-inc-and-dec)

`and`, `or`, `xor`, and `test` accept all eight low/high byte aliases with register/immediate operands and register-sized byte memory forms. They clear carry and overflow, and set zero, parity and signed branch state from the byte result. AND/OR/XOR merge only the selected byte; TEST preserves both operands. A byte self-XOR still requires an initialized numeric parent so its other 24 bits can be retained. TEST uses a register or immediate as its second operand. [Byte logical tests](../tests/fixtures/flowstone/README.md#byte-and-or-xor-and-test)

`shl` and `shr` accept all eight low/high byte aliases with an immediate count from 0 to 255 or `cl`. Counts use their low five bits; zero preserves all flags and parent bits. Nonzero shifts replace only the selected byte, update zero/parity and shifted-out carry, and handle one-bit overflow in signed branches. Masked counts from 8 through 31 zero the byte. For x86-undefined results, multi-bit overflow follows the existing scalar convention of zero, and large-count carry follows iterative byte shifting. A variable `cl` count preserves flag availability because it may be zero. Unsized byte-memory destinations remain unsupported. [Byte-shift tests](../tests/fixtures/flowstone/README.md#byte-shl-and-shr)

`xchg left, right` exchanges initialized 32-bit integer registers or one register and writable memory, in either operand order. It snapshots both values before writing, uses the original effective address when the register is also an index, and preserves EFLAGS. Register swaps also carry OpenStone's opaque reference and local-routine metadata with the values. Self-swaps preserve the value and its metadata. [Exchange tests](../tests/fixtures/flowstone/README.md#scalar-xchg-swaps)

`xchg` also accepts all eight low/high byte aliases, between two registers or a register and writable memory in either order. It exchanges the two old bytes, preserves all other parent bits and EFLAGS, and writes memory before changing a parent used as an address index. Self-swaps and AL/AH-style aliases use the same captured operands. Byte exchanges require initialized numeric parents; partial exchanges cannot carry opaque references or routine targets. [Byte-exchange tests](../tests/fixtures/flowstone/README.md#byte-xchg-swaps)

`test` and scalar `xor` use 32-bit integer operands. TEST computes a logical AND for flags without storing it; XOR writes its exclusive-OR result. Both clear carry/overflow and set zero, sign and even low-byte parity. TEST accepts a register or memory first operand and a register or immediate second operand. XOR accepts register/memory/immediate sources with at most one memory operand. `xor eax,eax` also initializes an unwritten register to zero and clears old reference metadata. [Logical flag tests](../tests/fixtures/flowstone/README.md#scalar-test-and-xor-flags)

`movhlps dst, src` copies the source high 64 bits into the destination low half; `movlhps dst, src` copies the source low 64 bits into the destination high half. The other half and EFLAGS stay unchanged. Both operands must be written XMM registers; self-aliasing duplicates the selected half. These moves preserve all bits across Float and Double register views. [Raw half-move tests](../tests/fixtures/flowstone/README.md#raw-xmm-half-moves)

`cvtss2si eax, source` converts the low raw Float word of an XMM or memory source to a signed 32-bit integer register. It uses the same nearest-even rounding policy as the packed SSE conversions, returning `0x80000000` for NaNs, infinities or out-of-range results. It preserves the source and the modeled EFLAGS. Register, named/indexed memory and Mem-indirect sources work; 64-bit integer destinations remain outside the scalar register model. [Conversion tests](../tests/fixtures/flowstone/README.md#scalar-cvtss2si-conversion)

`andpd`, `andnpd`, `orpd`, and `xorpd` combine the raw bits of double operands. They can apply a comparison mask to a double stream. `andnpd` writes `(~destination) & source`. [Intel instruction reference](https://software.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf)

Named memory operands support lane and array indices. A constant such as `table[3]` selects the fourth element. A register expression such as `table[eax*4+ebx+16]` gives a signed byte offset relative to the named storage. Addresses must be aligned to an element. Packed accesses transfer up to four 32-bit or two 64-bit elements starting at the index. Indexed double moves can use `in[0]` and `in[2]` to work on the two halves of a four-lane signal. Scalar accesses use one element, and the x87 stack accepts both `st(0)` and `st[0]`. Reads return zero for elements outside the named storage, and writes skip those elements. [FlowStone Assembler release notes](https://dsprobotics.com/versions.html)

Use local arrays such as `float table[32];` for writable storage. `memin` and `floatarrayin` declare read-only array connectors with a fixed capacity, as in `memin wave[1024];`. Headless hosts can supply data with `setDspArrayInput`.

Declare `memrefin buffer;` for a reference to a connected Green Mem, and use `mov eax,buffer[0];` or `mov rax,buffer[0];` to bind the register. Indirect operands like `[eax]`, `[rax+16]`, and `[rax+rcx*4]` address the connected buffer by byte offset. Scalar and packed moves and x87 loads and stores share this storage with Green. Each out-of-bounds element reads as zero or skips its write. Fields `buffer[1]`, `buffer[2]`, and `buffer[3]` report the byte, float, and channel counts. Compare the reference register with zero to check whether the Mem is empty. Headless hosts can bind a Mem with `setDspMemRefInput` between render calls. [FlowStone Assembler release notes](https://dsprobotics.com/versions.html)

Declare `memarrayrefin waves;` to connect a Green MemArray, such as the output from Wave File Array. OpenStone supports the FlowStone Alpha 64-bit descriptor: fields `[0]` and `[1]` expose the replicated integer wave count, and `[2]` and `[3]` the replicated floating-point last wave index. Fields `[4]` through `[7]` point to the channel-count, wave-address, byte-size, and last-frame-index tables. Counts, channels, and byte sizes use 32-bit integers; frame limits use floats; wave-address entries have an eight-byte stride. Load a wave pointer with `mov rbx,waves[5]; mov rax,[rbx+eax*8];`, then read samples through operands such as `[rax+ebx*4]`. Stereo frames contain interleaved left/right floats with an eight-byte frame stride.

The descriptor and its tables are read-only. Wave sample reads and writes share the original Green Mem storage. Bindings retain the wave buffers without copying their payloads. Table and sample addresses use a bounded virtual address space rather than host addresses; unbound or invalid accesses return zero or skip the write. The editor prepares these bindings with the audio graph. Headless hosts can replace or clear them with `setDspMemArrayInput` between render calls.

Unsupported instructions and operands report their source location. Numeric spans with unknown instruction widths, arbitrary native addresses, AVX, and native function calls require further implementation. The x87 stack now retains software 80-bit values, using a 64-bit significand and the extended exponent range. Loads, stack copies/exchanges, add/subtract/multiply/divide, `frndint`, `fscale`, and the complete `fprem` value operate at that precision. `fst`/`fstp` round to Float memory, `dfst`/`dfstp` round to Double memory, and non-popping stores retain the original stack value. `fdivp` and `dfdivp` divide ST(1) by ST(0), then pop. The interpreter and LLVM ORC share Berkeley SoftFloat arithmetic; this works without LLVM and does not depend on the host `long double` format. Exact `fsin`, `fcos`, `fsincos`, `fptan`, `f2xm1`, and `fyl2x` remain gaps: they still use the binary64 host math model. `fprem` does not yet implement partial-reduction/status behavior. The x87 arithmetic model currently uses full 64-bit significand precision with nearest-even rounding; FlowStone's effective precision setting on physical Windows x64 awaits community measurements. [Community precision probe](../tests/fixtures/flowstone/README.md#x87-extended-precision-and-community-probe) In a Poly graph, consecutive voice slots execute DSP stages together in four-lane packets. Packed instructions such as `shufps` can exchange values between those voices. Packets have fixed slot membership; inactive lanes enter a DSP stage as zero and only active lanes write back. Scalar graph processors and delay state remain private to each voice. FlowStone schematic files require a separate importer. [FlowStone Component Reference](https://dsprobotics.com/Files/V3/Component%20Reference.pdf#page=40)

The compile-only bundled-source audit and its remaining gaps are recorded in [Assembler audit findings](../docs/compatibility/flowstone-assembler-audit-notes.md). Enable `OPENSTONE_BUILD_ASSEMBLER_AUDIT` to build the probe, then run `python3 tools/audit_flowstone_assembler.py --help` for extraction and report options. This audit preserves the source and prepares both backends without rendering samples.

### Live audio

Place **Audio In** and **Audio Out**, wire **Left** to **Left** and **Right** to **Right**, then click **Start Audio**. The app uses the system’s default devices and shows the active sample rate in the footer. Allow microphone access on macOS if prompted. With one hardware input channel, both Audio In connectors carry that channel. An unwired Audio Out channel produces silence.

There is one Audio Out per schematic. Scalar Green values wired into DSP Code can change while audio runs. Restart audio after rewiring or recompiling to prepare the changed graph. DSP array inputs are read when the graph is prepared.

`Schematic::prepareAudio` prepares only the graph upstream of Audio Out. The resulting `stream::AudioProcessor` processes planar float input and output buffers supplied by its host. The desktop adapter opens and closes PortAudio streams. A plug-in host can supply the same buffers without PortAudio.

Use `stream::compileDspCode` to compile source into an immutable program. Input and output names and types are available in declaration order. Attach the program with `Graph::addDspCode`, connect graph sources by input index with `connectDspInput`, and wire its returned output nodes as ordinary graph outputs. Invalid code reports a line and column via `DspCompileError`.

DSP Code and Assembler accept connector lists such as `streamdoublein step, length;` and `streamout read1, read2, weight1, weight2;`. Each name creates a separate connector in source order. Array inputs keep individual bounds, for example `memin left[128], right[256];`. `stage` can name a variable or connector; `stage(n) { ... }` still introduces a stage block. Assembler also accepts `stage n;` for stages 0–3, selecting the stage for following top-level instructions. Parenthesized blocks keep their existing scope and return to stage 2 after the closing brace. [Stage directives and recovered delay kernels](../docs/compatibility/flowstone-assembler-stage-directives.md). In DSP Code, `stage(0)=stage+1;` is a lane assignment to a variable named `stage`.

```text
streamin input;
streamout output;
float previous;
stage(0) { previous = 0; }
stage(1) { output = previous; }
stage(3) { previous = input; }
```

Stage 0 initializes locals on the first sample after construction or reset. Stage 1 can read current graph inputs, delay values, upstream Stage 1 outputs, and held Stage 3 outputs. Unstaged assignments and explicit `stage(2)` blocks run in Stage 2. Stage 3 reads the completed sample and updates state for the next one. Assign each output in one stage. A Stage 3 output starts at zero and becomes visible on the following sample, so it can be used in a feedback loop. The optional FlowStone header `# COMPATIBILITY_3_0_8` is accepted. Use `loop(n)` for fixed iterations and `hop(n)` to run at multiples of `n`, starting at sample zero. Hop timing continues across buffers. [FlowStone User Guide, DSP Code stages and sections](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/downloads/FLowstone_UserGuide.pdf#page=243)

A program can mix `float`, `double`, signed 32-bit `int`, and `bool`. It supports Alpha's casts, math functions, ternary, compound assignments, masks, and bitwise operators. `^` is power and binary `~` is XOR. Hexadecimal initializers set the variable's raw bits. Direct links between DSP Code blocks carry values at their declared precision. Regular graph buffers still use the sample type selected at graph compilation. [FlowStone DSP Code release notes](https://dsprobotics.com/versions.html)

A scalar initializer fills a local array. A braced array initializer fills consecutive elements and leaves the rest zero. A braced scalar initializer sets individual lanes. Numeric arrays can use `rand()`, `rand(max)`, or `rand(min,max)`. OpenStone repeats the same pseudorandom sequence after reset. Array reads past either boundary return zero, and writes outside the array have no effect. In the headless runtime, use `setDspArrayInput` between render calls to supply FloatArray or decoded Mem samples. Input data is truncated or zero padded to the declared capacity and survives reset.

Lane selectors use indices 0 to 3. A read such as `x{1,3}(0)` copies lane 0 into lanes 1 and 3 and zeroes the other lanes. On an assignment, a mask keeps unselected destination lanes and a broadcast copies the specified result lane. Mono4 and MonoDouble4 graphs expose all four lanes. A scalar graph exposes lane 0 at graph boundaries. `PolyRuntime` gathers four consecutive voices for each DSP stage, so lane selectors and packed Assembler instructions can read neighboring voices within that packet. Voice outputs are then scattered back to their own graph state and mixed or selected by Poly to Mono.

When built with LLVM 21, OpenStone compiles each DSP stage to LLVM IR, optimizes it, and resolves machine code with ORC when an output render first needs the block. Both backends use the same graph scheduler and initialized state. Rendering reuses the compiled functions and does no JIT work. An LLVM-enabled build defaults to ORC. Pass `DspBackend::Interpreter` or `DspBackend::LlvmOrc` to the `Graph` constructor to select a backend explicitly. A build without LLVM defaults to the interpreter. [LLVM ORC documentation](https://llvm.org/docs/ORCv2.html)

```sh
cmake -S . -B build/orc -G Ninja \
  -DOPENSTONE_BUILD_DESKTOP=OFF -DOPENSTONE_ENABLE_LLVM_ORC=ON \
  -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build/orc
ctest --test-dir build/orc --output-on-failure
```

The stream suite in this build runs DSP fixtures through ORC and compares a mixed-precision program against the interpreter across multiple block sizes. The comparison checks exact sample bits, including signed zero, and checks NaNs by classification. FlowStone Alpha parity for rounding ties, denormal handling, random initialization, and expression edge cases still needs direct comparison.

### Poly streams

Use `stream::PolyRuntime` from `stream/PolyRuntime.hpp` to run a `Graph` per note and sum the resulting voices into Mono output channels. Each active note has its own delay history, even when several notes share a pitch. FlowStone also separates voice generation, per-voice processing, and summing to Mono. [Poly streams in the FlowStone User Guide](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=90)

The **Mono to Poly** and **Poly to Mono** primitives have an optional Int **Voice Index**. Leave it unwired to broadcast Mono to every active voice or sum all active Poly voices into Mono. Wire a zero-based slot index to feed or read just that slot, including during its release tail. Empty or invalid slots carry silence. The editor's offline DSP capture renders preview slot 0. In the headless runtime, `setMonoInputVoiceIndex` and `setOutputVoiceIndex` change routing between render calls while voice state continues.

Bind each graph input to a shared Mono channel or a per-voice signal: Gate, the one-sample NoteOn pulse, Pitch, Velocity, Frequency (1 is Nyquist), or Stage. Pitch is a MIDI note number and velocity is normalized from 0 to 1. Construct the runtime off the audio thread with a voice limit, sample rate, and output taps. `process` takes caller-owned buffers and sample offsets for note events. It applies events before the corresponding sample, in the supplied order for events on the same frame. Note-on IDs must be nonzero and strictly increasing until `reset`.

Note-off normally stops a voice immediately. Set `holdAfterNoteOff` to keep it rendering in Release with Gate low until an `EndVoice` event or a nonzero `releaseEnd` graph tap. The tap ends the voice after mixing its final sample. At capacity, a new note replaces the oldest active note and starts with cleared DSP state. A later note-off for the replaced note has no effect. `AllSoundOff` retires every active voice on its event frame.

The scalar reference runtime preallocates voice state and scratch buffers. It sums in voice slot order without normalization or clipping. Run `ctest --test-dir build/headless -R '^poly_runtime$' --output-on-failure` to compare eight overlapping voices with independent Mono renders across fixed and varying block sizes and to test MIDI release behavior.

OpenStone uses the MIDI 1.0 Detailed Specification as the reference for MIDI behavior. `PolyRuntime::processMidi` accepts complete, timestamped Note On, Note Off, Pitch Bend, and Control Change messages. Each `MidiNoteEvent` carries a frame offset, a status byte, and two data bytes. For instance, `{12, 0x91, 60, 64}` starts note 60 on MIDI channel 2 at frame 12 with a velocity of `64 / 127`. MIDI input adapters must decode the transport and pass only supported messages to this runtime. [MIDI 1.0 Detailed Specification](https://midi.org/midi-1-0-detailed-specification)

Every CC number is available as a graph input. Use `PolyInput{.kind = PolyInputKind::MidiCC, .controller = 74}` to read the current CC74 value (0 to 127) for each voice's MIDI channel. Use `MidiCC14` with an MSB controller number (0 to 31, except Data Entry) to read 0 to 16383 from an MSB and its corresponding LSB. A new MSB clears the old LSB for these pairs. Direct Poly voices get zero for both generic bindings. Patches define how controls such as soft pedal, brightness, and effect depth affect the sound. [MIDI 1.0 Detailed Specification, Continuous Controllers](https://www.seriesten.org/docs/protocols/MIDI_1.0_Detailed_Specification.pdf#page=17)

Four controllers also have named graph inputs with 7-bit normalization.

| Controller | Poly input | Input range | Initial MIDI value |
| --- | --- | --- | --- |
| CC1 | `Modulation` | 0 to 1 | 0 |
| CC7 | `ChannelVolume` | 0 to 1 | 100 |
| CC10 | `Pan` | −1 left to +1 right, with 0 at center | 64 |
| CC11 | `Expression` | 0 to 1 | 127 |

Updates take effect on the event frame for sounding and subsequent voices on that channel. The graph can use Channel Volume and Expression as gain factors and choose how to apply Pan and Modulation. Voices from direct Poly events get 0 for Modulation and Pan and 1 for Channel Volume and Expression. [MIDI 1.0 Control Change messages](https://midi.org/midi-1-0-control-change-messages) · [MIDI Association initial controller values](https://midi.org/about-midi-part-4midi-files)

MIDI 1.0 pairs each Note On with a corresponding Note Off and leaves assignment of overlapping voices to the receiver. OpenStone assigns each Note On a new voice ID and matches each Note Off (including a zero-velocity Note On) to the oldest unmatched Note On for that channel and pitch. If the matched note's voice was stolen, its Note Off is consumed without releasing a newer note. `reset` clears unmatched notes.

CC64 controls sustain independently on each channel. For example, `{8, 0xb1, 64, 127}` presses the pedal on channel 2 at frame 8. CC66 controls sostenuto, which captures only keys held when the pedal goes down. For both pedals, values 64–127 are down and 0–63 are up. A voice whose key is up releases as soon as neither pedal holds it. With `holdAfterNoteOff` enabled, its release tail starts on that frame. [MIDI 1.0 Detailed Specification, Hold Pedal](https://www.seriesten.org/docs/protocols/MIDI_1.0_Detailed_Specification.pdf#page=68)

Pitch Bend adjusts Frequency for sounding and subsequent notes on its channel. Pitch still carries the original MIDI key number. The two data bytes encode a 14-bit value with the least significant byte first and center at 8192. For example, `{8, 0xe1, 0, 64}` centers MIDI channel 2 at frame 8. [MIDI 1.0 Pitch Bend message](https://midi.org/summary-of-midi-1-0-messages)

Each channel's bend range starts at ±2 semitones. To change it, select Pitch Bend Sensitivity (RPN 0) with CC101 and CC100 both set to 0. Send CC6 for semitones and CC38 for cents. For example, CC6 = 12 and CC38 = 50 set a range of ±12.5 semitones. Data Entry updates the selected parameter on its event frame, including when the wheel is already off center. CC101 and CC100 both set to 127 select RPN Null. RPN Null and NRPN selection both end Data Entry to these RPNs. [MIDI 1.0 Registered Parameter Numbers](https://midi.org/midi-1-0-control-change-messages)

For Channel Fine Tuning (RPN 1), select CC101 = 0 and CC100 = 1, then send CC6 and CC38. The 14-bit Data Entry value is centered at 8192 with a resolution of 100/8192 cents per step. For Channel Coarse Tuning (RPN 2), select CC101 = 0 and CC100 = 2, then send CC6 to set an offset of −64 to +63 semitones, with 64 at unison. Both tuning offsets add to Pitch Bend for sounding and subsequent notes. [MIDI 1.0 Registered Parameter Numbers](https://midi.org/midi-1-0-control-change-messages)

CC96 and CC97 raise or lower the selected pitch RPN by one step, regardless of the message's data value. A step is one cent for RPN 0, 100/8192 cents for RPN 1, and one semitone for RPN 2. Adjustments stop at the parameter limits. [Data Increment and Decrement (RP-018)](https://midi.org/response-to-data-increment-decrement-controllers)

CC123 acts as Note Off for every held note on its channel, respecting both pedals. CC124–127 apply the same note release in this channel-addressed Poly runtime. CC120 stops every voice on its channel immediately, including sustained notes and release tails, and keeps pending Note Off matches. CC121 lifts pedals CC64–67, centers Pitch Bend, resets Modulation and Expression, and clears parameter selection. Channel Volume, Pan, the bend range, and both tuning offsets retain their settings. CC122 is available as a value for a patch's local-control routing. The runtime validates the data ranges of all Channel Mode messages. [MIDI 1.0 Channel Mode messages](https://midi.org/summary-of-midi-1-0-messages) · [Reset All Controllers (RP-015)](https://midi.org/response-to-reset-all-controllers)

### Green values and triggers

The Green runtime exposes separate operations to store a value, pull current input values, and fire a payload-free trigger. A trigger fires synchronously on the calling control thread. Handlers can pull values, store results, and choose which outputs to fire. Trigger-only connectors have no stored value.

Computed outputs declare a value type and evaluate a reader on each downstream pull. The reader can pull its own inputs, and its result goes through the usual link conversion and fan-in rules. To notify a receiver, call `fire` explicitly from a trigger handler or the control thread. OpenStone reports a read cycle with the output IDs in the cycle and limits how deep a read can go.

Green supports matching value types and these direct conversions:

- Float ↔ Int, Int ↔ Boolean, String ↔ Boolean, String ↔ Int, and String ↔ Float. Float to Int truncates toward zero. Int to Boolean treats zero as false and other values as true. Boolean text uses `True` and `False`. [FlowStone User Guide, pp. 103–104](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=103)
- String ↔ Float Array, using one value per line and six fractional digits when writing array elements. [FlowStone User Guide, p. 106](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=106)
- String ↔ Int Array, String ↔ Point Array, and String ↔ String Array use one element per line. Point Array lines use `x, y` pairs.

A link with a Trigger at either end carries pulses only. An input with both a value link and a Trigger link can reread the value source when the Trigger fires, as in the guide’s Ticker example. [FlowStone User Guide, pp. 269–270](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=269)

Float inputs sum their value sources, and String inputs concatenate them in connection order. OpenStone applies each link’s conversion before combining the values. The default applies when there are no value sources. [FlowStone User Guide, pp. 31 and 46](https://dsprobotics.com/Files/V3/User%20Guide.pdf#page=46) · [Float fan-in example from its schematic author](https://dsprobotics.com/support/viewtopic.php?t=573)

Scalar Green Float values stay finite from `-1e38` through `1e38` inclusive. Values beyond those limits become `-inf` or `+inf`. OpenStone applies this rule to defaults, stored and computed values, String-to-Float conversion, and the final Float input sum.

Int inputs sum their sources with signed 32-bit wraparound. In FlowStone build `3.0.9b2-5483-g56077f217`, wiring two Int sources to one input displayed `5` for `2 + 3` and `-2147483648` for `2147483646 + 2`. OpenStone uses modulo 2³² arithmetic, which also wraps in the other direction on underflow.

FlowStone converts each Float link to Int before summing at an Int input. Float sources `1.75` and `2.75` displayed `1` and `2` individually and `3` when wired together to the same Int input. Adding an Int source of `10` to that input gave `13`.

FlowStone also parses each String link before summing at an Int input. String sources `"12"` and `"34"` displayed `12` and `34` individually and `46` together. Adding an Int source of `10` gave `56`.

FlowStone and OpenStone both read the leading integer in `"1 2 3"` and `"12 3"`, returning `1` and `12`. Both accept a single leading sign. `"+1"` gives `1`, `"-1"` gives `-1`, and `"--1"` gives `0`.

At a Float input, FlowStone parses each String link as a Float before summing. String sources `"1.25"` and `"2.5"` displayed `1.25` and `2.5` individually and `3.75` together. Adding a Float source of `0.5` gave `4.25`.

Boolean inputs return `True` when any source is `True`. Each source can trigger the input even when the combined Boolean stays the same. At an Int input, each Boolean source contributes `1` for `True` or `0` for `False`, so two `True` sources sum to `2`.

FlowStone converts each Int link to Boolean before combining at a Boolean input. Int sources `1` and `-1` both displayed `True` individually and together on the same input. The input stayed `True` with `0` and `-1`, then became `False` with two zeros.

FlowStone also converts each String link to Boolean before combining. Separate `"False"` and `"True"` links gave `True`. Two `"False"` links and a single `"FalseTrue"` link gave `False`. OpenStone accepts the confirmed spellings `"true"`, `"on"`, `"yes"`, and `"y"` in any capitalization. It matches the whole string without trimming whitespace, so `"YeS"` gives `True` while `" yes "` gives `False`.

OpenStone reads decimal numeric prefixes independent of locale and uses zero when text has no numeric prefix. Other out-of-range numeric conversions report an error. Additional numeric text edge cases and conversion order for other mixed types still need live checks in FlowStone.

The headless `applyArrayOperation` function evaluates two arrays of the same type or an array and a scalar. Add concatenates String Array entries. Int, Float, and Point Arrays support addition, subtraction, multiplication, and division. Point Array math operates separately on x and y. With two arrays, results use the longer operand's length. A missing element acts as an empty string, zero, or `(0, 0)`, depending on its type.

With an array and a scalar, the operator applies the scalar to every element and preserves left and right operand order. FlowStone shows this behavior with Int, Float, Point, and String arrays. The operator uses the operand types after any link conversions. For example, `[1, 2, 3] / 2` with an Int Array and a Float divisor yields the Float Array `[0.5, 1, 1.5]`, even if the value `2` came from an Int output.

Int Array division truncates toward zero and returns `0` at each zero divisor. Float Array division returns `+inf` at each zero divisor, including for zero and negative numerators. Point Arrays apply the same rule to each coordinate. For example, dividing `[6, -6, 0, 8]` by `[0, 0, 0, 2]` gives `[0, 0, 0, 4]` for Int Arrays and `[+inf, +inf, +inf, 4]` for Float Arrays.

In FlowStone build `3.0.9b2-5483-g56077f217`, dividing the Int Array `[-2147483647, -2147483648]` by `[-1, 1]` returned `[2147483647, -2147483648]`. Changing the divisor array to `[-1, -1]` crashed FlowStone. OpenStone wraps the overflowing quotient to `-2147483648` and returns `[2147483647, -2147483648]`.

Multiple value links into an array input use the same Add operation in connection order. Each link applies its conversion first and delivers its own trigger.

Trigger delivery follows connection order and completes each branch before taking the next. A cascade that exceeds the depth or delivery limit stops with an error. FlowStone’s exact feedback behavior still needs a compatibility check.
