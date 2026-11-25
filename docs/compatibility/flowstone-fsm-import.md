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
- Bitmap (105), with embedded raster pixels or references into the document's
  image table. Bitmap/Width/Height outputs and Path/Load/Clear wiring survive
  conversion. A connected native clipboard-Paste input rejects explicitly.
- File Dialog (103), with its native connector order, default-Open policy and
  filter syntax. Selection and cancellation use OpenStone's host-dialog service.
- Button / Trigger Button (17), mapped to OpenStone's Trigger control, with its
  caption, schematic position and outgoing trigger links.
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
- Boolean Not (256), with one fixed Boolean input and output, mapped to Not.
  Its record contains only the common header; values are pulled from its input.
- Index Selector (408), Counter (326), After Load (258), and After Duplicate (551). Index Selector
  retains its selected index and consumes its native size/property-box layout;
  its List/Sort/Set and Index/Text ports use OpenStone's existing node menu.
  Counter retains its signed count and Inc/Reset connectors. After Load fires
  through the converted graph once per schematic open. After Duplicate fires
  only in newly copied nodes/modules, after their internal links are restored.
- Drop List (237), including its signed selected index and native
  View/X/Y/List/Open/Check/Style inputs. The Text/Index/Number of Options/Canceled
  outputs keep their original order. Native selection and menu-index policies
  survive conversion and saving as `.ostone`.
- Multiplexer (149), including populated output count, data type, native Index
  clamping and the final spare output. Green values, triggers, Views and supported
  DSP connector families use the existing routing runtimes. Inactive DSP routes
  disconnect their upstream code.
- Last Switch (1054), with its generic connector/type/lock lists and saved active
  input. Homogeneous supported Green types retain the selected input through
  conversion and saving as `.ostone`.
- Generic Sample and Hold (1053), with Value/Sample inputs, Held output and its
  cached Float, Int, Boolean or String value. Redraw Limiter (478) maps to the
  existing redraw gate; complete bundled `RedrawLimiter` modules are supported.
- MIDI Control Change (166), with MIDI/CC inputs and Integer Value/Trigger
  Changed outputs. The record has no payload beyond its common node header.
  Its selected controller comes from the live CC input; incoming MIDI state is
  transient and starts at zero after reopening.
- Preset Float (189), including its complete program bank, current Float, cached
  lock and floating-point Order. Native Preset connector ID 27 works through
  module boundaries and wireless links. The ten inputs and three outputs keep
  their native order. Automation bindings and recorded automation lanes still
  reject explicitly; an empty lane is consumed structurally.
- Sample Rate (472), De-Zipper (535) and Float De-Zipper (594).
- Delay (77), Double Delay (1141), Sine (33), Double Sine (1148), and Pitch to
  Freq (35). These records contain only their common node header. Delay uses a
  Mono/MonoDouble signal and Green Integer sample count; Sine keeps its
  Freq/Phase/Boolean Sync connectors; Pitch to Freq keeps its Stream template.
- Sawtooth (27 / 72 / 157 / 196), Double Sawtooth (1149), Triangle (165),
  Double Triangle (1150), Ramp (32), Impulse (36), and Glide (533). These have
  no primitive-specific payload. Sawtooth/Triangle retain Freq/Phase/Boolean
  Sync; Ramp retains Freq/Boolean Sync; Impulse has no inputs; Glide retains
  its Stream target and Stream length in samples. Generic Stream families
  continue to specialize through the existing runtime.
- DSP Code (154) and Assembler (22): unchanged source, declared connectors, source
  string outputs and bounded skipping of embedded compiled caches.
- Ruby (1110): unchanged source, manually configured named/type connectors, and
  zero-based native data links. Serialized Ruby state is consumed as opaque bytes and
  reported as unrestored; it is never deserialized by the importer.
- MFC CString short/extended lengths, UTF-16 strings and Windows-1252 narrow text.

## Preset Float conversion

Preset (189) is the native Preset Float primitive. After the common header,
versions 154–177 store a signed 32-bit program count, that many Float32 values,
a byte lock flag, the current Float32 value, a Float32 ordering key, and a byte
recording cache. Counts are limited to 4096 programs. Empty banks preserve their
inactive, zero-output state until a manager supplies a program. Truncated records
or invalid Booleans fail before replacing the current project. The recording cache
is refreshed from the live Rec input, as in the native initialization callback.
The ordering key is **not** the default value: Def remains input 7, and Order is
input 9. Native -1 ordering keys and fractional live Order inputs are retained.

The bank and current value are independent. Import does not substitute program
zero for the current value or clamp native Float values to 0–1. A converted node
retains this policy in `.ostone`; newly created OpenStone Preset Float nodes keep
their existing normalized-value behavior. Edits to a native node with no manager
store program zero unless its saved lock is enabled. A connected OpenStone
Preset Manager supplies program selection and locking. Before/Val/After retain
their native indices 1/0/2 and edits notify in that order, including equal values.
Empty native banks keep their cached value but output zero and ignore edits until
a manager supplies an active program.

