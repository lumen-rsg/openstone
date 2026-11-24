# Native Assembler frame memory

OpenStone binds incoming **EDX/RDX and EBP/RBP to the same owned, bounded frame**. It supports raw frame loads/stores and the verified engine memory aliases. This establishes frame memory access; it does not recreate native host pointers, function tables, or an imported schematic's global variable layout.

## Evidence from Alpha 5483

The inspected executable hashes are x86 `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8` and x64 `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`. Evidence is static disassembly and exact recovered sources, not a new Windows execution trace.

The native sample loops pass the frame in EDX/RDX immediately before calling generated code (x86 `0x458c84`–`0x458c89`; x64 `0x140063150`–`0x140063155`). The generated prologue copies it to RBP: `push rbp;mov rbp,rdx;`. The supplied native code dump independently shows bytes `55 48 8b ea`. See [incoming-register evidence](flowstone-assembler-entry-bindings.md).

The frame constructor requests 16-byte-aligned storage of a caller-specified size and clears that allocation (x86 `0x44ec5f`–`0x44ec80`; x64 `0x140057e0b`–`0x140057e2b`). Gate words are initialized to zero; Poly mode sets all four DWORDs at `0x1080` to `0xffffffff`. The native frame is a byte-addressed allocation containing four-lane packets, not a C++ object whose host layout can be exposed directly.

The name resolver maps the following names to **memory offsets**. They are named memory operands, not integer literals holding those offsets. For example, recovered reducer `08a8c075232b` exchanges RBP/RDX and then executes `movaps xmm1,_CHANNEL_MASK_OFFSET;andps xmm1,inPoly;`. Treating the name as a literal `0x1050` produces the wrong mask.

| Native memory alias | Byte offset | x86 resolver name reference | x64 resolver name reference |
| --- | --- | --- | --- |
| `_CHANNEL_GATE_OFFSET` | `0x1010` | `0x5d4509` | `0x140207ddd` |
| `_CHANNEL_TRIGGER_OFFSET` | `0x1020` | `0x5d4547` | `0x140207e23` |
| `_SAMPLE_POS_OFFSET` | `0x1030` | `0x5d4562` | `0x140207e41` |
| `_FRAME_POS_OFFSET` | `0x1040` | `0x5d457d` | `0x140207e5f` |
| `_CHANNEL_MASK_OFFSET` | `0x1050` | `0x5d4598` | `0x140207e7d` |
| `_IS_POLY_PROCESS` | `0x1080` | `0x5d45b3` | `0x140207e9b` |
| `_CHANNEL_CHANNEL_ID_OFFSET` | `0x10c0` | `0x5d45ce` | `0x140207eb9` |
| `_CHANNEL_NOTE_ID_OFFSET` | `0x10e0` | `0x5d45e9` | `0x140207ed7` |

Each x86 branch returns the listed byte offset in EAX; x64 returns it in EDI. The registrations also include `_CHANNEL_VOICE_ID_OFFSET`, but a corresponding resolved offset has not been established in this pass, so it is not fabricated.

Further native writes establish physical representations:

- Gate and active-mask lanes are raw DWORD masks. A held lane gets `0xffffffff`; release clears gate while active can remain set (x64 `0x1400911ef`–`0x1400911fe` and `0x14009129f`–`0x1400912d7`).
- The trigger packet is incremented with ADDSS by Float 1. The sample-position setter writes one raw integer to all four words at `0x1030`–`0x103c` (`0x1400913f1`–`0x14009140f`). Its caller meaning is not inferred from the field name alone.
- The sample loop converts a signed 32-bit position to Float and writes only the low word at `0x1040` (`0x1400630ff`–`0x140063119`); upper words are not implied broadcasts.
- The channel-ID packet is Float `4 * group + lane`, a DSP voice-slot index, not a MIDI channel (`0x14009146c`–`0x14009147d`). The note-ID setter also converts its source integer to Float (`0x1400914bb`–`0x1400914c7`). Other Float packets are written through `0x1100`–`0x110c`; their meanings remain unassigned here.

## Implementation

A frame reference carries provenance and a byte cursor. Both register spellings, register copies, stack saves/restores and XCHG preserve that provenance. Overwriting a register with numeric data removes it. Numeric output of a reference remains an error; no native address is guessed or exposed.

