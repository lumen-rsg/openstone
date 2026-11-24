# FlowStone schematic import

Open **File → Open**, choose a `.fsm`, then save the imported project as `.ostone`.
The editor treats an import as an unsaved project. It never uses the `.fsm` as its
save destination. Imports follow the existing unsaved-project and held-close
lifecycle. Parsing, compilation and type resolution happen on a detached graph;
a failed import leaves the current schematic and audio session intact.

This is an experimental subset, not full schematic compatibility. Unsupported
records stop the import with a component ID, byte offset and module path. They
are not silently replaced with empty modules or disconnected nodes.

## Supported records

- ZIP containers with a single `schematic.osm` member, stored or deflated, with
  central/local-header consistency and CRC verification. Other archive members
  are never extracted or opened.
- Binary format versions 154–177; nested modules, module names and grid positions.
- Module Input / Output (138 / 139), named and typed boundaries, connector order,
  internal and external wiring. DSP template types resolve across the complete
  module hierarchy before runtimes are installed. A connector's custom name in
  the common record header takes precedence over its payload's fallback name.
- Float (11), Int (29), String variants (88 / 117 / 118 / 119), Boolean (406),
  and Multiline String (236). Native Float connector ID **2** and Int ID **3**
  apply consistently to module boundaries, generic math and Ruby ports. Earlier
  importer builds reversed these IDs; reimport affected numeric schematics.
- Custom value-box connector layouts, multiline dimensions/bookmarks and optional
  module property-panel helper records are consumed with bounds checks. Values,
  text and graph wiring survive; skipped value-box styling is reported explicitly.
- Current MGUI / properties GUI (1191 / 1192), legacy MGUI / properties GUI
  (16 / 400), panel positions, frame sizes, anchors/reference sizes and enabled
  visibility. Footer-owned helper surfaces become actual MGUI/Props nodes.
- Static Wireless Output / Input (401 / 402) and Module Wireless Output (419),
  including typed property values and inherited enclosing-panel View routes.
- Color (403), Area (54), Area to Float (130), View Area (420), View Size (421),
  Line (62), Text (96), and Selector (137). View Size's Width/Height outputs stay
  live when the panel resizes. Selector retains its native first Index input,
  value order and spare input, including empty and single-value records.
- Color to ARGB (223), ARGB to Color (64), Color to AHSV (142), and AHSV to
  Color (143), with Integer channels in their native Alpha/Red/Green/Blue or
  Alpha/Hue/Saturation/Value order. All four are payload-free records.
- View Cache (609), also payload-free, with View and Clear inputs and a View
  output. Its drawing cache is rebuilt from the converted graph after reopening.
- Rect (218 / 219), Ellipse (220 / 221), Arc (109), and Pie (386). The two Rect
  IDs share a native factory, as do the two Ellipse IDs. All four shapes have
  only the common node header; their View/Area/Paint Tool connections, Rect
  Corner input, and Arc/Pie Start/Sweep inputs retain their native order.
- Round Rectangle (53 / 98), Redraw (132) and Redraw Area (131). These are also
  payload-free records. The two Round Rectangle IDs share a native factory.
  View/Area/Tool/Corner and View/Area/DoIt connections retain their native order.
  Linked Redraw **HiRes** inputs reject explicitly; see the scheduling limit below.
- Pen (214) and Font (215), including stored drawing values, property headers
  and optional connector layouts. Paint Tool connector ID 14 and Font ID 17
  work across module boundaries.
- String Format (216), Solid Brush (351), Linear Gradient Brush (352), Radial
  Gradient Brush (353), Hatch Brush (354) and Texture Brush (369). Stored
  formats, colors, geometry, wrap/focus settings and inline texture pixels survive
  conversion; connected inputs continue to override the stored defaults.
- Routed links retain their functional endpoints. Their short-coordinate routing
  points are bounded and consumed; OpenStone supplies the visible link geometry.
- Legacy Stream Add, Subtract, Multiply, Divide, Min and Max IDs; Delay1 (30),
  Pack (205), Unpack (206), Tooltip Help (170).
- Generic Add/Subtract/Multiply/Divide/Max/Min (1042–1047), Power (99 / 675),
  Log10 (240), and From Decibel (429). Typed connector lists and locked indices
  are validated; Power/Log10 use their version-170 layout change.
- Generic Equal, Less Than, Less Than or Equal, Greater Than, and Greater Than
  or Equal (1048–1052), plus their Trigger-output variants (622–626). Input
  types remain independent of the fixed Boolean/Trigger output. Float, Int,
  String, recognized Float Array connectors, and Automatic input types are
  supported; other native connector kinds retain the existing explicit limits.
- Generic Select (1057), Trigger Blocker (1056), and Trigger Switch (1060), with
  typed/Automatic Green value connectors and fixed Boolean control inputs.
  Trigger Switch also accepts a Trigger-only signal. The native connector order,
  type/lock lists and Select's saved condition-notification cache are retained.