Version 162 adds automation metadata. Version 163 replaces the legacy enabled
integer with four byte flags; version 164 adds a binding ID and cached lane
index. The importer accepts only an unbound, empty lane, validates its flags and
consumes its headers. Actual bindings or recording records stop with a specific
error rather than silently discarding automation. Preset Manager (190), other
Preset primitive record layouts, and complete native automation playback are
outside this pass.

## MIDI Control Change conversion

MIDI CC (166) uses the fixed connector order MIDI input 0, Integer CC input 1,
Integer Value output 0, and Changed trigger output 1. Its specific load/save
functions are empty. With an empty label, the entire native node is seven bytes:
16-bit component ID, two 16-bit grid coordinates, and an empty MFC string.
There is no saved controller number or cached incoming value in this record.
The controller number comes from its connected Green input (default 0); the last
received value starts at 0 and is not serialized into `.ostone`.

A valid matching CC message updates Value. If the number changes, Value sends a
notification first; Changed then fires for every matching packet, including a
repeated value or an initial zero. All 16 MIDI channels are accepted; this
primitive has no channel selector. Nonmatching status/controller messages are
ignored. Changing the CC input neither clears the held value nor invents an
event; negative/out-of-range controller selections match no valid MIDI packet.
Existing standard MIDI packet validation remains in place.

The shared OpenStone MIDI CC runtime previously updated its cached value without
notifying output 0. That prevented a bundled module's normal Value-to-Divide
connection from publishing its normalized output. It now follows the verified
Value-before-Changed order. Stream/Poly MIDI CC behavior is unchanged.

## Bitmap conversion

Bitmap (105), native class `BitmapFile`, has three outputs: Bitmap 0, Integer
Width 1 and Integer Height 2. Its inputs are String Path 0, Load trigger 1,
clipboard-Paste trigger 2 and Clear trigger 3. OpenStone's Bitmap has no Paste
socket yet, so native Clear links map to input 2; connected Paste links fail
explicitly rather than being mistaken for Clear. Path changes are passive.
Only Load requests the current path through the host bitmap loader, while Clear
discards the image and publishes the empty image and zero dimensions.

After the common component header, versions 154–159 begin directly with the
image payload. Versions 160–177 first store the standard property-enabled byte,
property-present byte and optional connector-box layout. The image payload is:

| Field | Meaning |
| --- | --- |
| Byte Boolean | 0 for an inline image; 1 for a document image-table reference |
| Inline: unsigned DWORD byte count and bytes | Encoded raster, normally PNG; a zero length means no image |
| Reference: signed DWORD index | Zero-based image-table entry; -1 means no image |

The document image table is a DWORD count followed by length-prefixed raster
blobs. Referenced images are decoded once on demand; invalid indices, invalid
images and truncated blobs fail before replacing the current schematic. Raster
dimensions are checked before allocating pixels, with a combined 128 MiB decoded
pixel budget across resources, inline Bitmaps and texture brushes. Decoding
produces top-down straight-alpha RGBA pixels. Converted `.ostone` projects embed
each node's pixels and do not depend on the original image path or native
resource table. Native image-pool object identity is not retained in project
serialization.

The bundled tests establish exact raster preservation, not visual parity with
Windows rendering or support for the separate Draw Bitmap (106) record.

## File Dialog conversion

File Dialog (103) has no primitive-specific load/save payload. With an empty
label its complete record is seven bytes: component ID, grid X/Y and an empty
MFC string. Its last selected path is transient, rather than stored in the
native file. The input/output order is:

| Direction | Index | Type | Name |
| --- | --- | --- | --- |
| Input | 0 | Trigger | Open |
| Input | 1 | String | Filter |
| Input | 2 | Int | Type |
| Input | 3 | String | Title |
| Input | 4 | String | Folder |
| Input | 5 | String | Name |
| Input | 6 | String | Ext |
| Output | 0 | String | Path |
| Output | 1 | Trigger | OK |
| Output | 2 | Trigger | Cancel |

Imported dialogs retain their native behavior through the `flowStoneFileDialog`
project flag. An unconnected Type defaults to 1 (Open); an explicit zero selects
Save and any nonzero integer selects Open. Unconnected Title defaults to
`Choose File`. Empty filters select all files. Native filter shorthand, including
`.png`, `*.png|*.jpg|`, and `Images|png|`, expands to label/pattern pairs for the
host picker. Full label/pattern pairs and semicolon-separated patterns remain
supported. Newly created OpenStone dialogs retain their existing defaults.

Only Open triggers a dialog. Its request snapshots the inputs and is routed back
to the owning node, including inside nested modules. Successful completion
updates and notifies Path before firing OK. Cancel fires only Cancel and keeps
the last successful path. Dialog requests and selected paths are not serialized:
saving while a dialog is open does not reopen it or emit a result on project
load. Stale response tokens cannot complete a later request.

The UI uses OpenStone's host file picker. FlowStone's Windows-specific folder,
filename and inferred-extension suggestions are not emulated; explicit Folder,
Name and Ext inputs are forwarded to the host. Headless tests simulate picker
responses and do not establish visual parity with a running Windows dialog.

## Button conversion

Component 17 is native class `ButtonTrigger`, titled Trigger Button. It has no
inputs and one Trigger output at index 0. It maps to OpenStone's existing Trigger
node: activating its Fire button emits one pulse, including through module
boundaries and to multiple receivers. Importing, connecting, saving and reopening
the node do not activate it. Its caption is the common node-header label; its
schematic position is retained.