Frame operands use the existing interpreter and LLVM ORC memory lowerings. A reserved virtual memory port selects owned frame bytes before ordinary Mem/MemArray input indexing. Scalar DWORDs, byte accesses, 128-bit Float packets and both Double windows therefore observe the same little-endian bytes. Local call targets retain separate provenance tags; overlapping writes invalidate them. Reads crossing the allocation return zero and invalid writes are ignored, following OpenStone's bounded-memory policy.

The eight aliases resolve through the **current RBP**. Thus swapping RBP/RDX to a different bounded memory reference retargets an alias exactly like an explicit `[rbp+0x1050]` access. An alias can use lane/index syntax, and explicit source declarations retain their existing precedence. The aliases appear in highlighting/completion and numeric-branch width calculations. Their offsets are not introduced as immediate expressions inside arbitrary address syntax.

Only programs that actually dereference a native frame allocate its storage. The default image is zeroed and covers bytes `[0,0x1110)`, through the last observed fixed packet. This is a supported prefix, not a claim that every native frame has that total size. `Runtime::setDspNativeFrame(block, bytes)` copies an explicit caller/import image, from that minimum size through 16 MiB; it does not alias caller RAM. This permits larger imported allocations without inventing variable offsets. Reset restores the supplied initial image, or zeroes for the default image.

Frame bytes persist across stages and buffers. A packed Poly packet owns the frame shared by its four lanes. Reinitializing a single note preserves that shared frame; resetting the whole runtime restores it. Declared DSP variables still use OpenStone's existing program slots. An importer must supply the original global frame image/layout rather than assuming that its offsets coincide with those slots.

## Engine field values and limits

OpenStone supplies active masks, held-note gate masks, Poly mode and Float voice-slot indices from its render context. Active masks include release voices; gate masks exclude them. Mode is initialized once, and lane fields change when the corresponding engine state changes, preserving intervening raw writes. Mono uses one valid lane; packed Mono uses four. The frame-position low word receives the signed low 32 bits of root render time converted to Float, independently of the ECX clock and the 64-bit HOP/fault frame.

Trigger, sample-origin, note-ID and other unassigned field values remain raw caller/import bytes, initially zero. Their automatic MIDI/host adapters are separate work. The special native ECX-relative wrapper is not automatically inserted. Host transport positioning, pointer-cell/code tables and arbitrary incoming RAX values remain explicit context gaps. Direct caller-owned `__const` data pointers now have [named engine bindings](flowstone-assembler-engine-pointers.md). Frame bytes do not replace the existing voice manager or become a new route for native machine calls.

## Exact sources and tests

The previous Float/Double frame-load fixtures remain byte-for-byte unchanged. New tests provide only their caller cursor, declarations, output taps and copied frame image; RBP/RDX are now supplied by the runtime rather than a synthetic Mem-pointer prologue. Every sample is checked against an independent byte reader.

Two new 77-byte fixtures are exact substrings of the native reducer sources:

| Fixture | Executable file offset | SHA-256 |
| --- | --- | --- |
| `engine-frame-mask-read-x86.asm` | x86 `0x4e6a75` | `d247a6ee3bca24f851f8feb93ea4c5e3ce3db5723ed4de8c38ce625eb56f50b1` |
| `engine-frame-mask-read-x64.asm` | x64 `0x588e22`, also `0x589703` | `95f50cb68df24e1f5e1dfcb05b8d473f5579c9fd21b63fb9751d1c2232a4de27` |

These fragments run unchanged in explicit caller contexts. The complete reducer kernels still need their original `__const` bindings; fragment tests do not claim full-kernel acceptance.

`assembler_frame_bindings` checks both backends across buffers 1, 3, 7, 31, 64 and 257 where applicable. Tests cover exact Float/Double bits, double-only precision, NaNs, signed zeros, subnormals, byte neighbors, frame bounds and extended images, copied image ownership, scalar stores, stage timing, reset, independent blocks, pointer opacity, aliases after frame exchange, source-name precedence, numeric skips, memory calls/tail jumps and target invalidation. Five Poly voices exercise two packets, held versus release masks, idle intervals and slot reuse. A separate counter checks that reusing the packet's owner slot cannot erase the shared frame. The new frame suite and incoming-register suite also run with LLVM disabled and portable SoftFloat enabled.

The strict compile audit retains **all 56 accepted entries**, with no new backend-only failure or missing mnemonic. The unchanged frame-load fragments now advance past the RBP binding and stop at their genuinely missing incoming RAX cursor. No arbitrary cursor, native pointer or source rewrite is injected to increase the count.