- Index Selector (408), Counter (326), and After Load (258). Index Selector
  retains its selected index and consumes its native size/property-box layout;
  its List/Sort/Set and Index/Text ports use OpenStone's existing node menu.
  Counter retains its signed count and Inc/Reset connectors. After Load fires
  through the converted graph once per schematic open.
- Sample Rate (472), De-Zipper (535) and Float De-Zipper (594).
- Delay (77), Double Delay (1141), Sine (33), Double Sine (1148), and Pitch to
  Freq (35). These records contain only their common node header. Delay uses a
  Mono/MonoDouble signal and Green Integer sample count; Sine keeps its
  Freq/Phase/Boolean Sync connectors; Pitch to Freq keeps its Stream template.
- DSP Code (154) and Assembler (22): unchanged source, declared connectors, source
  string outputs and bounded skipping of embedded compiled caches.
- Ruby (1110): unchanged source, manually configured named/type connectors, and
  linked source strings. Serialized Ruby state is consumed as opaque bytes and
  reported as unrestored; it is never deserialized by the importer.
- MFC CString short/extended lengths, UTF-16 strings and Windows-1252 narrow text.

## Code-source connectors and caches

DSP/Assembler boxes have a native source-string **output at index zero**; their
signal inputs already start at zero. Ruby reserves index zero on **both sides**
for source strings. Imported nodes expose these as String ports named **Source**
following their data ports in OpenStone, preserving DSP signal indices and Ruby
callback indices. The distinction is important: treating DSP input zero as a
source connector incorrectly disconnects the first signal input in bundled Delay
and filter modules.

Source outputs can feed String boxes and pass through module boundaries. Ruby
source inputs resolve from linked String boxes, other code-source outputs and
module boundaries before the detached import is installed. They override an
empty or stale stored Ruby source. Cyclic bindings, multiple source writers and
unsupported source producers fail explicitly. Import parsing does not run Ruby.

Source ports, effective source and links are saved in `.ostone`. Changing data
port counts remaps the Source connections too. Editing a linked String updates a
Ruby node's source; a running node restarts through the existing Ruby host path.
Updates wait for Green dispatch to unwind before rebuilding ports or runtimes.

All native compiled-cache bytes are discarded. The reader walks the serialized
port tables, variable records (scalar, array and memory/reference kinds), four
stage blobs and symbol/relocation records with bounds and count checks. It never
loads cache pointers, allocates executable memory for them or invokes vendor
machine code. OpenStone compiles the source with its own compiler. Invalid source
still fails even if a vendor cache is present; malformed or truncated caches fail
without replacing the current project. Saving the conversion writes source and
links, without the vendor cache.

## Current limits

Other primitive record layouts, mixed-type generic math records, disabled Ruby
boxes, multi-view documents and Sequencer tracks are rejected explicitly.
Embedded image/font resource blobs are bounded and skipped with an import
warning. Synced modules become independent copies, also reported. Additional
connector kinds will be added as their layouts are verified. This pass does not
add `.fsm` writing, general `.hom` loading or native x86 execution.

## GUI and property conversion

GUI nodes preserve serialized frame dimensions and positions in grid units.
Property-panel footer records, previously skipped as metadata, now create real
surfaces. A root drawing surface is wrapped in a module because OpenStone panels
belong to modules. Native GUI-selection markers (including 254), property-view
footer flags, borderless state and inherited Views are consumed without shifting
subsequent records.

Legacy Mouse inputs retain their polarity; Borderless inputs pass through a
Boolean inverter to OpenStone's Border input. Width/Height outputs become live
View Size nodes and continue to follow resizing. Connected legacy X/Y positioning
inputs still reject explicitly. Translation adds ordinary nodes and links, so it
survives `.ostone` save/reopen without native-only runtime objects.

Current MGUI (1191) has View, Mouse and Borderless inputs; current properties GUI
(1192) has View and Mouse. Mouse remains positive-polarity. Current Borderless,
like its legacy counterpart, becomes a Boolean inverter feeding Border. Earlier
imports rejected these current Boolean controls as invalid connector indices.
Connector errors now identify both link endpoints and their native component IDs.

Names and types rebuild native wireless property routes through OpenStone's
existing ancestor/module scopes. Serialized route tables are cached paths, not
values: they are bounded and skipped. Enclosing-panel View receivers become
explicit module boundaries and persistent connections when that surface exists;
a missing enclosing surface is reported. Area, Color, Paint Tool, Font and String Format
connector IDs are recognized; Ruby Area/Font/String Format ports still reject
until the Ruby bridge supports them.

This is structural and functional GUI import, **not pixel-identical rendering**.
Frames retain their saved sizes, while inner drawing extents, border padding and
legacy size readouts use OpenStone's panel layout. This normalization is reported
as a warning. Custom drawing-coordinate rectangles also warn. Exact
drawing-area/border conversion, explicit cached View-source
selection and embedded-resource reconstruction remain future work.