The component-specific record contains the standard property-editor header:
one Boolean flag, a Boolean indicating an optional connector-box layout, and
that layout when present. The layout has a DWORD box count, 24 bytes per box,
version-157+ box flags/optional anchor reference, then a DWORD order count and
WORD order entries. There is no extra pressed state, value or per-button cache
after this header. The native Button loader delegates directly to the property
base loader for all supported schematic versions.

As with imported value-box property layouts, FlowStone's custom property-box
appearance is consumed with bounds checks and reported as a conversion limit.
Converted buttons use OpenStone's existing editor control; this pass does not
recreate the native property-panel widget or its Windows styling.

## After Duplicate conversion

After Duplicate (551) has no inputs and one Trigger output at index 0. Its
primitive-specific load/save functions are empty: with an empty caption the
complete record is seven bytes (component ID, grid coordinates and empty MFC
string). No last-event state or native pointer is imported.

It maps to OpenStone's existing After Duplicate lifecycle primitive. Importing,
opening, rebuilding runtime wiring, copying to the clipboard and reopening a
saved project do not emit its event. Duplicating or pasting a module emits one
event only inside the new copy, recursively including nested modules, after
the copied links exist. The source and its external receivers remain quiet.
Duplicating an already duplicated module creates another single event in the
new copy. After Load remains a separate event and runs once on project open.

The native Bitmap loader uses both After Load and After Duplicate to trigger
its Bitmap-typed module output. Those event-only links republish the current
image; they do not load an image file. Copying retains independent Bitmap
storage, so a later file selection in the copy updates only that copy's image
and its Width/Height monitors. Project serialization uses the ordinary
After Duplicate primitive kind, with no additional compatibility flag.

## Redraw Limiter conversion

The bundled `RedrawLimiter` is a four-node module, comprising a Float input,
Float output, generic Sample and Hold (1053), and Redraw Limiter (478). Its
original four links are retained, including their order: the Float input
notifies the redraw gate, whose accepted trigger samples the current value.
Input changes between accepted events leave the held output unchanged.

Record 478 is payload-free, with one Trigger input and one Trigger output.
Record 1053 begins with the common generic connector/type/lock lists, followed
by a cache determined by its Value type: a raw DWORD for Float or signed Int,
a Boolean byte, or an MFC CString. Unresolved Value ports have no cache bytes
and can specialize later. Mixed connector types, invalid fixed Sample types,
bad locks and unsupported cached-value types fail explicitly. Other native
cache formats, including arrays, Area/Color and resource handles, are outside
this pass.

Imported holds carry the `flowStoneSampleAndHold` project flag, preserving their
held value in project files and module copies without inventing a Sample event.
Sampling updates the value and notifies on every Sample trigger, including
repeated equal values; the Value input alone does neither. Ordinary OpenStone
holds retain their existing transient-cache policy. Saved native caches with a
conflicting declared type reject before replacing the current project.

The native 478 description limits triggers to the redraw rate and suppresses
them when redraws are suspended. Its implementation uses the Windows document
message loop. Imported gates use OpenStone's existing clock-based policy: pass
the first event and then at most one per 10 ms, suppress events during preset
redraw suspension or while a plug-in editor is hidden, and resume on a new
event. There is no deferred catch-up event. This verifies OpenStone's scheduler
behavior, not exact Windows message-loop coalescing or display-rate parity.

## Boolean Not conversion

Component **256** (`BoolNot`, native title `Not`) contains only the common
primitive header: little-endian component ID, grid X/Y and an MFC CString label.
Its specific load/save vtable slots both point to the empty function
`0x14000d180`; there are no generic connector lists, property layout or cached
value bytes. The unchanged seven-byte empty-label records in Multi LFO,
Detuner and Amp end immediately before their containing module's link count.

The importer maps this record to OpenStone's fixed-Boolean Not primitive. An
unconnected input reads false, so the output reads true. Reading a value does
not emit a notification; each incoming notification forwards one output event,
including when the Boolean value repeats. No additional project flag or runtime
implementation is required. Schematic position, label, typed module boundaries
and outgoing links survive conversion and disk save/reopen.

## Native routing conversion

**Multiplexer is component 149. Component 1054 is Last Switch.** An earlier
inventory note incorrectly named 1054 as Multiplexer; the class name, title
resource, factory and serializer now establish the distinction.

Multiplexer stores two signed 32-bit fields after its common header: the number
of populated outputs (excluding the final spare), then the data connector type.
The importer bounds counts to 4096, retains unconnected populated outputs, and
keeps the fixed Index/Data inputs at 0/1. Index clamps to 0 through the last
populated output; an empty list clamps to its only spare at 0. Adding a new wired
output expands the routing range. On an Index change, only the newly selected
output receives a Green notification; an incoming data pulse reaches that output
alone. Repeated Index changes within the same clamped branch are silent. Trigger
routes also pulse on an actual selection change, matching the native callback.
These policies are stored in `.ostone` and apply only to imported nodes.

The DSP compiler and live audio control updates use that same clamped selection.
A route without a connected consumer does not schedule its upstream DSP code.
OpenStone-created Multiplexers retain their existing invalid-index/disconnect
and old/new-output notification policies.

Last Switch stores the common generic input/output type lists and locked port
indices, followed by a byte selecting input 0 or 1. Supported records have two
matching Green inputs and one matching output; unsupported or mixed native
connector forms fail explicitly. An unconnected selected input reads its typed
default, rather than falling back to the other input. The saved selection and
subsequent changes survive project snapshots and disk save/reopen.

## Code-source connectors and caches

DSP/Assembler boxes have a native source-string **output at index zero**; their
signal inputs already start at zero. Their source outputs are remapped to a
String port named **Source** following the data outputs in OpenStone. These
outputs can feed String boxes and pass through module boundaries. Treating DSP
input zero as source incorrectly disconnects the first signal input in bundled
Delay and filter modules.

Ruby's saved input/output lists contain **data connectors starting at zero**,
including View, Float, Int, String and MIDI. The stored Ruby source is separate
from these lists. The importer preserves all native Ruby link indices and
validates them against the declared data-port counts. It never evaluates Ruby
or follows data input zero as code during parsing. Earlier importer builds
incorrectly treated native Ruby input/output zero as implicit source sockets;
reimport affected modules from their original `.fsm` files.

OpenStone adds its own editable String **Source** ports after the Ruby data
connectors. They are not extra sockets that native links can address. After
conversion, an explicitly connected OpenStone Source input can still update the
program; a running node restarts through the existing Ruby host path. Updates
wait for Green dispatch to unwind before rebuilding ports or runtimes. Source
ports and links survive `.ostone` save/reopen and follow data-port resizing.

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
Embedded font resource blobs are bounded and skipped with an import warning;
Bitmap image-table references are supported as described above. Synced modules
become independent copies, also reported. Additional
connector kinds will be added as their layouts are verified. This pass does not
add `.fsm` writing, general `.hom` loading or native x86 execution.

Some delay-free DSP feedback cycles still fail graph validation. The complete
native Multi Osc oscillator bank, for example, contains a Sine/Multiply phase
feedback loop that is rejected after its generator records decode successfully.
The importer does not remove or rewrite that loop to force an import.

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

Drop List's primitive-specific payload is exactly one little-endian signed
32-bit selected index, with no property-layout or menu-string payload. Its option
String and other settings arrive through normal links. The saved index is kept
as the node's optional Integer `value` in `.ostone`; a missing value retains
OpenStone's existing DropList behavior. Invalid saved value types reject before
replacing the current project. No popup request or platform menu handle is saved.

Imported Drop Lists expose their saved index even when it is outside the current
list. Text is resolved by native item ID against the current List. List events
reset the index to a valid connected Check value or zero; valid Check events
change it silently, and invalid Check events leave it alone. Neither event
publishes Text, Index or Count pulses. Opening a valid popup makes the same
silent Check/zero reset. Choosing an item publishes Text then Index; dismissing
it publishes only Canceled. The Open reset remains visible after cancellation.
If the List changes while OpenStone's asynchronous popup is pending, a returned
ID is checked against the current List before publishing a selection.