Pen and Font records before version 159 have no property payload and retain
builder defaults. From version 159 onward, Pen stores width, a little-endian
ARGB word and a dash-style enum (solid, dash, dot, dash-dot, dash-dot-dot).
Version 167 adds a paint-tool discriminator: Pen accepts stroke kind 1;
the separate brush records accept kinds 2–6. Other Pen paint-tool kinds and
custom dash styles reject explicitly. Zero-width
hairline pens are retained. Font stores an MFC face string, float size and style
bits (bold, italic, underline, strikeout); version 171 adds an ordered fallback
list. All fallback names survive conversion and participate in font selection.
Embedded font bytes still require separate resource support.

Stored values become defaults for the existing Pen/Font builders. Connected
Color/Width/Style or Face/Size/Style/AltF inputs override the corresponding
defaults; disconnecting restores them. `.ostone` stores both the defaults and
connections, rather than flattening a connected property into its last value.
Available system fonts can change the visible typeface between platforms.

Round Rectangle defaults to a corner radius of **1** when Corner is unconnected;
an explicit zero gives square corners. Rect keeps its separate zero default.
Corner radii use OpenStone's existing geometry normalization and half-extent limit.

Redraw passes its View through and retains the previous drawing until DoIt fires.
Redraw Area repaints the mapped, clipped Area while retaining paint outside it;
full redraws replace the whole retained frame. Their triggers remain live after
conversion. Requests use OpenStone's repaint scheduler, rather than reproducing
FlowStone's 100 Hz automation limiter; the importer reports this normalization.
The native optional HiRes Boolean bypasses that limiter. A connection to it is
currently rejected explicitly because OpenStone has no equivalent input.

The native Selector clamps Index to the range of its value inputs, excluding
the spare. Conversion adds ordinary Int/Max/Min nodes to clamp the combined
Index before it reaches OpenStone's Selector. Multiple Index sources are summed
before clamping, and these adapter nodes survive saving. OpenStone's own
Selector behavior is unchanged. Earlier imports incorrectly treated Index as
the last native connector and lost custom module-boundary names; reimport an
affected `.fsm` to obtain the corrected routing and names.

Generic comparisons use the same four type/lock lists as generic math. Their
fixed Boolean or Trigger output is validated separately, so it cannot incorrectly
force String or numeric operands to Boolean. Mixed explicit operand types still
reject until their separate connector overrides can be preserved. Trigger-output
comparisons forward an incoming pulse only when the predicate is true; pulses
on either operand are checked, including repeated true results.

Imported comparison nodes retain FlowStone's paired-array rule: lengths must
match, and every corresponding element must satisfy the predicate. For example,
`[1,9] < [2,3]` is false even though its first pair is increasing. A length mismatch
is false for all five predicates. Two empty arrays satisfy all five predicates,
following the native helpers' empty-loop result. Float-array relational predicates
let unordered NaN pairs pass; equality still fails, and any failing ordered pair
still makes the result false. Scalar NaN comparisons are false for all five
predicates. Signed zeros compare equal in both scalar and array inputs.

This rule is stored as the optional `compareArrayElements` node flag in `.ostone`,
retained by module duplication and synchronization identity checks. Existing
OpenStone comparisons and older projects without the flag keep their existing
lexicographic array ordering. Invalid flag types or a flag on another primitive
reject before replacing the current project. These checks follow native helper
control flow and portable runtime tests, rather than new Windows output captures.

Select uses False, True, Condition inputs in that order. Its extra saved Boolean
is the last notified condition, not a default branch selection: reads always use
the live Condition input. An event on the active data input forwards; one on the
inactive input is suppressed. Condition events forward only when the Boolean
changes. The notification cache survives `.ostone` save/reopen, and non-Boolean
saved cache values reject before replacing the current project.

Trigger Blocker and Trigger Switch have no payload beyond the common generic
type/lock lists. Both expose the current upstream value when pulled. Blocker
never forwards events, including through chains of blockers. Switch forwards a
signal event only while its Open input is true; changing Open does not publish a
signal event. These imports also correct the shared runtime's former blocker
latch and repeated Select-condition notifications. Fixed Boolean inputs are
validated separately from generic value types, as comparison outputs already are.
Other native routing connector kinds and legacy aliases remain explicitly
unsupported until their mapping is verified.

Index Selector stores two floats for its node dimensions, a signed selected
index, a property-box flag and an optional connector-box layout. An index of -1
is the native no-selection sentinel; invalid/empty current lists use the
existing runtime's first-option/empty-list fallback. The selected index persists
in `.ostone`; selection by name, sorted options, menu choices and cancellation
remain functional after reopening. Native dimensions and property-box appearance
use OpenStone's layout, with an explicit import warning. Non-finite/non-positive
dimensions, selections below -1, invalid Booleans, oversized layouts and truncated
records reject before replacing the current project.

Counter stores one signed 32-bit count. Inc adds one using a 32-bit operation,
then resets a negative result to zero, including at signed overflow. Reset
publishes zero even when the count is already zero. Project saving now preserves
Counter's current count at the root and inside modules; copy/duplication keep
their existing reset policy, and diagnostic Trigger Counter wrapping is unchanged.
After Load has no extra payload. Loading restores Counter values and links before
dispatching After Load, so a saved startup counter advances once on each reopen.