Native menu parsing skips empty tokens, uses `<<` to enter a submenu and `>>` to
leave it, and turns the preceding label into a submenu header. Headers have ID
`-1` and do not consume a selectable index; separators reserve an ID but cannot
be selected. Number of Options counts native records, including headers and
separators. A raw saved or connected Check value of `-1` can therefore resolve
to the first header's text. OpenStone-created DropList nodes keep their earlier
contiguous selectable indices and count behavior. Popup rendering uses
OpenStone's menu UI; Windows modal timing and pixel layout are not reproduced.

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
| Ruby declared input/output indices / input callback | `0x140123d20`–`0x140123f05` / `0x140123300` |
| Value-box constructor / value state | `0x140306c30` / `0x140308100` |
| Custom connector-box layout | `0x140083ee0` |
| Multiline text / version-177 property header | `0x14036f1d0` / `0x1403bf950` |
| Generic math / versioned Power and Log10 | `0x1402e2470` / `0x1402e4860` |
| Boolean Not (256) factory / constructor | `0x1400d4570` / `0x140328540` |
| Boolean Not specific load/save (no payload) | `0x14000d180` |
| Boolean Not value pull / input notification | `0x1403285f0` / `0x1402e3a50` |
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
| Drop List factory / constructor | `0x1400d6260` / `0x1403512b0` |
| Drop List selected-index load / save | `0x140333e60` / `0x140333ed0` |
| Drop List input / Integer / String callbacks | `0x1403514c0` / `0x140351550` / `0x1403515d0` |
| Drop List popup / native menu parser / ID lookup | `0x140351650` / `0x1403bb050` / `0x1403bafc0` |
| Delay / Double Delay factory | `0x1400dae90` / `0x1400daef0` |
| Sine / Double Sine factory | `0x1400db6d0` / `0x1400db730` |
| Pitch to Freq factory | `0x1400db9d0` |
| Sawtooth / Double Sawtooth factories | `0x1400db610` / `0x1400db670` |
| Triangle / Double Triangle factories | `0x1400db790` / `0x1400db7f0` |
| Ramp / Impulse / Glide factories | `0x1400dbe60` / `0x1400db310` / `0x1400db2b0` |
| Generator-specific load/save (no payload) | `0x14000d180` |
| Multiplexer (149) factory / constructor | `0x1400d8220` / `0x1403716f0` |
| Multiplexer load / save | `0x140372350` / `0x1403739c0` |
| Multiplexer Index clamp / trigger callback | `0x140373c10` / `0x140372240` |
| Last Switch (1054) factory / load / save | `0x1400d2f50` / `0x14030ef30` / `0x14030f6f0` |
| Last Switch input notification / Float read | `0x14030eec0` / `0x14030f120` |
| MIDI CC (166) factory / constructor | `0x1400d74f0` / `0x140363940` |
| MIDI CC specific load/save (no payload) | `0x14000d180` |
| MIDI CC init / parameter-input callback | `0x1403639c0` / `0x140363bc0` |
| MIDI CC message / Value-before-Changed callback | `0x140363ac0` |
| Bitmap (105) factory / constructor | `0x1400d4390` / `0x140326ac0` |
| Bitmap image load / save | `0x140326ec0` / `0x140327440` |
| Bitmap property header load / save (v160+) | `0x1403bf950` / `0x1403bfa10` |
| Bitmap Load / Paste / Clear dispatch | `0x140326cd0` |
| Image-table zero-based lookup | `0x140029e10` |
| File Dialog (103) factory / constructor | `0x1400d5960` / `0x14033f310` |
| File Dialog specific load/save (no payload) | `0x14000d180` |
| File Dialog Open / result dispatch | `0x14033fa70` |
| File Dialog filter expansion | `0x14033f420` |
| Button (17) factory / constructor | `0x1400d4930` / `0x14032dfd0` |
| Button load / inherited property save | `0x14032e1c0` / `0x1403bfa10` |
| Button property-base loader | `0x1403bf950` |
| Button activation dispatch | `0x14032e1d0` |
| After Duplicate (551) factory / constructor | `0x1400d2090` / `0x1402e0510` |
| After Duplicate specific load/save (no payload) | `0x14000d180` |
| After Duplicate lifecycle dispatch | `0x1402e0580` |
| Sample and Hold (1053) native load / save | `0x14031f6d0` / `0x1403214b0` |
| Sample and Hold type storage / Sample callback | `0x14031e950` / `0x14031f690` |
| Redraw Limiter (478) factory | `0x1400d9e10` |
| Redraw Limiter specific load/save (no payload) | `0x14000d180` |
| Native redraw gate / queued redraw dispatch | `0x14037deb0` / `0x14037dfc0` |
| Preset Float factory / constructor | `0x1400d99f0` / `0x140092ff0` |
| Preset Float load / save | `0x140093f10` / `0x140094460` |
| Preset Float input / init callbacks | `0x140093cd0` / `0x1400931b0` |
| Preset Float lock setter / ordering input | `0x1400927f0` / `0x140093ed0` |
| Preset automation header / lane loader | `0x140158fc0` / `0x140161af0` |
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

The signal-generator pass adds ten native IDs covering seven primitives,
including all four Sawtooth aliases that share the same factory. Five
hash-pinned unchanged Sawtooth/Triangle records from Multi Osc, Multi LFO and PWM
are exercised in a connected DSP bench. The complete unchanged Square submodules
from all three modules also render: they preserve their two Sawtooth instances,
phase summing, negative gain and optional hard-sync route before and after disk
save/reopen.

Authored fixtures cover all ten IDs across versions 154, 159, 166, 171 and 177,
including rising-edge sync across buffer boundaries, wrapped phase offsets,
`2^-40` double-precision phase increments, Ramp reset/wrap behavior, a single
startup Impulse, and rising/falling/zero-length Glide changes. Samples agree
across buffer sizes, and malformed ports or truncated records leave the current
project intact. Harmonic checks validate waveform and routing with one gain
calibration; absolute wavetable normalization remains covered by the core
oscillator tests. Double Sawtooth/Triangle, legacy aliases, Ramp, Impulse and
Glide use authored records based on the native factories and empty serializers;
no new unchanged bundled record is claimed for them. These checks use
OpenStone's existing DSP behavior, not new Windows output traces.

The refreshed scan remains **24/124** complete imports/save-reopens. Multi LFO
and PWM now reach Drop List (237); Multi Osc reaches unsupported record 405.
Their isolated oscillator banks additionally expose the delay-free phase-feedback
limit noted above. The verified Square submodules import independently.

The Drop List pass pins four unchanged records from Multi LFO, Detuner, Sample
and Hold, and PWM. Their saved indices include **0, 1 and 5**. Three additional
benches retain the original contiguous Items String, Index boundary and Drop List
records from Multi LFO, Detuner, and Sample and Hold, adding authored links and
headless View/Open controls. They verify the actual bundled menu options, chosen
item and boundary output, then save to disk, reopen and repeat the interaction.
These are record benches, not claims that the complete vendor Selector modules
import successfully.

Authored fixtures cover versions 154, 159, 166, 171 and 177; all seven input and
four output routes; negative and extreme signed saved indices; silent List/Check
changes; layout and popup coordinates; selection/cancel output pulses; stale
popup tokens; options removed during a pending menu; empty lists; separator IDs,
submenu headers and native counts; and disk save/reopen with a selected submenu
item. Truncated index payloads, invalid connector indices and invalid saved
selection types check atomic rejection. The existing core suite also checks
OpenStone-created DropList nodes so their behavior stays unchanged. Evidence
comes from native serializers/callbacks and headless tests, not new Windows UI
captures.

The refreshed library scan remains **24/124** complete imports/save-reopens.
Detuner, Multi LFO and Sample and Hold now reach Preset (189); PWM reaches the
legacy Mouse record (65). Their earlier Drop List blockers are removed.

The Preset pass adds hash-pinned 149-byte records from Multi LFO (`0x834`),
Detuner (`0xcfd`), and Sample and Hold (`0x64a`), each retaining all 32 saved
Float values. Detuner retains the original `0x3ee8ba2f` words rather than a
rounded textual value. Each record is imported into an authored wrapper,
checked before and after disk save/reopen, and edited through its live input.
No vendor sources or binary fixtures are distributed.

Authored Preset fixtures cover versions 154, 159, 161, 162, 163, 164, 166, 171 and
177; all ten input and three output routes; Preset-typed boundaries; independent
bank/current values; unnormalized Float values; fractional Order; standalone and
manager locks; and recalling/editing programs after reopening. Before/After
sampling checks the old and new values through the imported links. Tests also
cover empty banks before/after activation, a 4096-program bank and atomic rejection at every truncated payload length,
excessive or negative counts, invalid flags, nonempty recordings and automation
bindings. The existing preset/runtime regression tests still cover OpenStone
nodes created directly. These checks establish native record conversion and
OpenStone runtime behavior, not new Windows host automation parity.

The refreshed library scan remains **24/124** complete imports/save-reopens.
Detuner, Multi LFO and Sample and Hold now pass Preset (189) and reach generic
Last Switch (1054). Complete native Preset submodules remained blocked there;
the three unchanged Preset records are verified independently.

The routing pass verifies two unchanged Multiplexer View records from Alpha
Switch (`0xbe`) and 0-1 Timed Counter (`0xe4e`), each 15 bytes including its
common header. Six unchanged 44-byte Last Switch records from Multi LFO, Detuner
and Sample and Hold cover saved selections 0 and 1. Only hashes and offsets are
public; the tests use a separately supplied native library. Headless benches
exercise actual selected View drawing, Last Switch value routing, edited active
inputs and unconnected selected inputs before and after disk save/reopen.

Authored fixtures cover versions 154, 159, 166, 171 and 177; Automatic, Float,
Int, String, Boolean and Trigger Multiplexers; Index limits and notification
counts; empty, single, unconnected and 4096-output records; malformed counts,
types, selected-input bytes and truncated payloads. Float and double DSP benches
compare every sample across buffer sizes 1, 7, 64 and 257, retain `2^-40` double
precision, and check that unselected/unconsumed routes leave upstream code idle.
Each DSP bench is saved to disk and reopened before repeating the checks.
A reopened Float bench also changes routes between live audio buffers, including
negative/oversized Index clamping and an unconsumed route with no upstream trace.
No new native Windows rendering/audio traces are claimed.

The latest scan remains **24/124** complete imports/save-reopens. Detuner,
Multi LFO and Sample and Hold pass both Last Switch records and reach MIDI CC
(166). Alpha Switch passes Multiplexer and reaches Draw Bitmap (106).
0-1 Timed Counter now parses all its primitive records but stops on an unsupported
linked code-source producer. The importer keeps these explicit failures; the
isolated native routing records are verified independently.

The MIDI CC pass verifies three unchanged seven-byte component records from
Multi LFO (`0xc5a`), Detuner (`0x1125`) and Sample and Hold (`0xa72`). It also
imports each complete nine-node MIDI CC submodule, retaining its original
controller setting, enable Selector, -1 disable value, division by 127 and Float
module output. An authored host-MIDI source feeds the decoder alongside the
original blank wireless source; automatic binding of unnamed MIDI wireless
receivers is not established by these tests. Only hashes/offsets are public;
the native module bytes remain in the separately supplied reference library.

Authored records cover versions 154, 159, 161, 166, 171 and 177, both isolated and
through MIDI/Int/Trigger module boundaries. Tests check controller/status
filtering, channels 1/2/16, 7-bit endpoint values, repeated values and zero,
Value-before-Changed notification order, sampling the updated value on Changed,
invalid packet rejection, controller retargeting and disabled selections. Native
submodules additionally verify their actual normalized Float output and enable
controls. All benches save to disk, reopen and repeat their behavior checks;
transient CC data resets while stored controls and links remain intact. Header
truncation and invalid input/output indices check atomic rejection.

The MIDI CC scan remained **24/124** complete imports/save-reopens. Detuner and
Multi LFO passed MIDI CC and reached Bitmap (105). Sample and Hold parses its
primitive records but stops on an unsupported linked code-source producer.
These record/submodule tests do not claim new Windows output-trace parity or
complete import support for their enclosing vendor modules.