The audio mappings use OpenStone's existing DSP primitives. Delay clamps the
sample count to at least one sample, and its double variant retains the extra
precision of a `1 + 2^-40` impulse. Sine retains normalized frequency, phase
offset/wrapping and rising-edge hard sync. Pitch to Freq produces
`880 * 2^((pitch - 69)/12) / sampleRate`, rather than Hz. Stream-template ports
specialize through their connected graph before rendering and after reopening.

Delay length edits retain OpenStone's existing history. This is reported as an
import warning: the verified native ID-77 control callback clears its line when
it updates the delay length. Saving/reopening starts a fresh DSP run; transient
oscillator phase, sync state and delay samples are not project data. The tests
check deterministic rerendering, not restoration of an interrupted audio tail.

Shape Color inputs convert directly to solid Paint Tools, retaining all ARGB
bits. Arc accepts a Pen stroke or a brush: a brush fills the segment bounded by
the arc and its chord, while Pie includes the ellipse's center. Both solid and
gradient brush routes support this distinction. Arc/Pie angles use zero at +X
and positive clockwise sweeps; this removes OpenStone's previous extra half-turn
in the panel renderer. Zero sweeps hide the drawing without breaking the View
route. Full sweeps fill the complete ellipse.

This angle convention follows the native drawing callbacks and the documented
[GDI+ DrawArc angles](https://learn.microsoft.com/en-us/windows/win32/api/gdiplusgraphics/nf-gdiplusgraphics-graphics-drawarc%28constpen_real_real_real_real_real_real%29)
and [Qt Quick PathAngleArc angles](https://doc.qt.io/qt-6/qml-qtquick-pathanglearc.html).

Color conversion links remain live rather than being flattened to stored colors.
ARGB channel values retain their low eight bits. AHSV uses Hue in degrees and
Alpha/Saturation/Value in the 0–255 range; the existing color runtime wraps signed
Hue values. The import fixtures check exact channel order, transparency, primary
colors, grayscale and six hue sectors. They exercise the existing portable color
runtime and are not new output captures from Windows FlowStone.

An imported View Cache captures downstream drawing on its first render. A sibling
redraw reuses that drawing; Clear invalidates it for the next external redraw.
A Redraw on the cached View route invalidates that cache. Panel resizing also
rebuilds it with the new bounds. `.ostone` retains nodes and connections, while
cached drawing buffers remain transient and rebuild on the first reopened render.

String Format uses the same version-159 property header, then three DWORDs:
GDI+ flags, horizontal alignment and vertical alignment. All nine supported
flags are retained: RTL, vertical text, no-fit-black-box, visible controls,
no font fallback, trailing spaces, no wrap, whole lines and no clip. Text still
uses Qt font metrics; this is not a claim of identical glyph-overhang layout.
The legacy Green String form `flags,horizontal,vertical` (for example,
`0,left,top`) converts to String Format through live links and survives saving.

Brush records call the common paint-tool serializer directly, without a property
header. From version 167 their payloads are a kind DWORD followed by:

| Kind | Payload |
| --- | --- |
| 0 | Empty cache; use the builder defaults |
| 2, solid | ARGB DWORD |
| 3, linear | Two ARGB DWORDs, four area floats, angle float, wrap DWORD |
| 4, radial | Path/polygon, surround colors, center color, focus and center points |
| 5, hatch | Foreground/background ARGB DWORDs and hatch-style DWORD |
| 6, texture | Length-prefixed image bytes and wrap DWORD |

Radial records retain custom polygons and typed paths, including cubic Beziers,
multiple open/closed figures, alternate/winding fill rules, and the original
close/marker/dash flag bytes. Surround colors retain their original ARGB values,
order and count, including partial arrays, repeats and fully transparent colors.
The center color, center point and focus scales are retained separately. Texture bytes are
decoded from memory with byte/pixel bounds, then saved as RGBA pixels; no source
file is needed when reopening. Legacy brush caches before version 167 use the
old stroke encoding: it is consumed and reported, and the brush rebuilds from
its inputs. Unknown kinds, incompatible brush kinds, bad enums, excessive counts,
invalid geometry and malformed images fail before replacing the open schematic.

`.ostone` stores the original radial boundary points/type bytes/fill rule and
surround array rather than reducing them to an ellipse/rectangle or one color.
Earlier converted files without these fields remain readable as standard radial
brushes. The same validation applies to native and converted path structures:
point/type counts are bounded, Beziers require complete triples, close flags may
only end segments, and native surround arrays cannot exceed the point count.

The portable renderer clips to the custom path and interpolates the boundary
colors toward the center. Curves are flattened for rasterization only; saved
control points are untouched. Marker/dash flags remain in the stored data and
do not change fill geometry. Color arrays shorter than the point array use their
last color for the remaining points in OpenStone's renderer. Exact Windows GDI+
pixel equivalence, including curve-color interpolation and focus behavior on
complex paths, has not been established by these headless checks.

Unconnected brush inputs retain imported paths/colors. A connected Area maps the
stored points to that area; Shape explicitly selects a standard ellipse or
rectangle; Col2 explicitly replaces the surround array with a uniform color
while retaining its count. Disconnecting restores the imported data.

A source program still needs to compile in OpenStone. The bundled Denormal
Remover (Bitmask) now loads unchanged: incoming XMM registers have explicit,
portable entry bindings with zero defaults. Its FlowStone scratch-register
contents are not inferred from the cache. See [entry policy and saved-trace
verification](flowstone-assembler-xmm-entry.md).

## Format evidence and validation

The container and binary layouts were checked against the bundled FlowStone
build 5483, using its x64 serializers and constructor dispatch table. The bundled
`system.fsm` is version 166 and imports as two nested nodes, including its Ruby
source. Bundled vendor binaries are not copied into repository fixtures.

Useful serializer addresses in that build (image base `0x140000000`):

| Record | Native function |
| --- | --- |
| Document loader / version | `0x14000e0e0` |
| Document body | `0x14002f600` |
| Module load / save | `0x140045900` / `0x140047e50` |
| Child graph load / save | `0x140038f10` / `0x140039d90` |
| Common primitive load / save | `0x1400519c0` / `0x140052950` |
| Link load / save | `0x14006e7e0` / `0x14006fd10` |
| Component factory table | `0x1400dec40` |
| DSP/Assembler state | `0x140080850` |
| Compiled-code cache | `0x14020aff0` |
| Variable reference records | `0x1402dfba0` |
| Ruby state | `0x140123960` |
| Value-box constructor / value state | `0x140306c30` / `0x140308100` |
| Custom connector-box layout | `0x140083ee0` |
| Multiline text / version-177 property header | `0x14036f1d0` / `0x1403bf950` |
| Generic math / versioned Power and Log10 | `0x1402e2470` / `0x1402e4860` |
| Boolean comparison factory / loader | `0x1400d2a10` / `0x1403036b0` |
| Boolean comparison predicate / Float Array helper | `0x1403036d0` / `0x1403024d0` |
| Trigger comparison factory / predicate | `0x1400d2a70` / `0x140304d40` |
| Trigger comparison Float Array helper | `0x140303e10` |
| Select loader / trigger callback | `0x140315460` / `0x1403153f0` |
| Trigger Blocker Float value pull | `0x14031d970` |
| Trigger Switch trigger callback | `0x14031e710` |
| Index Selector loader / shared size-selection loader | `0x140357450` / `0x14005ebb0` |
| Index Selector trigger callback | `0x140357380` |
| Counter loader / trigger callback | `0x140333e60` / `0x140333e20` |
| Delay / Double Delay factory | `0x1400dae90` / `0x1400daef0` |
| Sine / Double Sine factory | `0x1400db6d0` / `0x1400db730` |
| Pitch to Freq factory | `0x1400db9d0` |
| ID-77 delay-length control callback | `0x14038fc70` |
| Property helper panel records | `0x140072750` / `0x140072b80` |
| Embedded module GUI geometry | `0x140078e40` |
| Legacy MGUI callbacks / border polarity | `0x140369fa0` / `0x14036a130` |
| Wireless property receiver | `0x140091eb0` → `0x14007b620` |
| Wireless property sender / route cache | `0x14006b050` / `0x14006b190` |
| Color property | `0x14032f8d0` |
| Selector load / save / Index clamp | `0x140383fc0` / `0x140385a60` / `0x140385cc0` |
| Pen property / paint-tool value | `0x140378ab0` / `0x140086ad0` |
| Font property / font value | `0x1403460c0` / `0x14004f8a0` |
| String Format property | `0x14039c190` |
| Brush record / common paint-tool value | `0x140328a80` / `0x140086ad0` |
| Native radial path / inline texture image | `0x1400b73b0` / `0x1400b76c0` |
| Rect / Ellipse factory | `0x1400ddcc0` / `0x1400dda20` |
| Arc / Pie factory | `0x1400dd960` / `0x1400ddba0` |
| Shape-specific load/save (no payload) | `0x14000d180` |
| Rect / Ellipse drawing callbacks | `0x1403abc60` / `0x1403aa140` |
| Arc / Pie drawing callbacks | `0x1403a9a40` / `0x1403aaff0` |
| Round Rectangle factory / drawing callback | `0x1400ddd20` / `0x1403abf30` |
| Redraw / Redraw Area factories | `0x1400d5720` / `0x1400d5780` |
| Redraw / Redraw Area input callbacks | `0x14033de50` / `0x14033e190` |
| ARGB to Color / Color to ARGB factories | `0x1400d4b10` / `0x1400d4c90` |
| AHSV to Color / Color to AHSV factories | `0x1400d4ab0` / `0x1400d4c30` |
| Native AHSV construction / decomposition helpers | `0x1400b4ab0` / `0x1400b4c40` |
| View Cache factory / Clear callback | `0x1400dd900` / `0x1403a97e0` |
| Current MGUI Mouse / Borderless readers | `0x140077dd0` / `0x14007a130` |

`flowstone_file` builds independent binary fixtures rather than round-tripping an
OpenStone encoder. It covers archive integrity, version-dependent layouts,
nesting, Green values, Unicode, source connectors and save/reopen. Linked source
ports survive DSP and Ruby data-port changes. Cache fixtures cover all eight
variable kinds, all four array initializer modes, multiple caches and poisoned
machine-code blobs, plus invalid counts, unknown kinds, truncation and invalid
source. Corruption and compilation/type errors check atomic replacement.
Numeric fixtures distinguish native Integer and Float IDs rather than merely
accepting a successful parse. They cover Green math, typed module boundaries,
single/double stream math, layout flags, optional clip/drawing rectangles,
bookmarks, excessive counts, bad indices/types and truncated metadata.
Cross-version fixtures exercise decoded layouts; native files from every
supported version have not yet been collected.
Pen/Font fixtures cover legacy payload-free records, the version-159 property
header, version-167 paint-tool tag, version-171 fallbacks, all standard dash
styles, style bits, connected overrides, typed boundaries and invalid payloads.

For reproducible local vendor checks, configure with:

```
cmake -S . -B build -DOPENSTONE_FLOWSTONE_ROOT=/path/to/flowstone-5483
cmake --build build
ctest --test-dir build -R 'flowstone_(file|bundled_import)' --output-on-failure
```

The optional `flowstone_bundled_import` test pins SHA-256 hashes and exact
module offsets for **17 bundled modules**: Delay, Clip, Phase Invert, all three
Denormal Removers, 0-1 To Hz, Hz To 0-1, DeZip, and all eight single/double
DF1/DF2/TDF1/TDF2 Stream Biquads. It wraps unchanged `.hom` module records in
authored `.fsm` containers. These are generated wrappers, not native `.fsm`
saves or newly captured FlowStone output traces.

The same test imports six unchanged Pen/Font records from bundled GUI modules:
LED-Indicator Pen, Label Pen and Font (including its custom connector layout),
Stream Power Font, Text View Font and the version-177 Edit Box Pen. Each source
module is hash-pinned. Records are isolated in authored wrappers because the
complete modules still contain unsupported records or source producers. Tests
connect them to Line/Text drawing nodes, verify the resulting descriptors, then
save to disk, reopen and repeat the drawing checks. These checks establish
record/value compatibility, not a pixel comparison against Windows FlowStone.
The complete separator GUI submodule from LED-Indicator is also wrapped
unchanged: its eleven nodes and fourteen links retain the native Color/Width
connections to Pen, with two explicit panel-control nodes added by conversion.
An edited width is checked again after saving and reopening the schematic.

String Format checks pin unchanged property records from Label and Text State,
including their connector layouts, and verify centered Text drawing before and
after a disk round-trip. The complete Label submodule from Stream Power also
loads unchanged, including its nested Inflate graph and original legacy String
Format link. A bench edits that string, draws text and a brush driven by the
native Color property, then repeats the checks after save/reopen.

Independent serializer fixtures cover all five brush records, both radial
geometry encodings, texture alpha/pixels, empty/legacy caches, connected
overrides and invalid data across versions 154–177. The shipped GUI modules
used here provide native String Format records and property wiring, not saved
examples of every brush cache; those brush payloads are authored from the
verified native serializers. These tests do not claim a Windows-rendered pixel
comparison.

Custom radial fixtures cover triangles, concave polygons, cubic control points,
multiple figures, open/close/marker/dash flags, both fill rules and partial/full
multicolor arrays in native versions 167, 170 and 177. Tests compare every
stored coordinate, type byte and ARGB color before and after a disk save/reopen,
verify the drawing commands, and compare the rendered images across reopening.
Separate rendering checks distinguish triangle/curve boundaries and winding
holes, and check that distinct boundary colors contribute to the gradient.
Malformed native/converted paths and oversized render requests also have checks.

The byte flags and point/color association follow the documented
[GDI+ path point types](https://learn.microsoft.com/en-us/windows/win32/api/gdiplusenums/ne-gdiplusenums-pathpointtype)
and [surround colors](https://learn.microsoft.com/en-us/windows/win32/api/gdipluspath/nf-gdipluspath-pathgradientbrush-setsurroundcolors).

The tests import each hierarchy, preserve linked source readouts where code is
present, check every impulse/step/noise sample at buffer sizes 1, 7, 64 and 257,
then save to disk, reopen, compare the project and repeat the rendering checks.
The frequency conversions use a 3200 Hz sample rate; DeZip preserves its stored
40 ms value and produces a 128-sample ramp. The noise remover checks its original
Assembler integer recurrence, float conversion, scaling and -180 dB control;
the truncating remover rejects positive/negative inputs below its stored threshold.
These expected outputs are derived from the source/bench, not new Windows traces.
The checked LLVM build uses LLVM ORC for these complete imported DSP graphs.

A broader [library import report](flowstone-fsm-bundled-import.json) records
**24 imports/save-reopens out of 124 matching module bodies** in build 5483.
Another 21 library entries lack the exact module-body/name signature needed by
this wrapper tool and are listed separately. Successful entries in this report
have not all received a signal-response check. Every rejected entry retains its
first actual blocker; unsupported GUI/property primitives remain the largest
group. The GUI pass moves those failures deeper into Ruby GUI types, drawing
resources and other unimplemented records. The audio mapping pass additionally
imports the complete FM De-modulator. No Assembler compiler change was required.

Regenerate that diagnostic report with:

```
python3 tests/tools/FlowStoneBundledImportTest.py \
  --flowstone-root /path/to/flowstone-5483 \
  --probe build/openstone_flowstone_file_tests --output /tmp/fsm-import-scan \
  --scan-library --report /tmp/fsm-import-report.json
```

The same optional test also pins **three unchanged MGUI component records** from
Amp, Label and LED-Indicator. It puts those records in authored containers and
checks their independently recorded geometry plus save/reopen. It does not claim
that the complete source modules load yet.

Five unchanged shape records are pinned from Graph Shaper (Rect 218), Float
Graph Recorder (Rect 219), Group Adornment (Ellipse 220), Parametric EQ (Ellipse
221), and Multi Sample (Arc 109). Each produces an actual drawing command and
survives a disk save/reopen. Complete BG submodules from Float Graph Recorder
and Graph Shaper also retain their native View Area and Color routes, footer
properties surfaces, and live color changes after conversion. Source module
hashes and exact record/body offsets are checked before wrapping them.

Independent cross-version fixtures cover all six shape IDs, ordered View chains,
Color-to-brush conversion, optional corners, signed/live angles, zero sweeps,
pen/gradient Arc inputs and save/reopen. Headless path checks distinguish an Arc
segment from a Pie sector and verify the angle origin and sweep direction.
A verified bundled Pie record has not been collected; its coverage uses an
authored record following its native no-payload serializer and five-port layout.
No new Windows-rendered pixel comparison is implied.

Additional hash-pinned tests import both unchanged Round Rectangle aliases from
Group Adornment and a Redraw Area record from Multi Sample. The complete Label
submodule from Group Adornment retains its rounded rectangles, ellipse, Text,
native property colors, and View Size-to-Area links. Tests edit the color and
resize its panel, then repeat the drawing checks after disk save/reopen.

The complete Arc submodule from Multi Sample retains its native Inflate and
Sweep Pen modules, both Selectors, same-name wireless properties of different
types, and wireless-to-Redraw trigger. Tests drive Size/Start/Sweep, verify exact
geometry, pen width/color/style and repaint requests, then save/reopen and verify
again. This tests native record conversion and drawing commands, not Windows
pixels or repaint timing. Authored cross-version fixtures cover both rounded
aliases, their default/zero/connected corners, full and partial redraw clipping,
live View Size outputs, named trigger boundaries and disk round-trips. Invalid
connector indices, truncated records and unsupported linked HiRes options verify
atomic rejection. Selector tests include zero/one/two-value records, routed
links, and clamping a sum of Index sources.

The complete bundled Group Adornment now imports and reopens. Hash-pinned tests
preserve all its original nodes and links, verify the Alpha submodule's split and
combine wiring, change the Back color and Alpha value, and check both its actual
background drawing and its separate nested Label drawing surface. The complete
Colour submodule from Stream Power provides a second native ARGB conversion
example. Tests verify that replacing alpha preserves RGB, including byte wrapping,
then save/reopen and edit it again.

A hash-pinned View Cache record from Float Graph Recorder is wrapped unchanged
and connected to a headless drawing bench. It verifies capture, reuse, Clear and
disk save/reopen. The complete graph-display modules still encounter other
unsupported records or Ruby bindings; this isolated check does not claim their
full compatibility. No verified AHSV primitive record was found among these
bundled fixtures. AHSV coverage uses authored records based on the native
no-payload serializers, connector callbacks and documented channel ranges.

Cross-version authored fixtures additionally exercise channel conversion and
drawing links, signed Hue wrapping, live input edits, cache reuse during sibling
redraws, invalidation through Clear and an internal Redraw, resizing, current MGUI
Mouse/Borderless controls, properties-GUI Mouse control, and save/reopen. Malformed
connector indices and truncated new records check atomic rejection. The GUI/color
pass imported and reopened **23 of 124** matching wrapped module bodies.

Five additional hash-pinned, unchanged comparison records cover String Equal
from Moog Filter, Float Less Than and Less Than or Equal from Multi Sample, and
Float Greater Than and Greater Than or Equal from Hysteresis Gate. Each is wired
to live inputs and verified before and after disk save/reopen. The complete Preset
Name submodule from Moog Filter also loads unchanged: its String comparison,
Selector, same-name wireless prefix route and original `-` separator generate
`Cutoff`, `Patch-Cutoff`, and `Patch-Resonance` as inputs change. Reopening and
clearing the prefix restores `Resonance`.

Independent fixtures cover all ten comparison IDs across versions 154, 159, 166,
171 and 177, typed and Automatic inputs, scalar and paired-array results, both
input trigger routes, false/true/repeated predicate delivery, unequal array
lengths, crossing element orders, empty arrays, scalar and array NaNs, and signed zero. The native
array rule survives disk save/reopen and duplication; fixtures without the saved
flag verify the earlier OpenStone behavior. Invalid output types, mixed operand
types, connector counts, locked indices, truncated records and malformed saved
modes check atomic rejection. No unchanged bundled Trigger-comparison record has
been verified yet; those fixtures follow its native common serializer and
conditional trigger callback. The comparison pass reported **23/124**; eleven
modules passed their comparison blocker and reached later unsupported records.

Four hash-pinned native routing records cover Float Trigger Blocker from Step
LFO, Trigger-only Trigger Switch from Poly Stream Record Samples, and Float and
String Select from Reverb. Tests attach live data and explicit trigger sources,
check active/inactive branches, closed/open event gates and repeated condition
events, then repeat after disk save/reopen. The complete 13-node Left/Right split
submodule from Reverb imports unchanged: its two Selects, comparisons and math
produce the expected stereo gains at balance values 0, 0.25, 0.5, 0.75 and 1,
before and after saving/reopening the converted schematic. The test also checks
that output order follows native connector positions rather than child order.

Authored routing records cover versions 154, 159, 166, 171 and 177; Automatic,
Float, Int, String, Boolean and recognized Float Array types; Trigger-only
Switches; both saved Select cache values; malformed fixed-control and output
types, lock indices, Boolean state and truncated records. A malformed saved
Select cache also checks atomic rejection. Core tests explicitly pulse an
unchanged Select condition and pull through two chained blockers. These tests
follow the bundled native loader and callback control flow; they do not add new
Windows output captures.

The routing pass reported **23/124**. Step LFO passed its Trigger Blockers and
reached Mouse (65), Poly Stream Record Samples passed its Trigger Switch and
reached Index Selector (408), and Reverb passed its Selects and reached Bitmap
File (105).

Five additional hash-pinned native records cover Index Selector from Fast Power,
Bitmap and Float Graph Recorder (versions 159, 160 and 177), Counter from
Hysteresis Gate, and After Load from Ticker - Custom. Each exercises live runtime
behavior before and after disk save/reopen. The two unchanged After Load and
Counter records are also wired together in an authored test wrapper: import
produces count 1, saving retains 1, and reopening restores 1 before the single
startup event advances it to 2. This is an integration fixture assembled from
bundled records, not an unchanged complete vendor module.

Authored records cover versions 154, 159, 166, 171 and 177; both property-box flags;
optional connector layouts; selected indices -1, 0, 1 and 42; selection by name,
sorted menus and cancellation; negative and extreme signed Counter values;
repeated resets; one After Load event per open; and atomic rejection of malformed
new payloads. Root/nested Counter persistence and the unchanged duplication reset
policy are checked separately. Native evidence verifies record layouts and the
Counter overflow branch; the runtime tests do not establish new Windows UI or
output parity.

The Index Selector/Counter/After Load pass reported **23/124**. Thirteen modules
passed their former blocker. Fast Power reached its known multiple-value-source
limitation; Ticker - Custom reached multiple linked code sources.

Five hash-pinned, unchanged audio records cover Delay from Echo Delay, Sine from
Multi Osc and Multi LFO, and Pitch to Freq from State Variable Filter and Moog
Filter. Each renders against expected signals across buffer sizes, saves to disk,
reopens and repeats the checks. The complete unchanged Echo submodule from Echo
Delay exercises its nested sample-rate/seconds conversion, dry/wet sum and
feedback loop with positive and negative feedback at two sample rates. The
complete unchanged FM De-modulator is a permanent regression fixture too: it
renders the expected `2 * x[n] * x[n-delay]` product for several delay lengths,
before and after disk save/reopen.

Authored records cover all five audio IDs across versions 154, 159, 166, 171 and
177, including exact double-precision impulses, phase offsets containing `2^-40`,
sync edges across buffer boundaries, negative/zero delay counts, current sample
rate in pitch conversion, and atomic rejection of invalid IDs/ports or truncated
records. No unchanged bundled Double Delay or Double Sine record is claimed;
those fixtures follow the verified native factory/vtable and empty serializer.
Signal checks use OpenStone's existing DSP behavior and do not establish new
Windows sample-for-sample parity with native oscillators or delay-length edits.

The refreshed complete-module scan imports and reopens **24/124**. Ten modules
pass their former audio-primitive blockers, including the now-supported FM
De-modulator. Others report their next unsupported record in the JSON inventory.

Independent GUI fixtures exercise all four panel IDs across versions 154, 155,
170 and 177; actual text drawing commands; anchor metadata; live legacy size and
border/mouse routes; inherited parent Views; property value propagation before
and after save/reopen; Selector ordering; routed links; and atomic rejection of
invalid coordinates and truncated metadata. These checks are headless and do not
establish visual parity with a running FlowStone instance.

The test executable also accepts arbitrary native `.fsm` paths for local
import/save/reopen checks:

```
openstone_flowstone_file_tests /path/to/example.fsm
```

Further work includes additional primitive serializers and custom panel/resource
reconstruction.