The Bitmap pass verifies three unchanged inline-PNG records: Multi LFO Speed
(`0x2168`, 4168 bytes), Multi LFO Depth (`0x493b`, 4168 bytes), and Detuner
(`0x337e`, 4215 bytes). All three contain 32×32 RGBA images. Tests compare every
decoded byte against SHA-256 digests independently obtained with a separate
PNG unfilter implementation, then save to disk, reopen, repeat the pixel and
dimension checks and exercise Clear. Their containing `.hom` files are
hash-pinned; public tests include only hashes and offsets, not vendor images.

Authored Bitmap fixtures cover versions 154, 159, 160, 161, 166, 171 and 177,
inline and document-table images, empty images and the -1 reference sentinel,
property headers and connector layouts, non-square dimensions, straight alpha,
native Bitmap/Width/Height module-output links and remapped Clear inputs. A
headless host loader verifies passive Path changes, explicit Load, and reopening
from saved pixels without reading the original image path. Invalid resource
indices, oversized dimensions, corrupt images, truncation, bad lengths/flags,
invalid ports and connected Paste inputs check atomic rejection.

The Bitmap inventory remained **24/124** complete imports/save-reopens. Detuner
and Multi LFO passed Bitmap and reached File Dialog (103); their complete Knob
and Bitmap wrapper modules are not yet supported. This pass establishes static
pixel preservation, not Windows GUI rendering parity.

The File Dialog pass verifies the unchanged seven-byte records following those
three Bitmaps: Multi LFO Speed (`0x31c3`), Multi LFO Depth (`0x5996`), and Detuner
(`0x4408`). Separate authored benches retain both the native File Dialog and
native Bitmap records and reproduce their original Path-to-Path and OK-to-Load
connections. A simulated successful selection loads exactly one image; Cancel
does not request a load. The loaded pixels and functional picker links survive
disk save/reopen without reading the original image file.

Authored File Dialog fixtures cover versions 154, 159, 160, 161, 166, 171 and 177,
standalone records and all seven inputs/three outputs through module boundaries.
They verify default and explicit Open/Save modes (including nonzero extremes),
full and shorthand filters, Unicode paths, passive configuration, captured
request parameters, Path-before-OK ordering, Cancel, stale responses and saving
with a pending request. Native compatibility flags survive reopening; malformed
flags, truncated headers and invalid connector indices reject atomically.

The File Dialog inventory remained **24/124** complete imports/save-reopens. Detuner
and Multi LFO passed File Dialog and reached Button (17). The tests above use
unchanged component records in authored benches; their complete enclosing Knob
and Bitmap wrapper modules remain unsupported.

The Button pass verifies four unchanged native records: Multi LFO Speed
(`0x31ca`, 62 bytes), Multi LFO Depth (`0x599d`, 62 bytes), Detuner (`0x440f`,
62 bytes), and Switch (`0x8c0`, 65 bytes). The first three retain the caption
`Load Bitmap...`; Switch retains `Load Animation...` and its different property
flag. Each record sends exactly one pulse per activation to both connected
receivers before and after disk save/reopen.

Three additional authored benches retain the native Button, File Dialog and
Bitmap records and reproduce their original activation, Path and Load links.
Clicking Button opens the correct nested File Dialog, successful selection
loads one bitmap, and Cancel preserves existing pixels. The complete chain and
loaded image survive disk save/reopen, including when a dialog was pending at
save time, without replaying a click or reading the source file on reopening.
Only module hashes and record offsets are public; vendor record/image bytes
stay in the externally supplied reference corpus.

Authored Button records cover versions 154, 156, 157, 159, 160, 166, 171 and 177,
both property flags, present/absent connector layouts, anchor metadata, module
boundaries, trigger fan-out, captions and grid positions. A following Int record
checks byte alignment. Truncation at each header/layout byte, invalid Boolean
flags, excessive box counts and invalid input/output indices reject atomically.

The Button inventory remained **24/124** complete imports/save-reopens. Detuner,
Multi LFO and Switch passed Button and reached After Duplicate (551).

The After Duplicate pass verifies four unchanged seven-byte records: Multi LFO
Speed (`0x3208`), Multi LFO Depth (`0x59db`), Detuner (`0x444d`), and Switch
(`0x901`). Tests exercise duplication, clipboard paste, duplicating a copy and
duplication after disk save/reopen. Only new copies receive one event; source
modules and external receivers remain unchanged. Native Counter observers use
the existing reset-on-copy policy, while project saving retains their values.

Three complete unchanged eight-node/eight-link Bitmap loader submodules are
also imported: Multi LFO Speed (`0x212c`–`0x326e`), Multi LFO Depth
(`0x48ff`–`0x5a41`), and Detuner (`0x3342`–`0x44b3`), with end offsets exclusive.
They retain their original Button, File Dialog, image pixels, dimension monitors,
Bitmap output and both lifecycle nodes. The copy publishes its image exactly
once through After Duplicate without loading a file; subsequent selection
updates only the copy. Both images and all links survive disk save/reopen, which
fires only After Load. Reopened loader copies remain functional.

Authored records cover versions 154, 159, 161, 166, 171 and 177, direct and nested
modules, coexistence with native After Load, following-record alignment,
clipboard isolation, duplicate-time link availability and invalid/truncated
headers or connector indices. Reopening checks persisted duplicate counters
separately from expected new After Load events.

The After Duplicate inventory remained **24/124** complete enclosing-module imports and
save/reopens. Detuner and Multi LFO now pass their Bitmap loader submodules and
reach component 1053 in `Preset/RedrawLimiter`. Switch parses its primitive
records but stops on an unsupported linked code-source producer. Internal
submodule tests do not imply full support for those enclosing vendor modules.

The Redraw Limiter pass imports three complete unchanged four-node/four-link
submodules from Multi LFO Speed (`0x37a5`–`0x3846`), Multi LFO Depth
(`0x5f78`–`0x6019`), and Detuner (`0x49ed`–`0x4a8e`), with end offsets exclusive.
Each retains its cached Float word `0x3ed5ab57` before the first input event.
Headless tests inject a monotonic clock, check immediate/burst/10 ms boundary
events, explicit redraw suspension, hidden/open plug-in editors and the current
value sampled through the original link order. Disk save/reopen preserves the
held output, controls and links; the scheduler starts with no stale timestamp.

Authored fixtures cover versions 154, 159, 161, 166, 171 and 177, Float/Int/Boolean/
String caches, later-specialized unresolved connectors, passive Value changes,
repeated Sample events, native cache persistence in module copies and unchanged
ordinary-hold behavior. Malformed caches, headers, fixed ports and lock indices
reject atomically. The public tests contain only authored data and native module
hashes/offsets, with vendor bytes supplied by the external reference corpus.

The Redraw Limiter inventory remained **24/124** complete enclosing-module
imports/save-reopens. Detuner and Multi LFO now pass `Preset/RedrawLimiter` and
reach Boolean Not (256) in their containing Preset modules. Internal module
coverage has advanced; those complete enclosing modules still require more
record mappings.

The Boolean Not pass tests unchanged records from Multi LFO Speed (`0x3a54`),
Detuner (`0x4c9c`) and Amp (`0x2621`), plus their complete 19-node/20-link Preset
submodules: `0x330a`–`0x3b2f`, `0x454f`–`0x4d77` and `0x1ed9`–`0x26fc`,
respectively, with end offsets exclusive. Supplemental data notifications check
the original Interact-to-edit-gate and Interact-to-Not-to-redraw-gate routes:
the edit gate opens during interaction, and the inverted redraw gate opens only
when idle. Both values, repeated notifications and both gates remain functional
after disk save/reopen. Whole-file hashes pin the external native fixtures;
vendor bytes are not included in the public tests.

Authored fixtures cover versions 154, 159, 161, 165, 166, 171, 174 and 177,
fixed Boolean types, unconnected defaults, chained inversions, fan-out through
module boundaries, header/following-record alignment, silent import/wiring and
disk save/reopen. Truncated headers and invalid input/output indices reject
atomically. These are headless import/runtime checks, not live Windows tests.

The Boolean Not inventory remained **24/124** complete enclosing-module
imports/save-reopens. Amp, Detuner and Multi LFO now pass their Preset modules;
their next failures are unsupported linked code-source string producers in
`Label/1`, `Label/1` and `Selector/1`, respectively.

The Ruby connector correction raises the complete-module scan to **40/124**
imports/save-reopens. The 16 newly passing modules are 0-1 Timed Counter,
Bitmap-Strip LED-Indicator, Bitmap-Strip State, Bitmap, Detuner, Distortion,
Float Array Reader, Knob - Animated, Knob - Static With Gauge, Knob - Static,
LED-Indicator, Midi-CC Encode, Overdrive, Sample and Hold, Slider - Horizontal
and Ticker - Custom. Every one is now a hash-pinned complete-module regression
fixture. The scan measures import and project persistence, not complete runtime
parity for each module.

Execution tests use Amp's unchanged Label submodule (`0x2e8`–`0x614`, exclusive)
and the complete unchanged Midi-CC Encode module (`0x77` onward). Label retains
View/color/String inputs 0/1/2 and draws its text, color, font and alignment;
code-looking label text stays ordinary data. The encoder's Float input zero
emits correctly clamped CC packets through MIDI output zero, and changing Int
input one changes the CC number without running its value-input branch. Both
unchanged Ruby kernels execute again after disk save/reopen. Tests use the
installed Ruby backend when available; importer-only fixture checks still run
without it. Vendor Ruby sources remain in the external corpus.

The encoder also exposed a missing runtime API: native code constructs packets
with `Midi.new`, using channel zero for the first MIDI channel. OpenStone now
supports this spelling with channels 0–15, including Program Change and SysEx.
The existing `MIDI.new` constructor retains its channels 1–16. Headless tests
check both channel endpoints, invalid-channel rejection and packet lengths;
the unchanged encoder exercises `Midi.new` through imported output zero.

Authored tests verify native String input/output zero, non-executing import,
invalid extra native connector indices, and independently connected OpenStone
Source ports through data-port resizing and disk save/reopen. Amp now reaches
a DSP connector-type mismatch, while Multi LFO reaches a feedback-link rejection;
these remain explicit blockers in the refreshed inventory.

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
