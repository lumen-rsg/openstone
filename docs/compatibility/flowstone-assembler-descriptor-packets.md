# Packed numeric Mem descriptor reads

Assembler can now read `memrefin` fields 1, 2 and 3 with packed instructions. This closes the `movaps xmm2,mem[2]` failure in the recovered Stereo Wave Write source. The complete original Mono and Stereo Wave Write bodies run with only their omitted caller ports declared; no instruction, label, register or branch is rewritten.

## Native evidence and raw layout

The [verified x86/x64 `CVarMemRef` binders](flowstone-assembler-mem-limit.md#verified-binder-representation) write four eight-byte fields followed by a sixteen-byte Float limit packet. Both builds use this layout. The numeric fields hold byte length, Float-word count and channel count. Consequently a sixteen-byte read at a numeric field spans the next field, rather than broadcasting four copies of the selected DWORD.

| Operand | Byte window | Four raw DWORDs |
| --- | --- | --- |
| `mem[1]` | 8..23 | byte length, 0, Float-word count, 0 |
| `mem[2]` | 16..31 | Float-word count, 0, channel count, 0 |
| `mem[3]` | 24..39 | channel count, 0, limit bits, limit bits |
| `mem[4]` | 32..47 | limit bits, limit bits, limit bits, limit bits |

The zero high DWORDs follow from OpenStone's existing signed-DWORD bound on Mem byte length. Limit bits retain the native two-step Float calculation and null behavior documented in the binder reference. The cross-field windows are derived from the binder's byte layout, not from a new Windows runtime capture.

The exact recovered writers occur in both Alpha 5483 executable builds:

| Fixture | Bytes | SHA-256 | x86 file offset | x64 file offset |
| --- | ---: | --- | --- | --- |
| `engine-mem-writer-mono.asm` | 668 | `fdd8dc0a38c0f5797ac1200567d2def6f98b8b44856e80c49b864b74108aa239` | `0x4ee400` | `0x597c80` |
| `engine-mem-writer-stereo.asm` | 928 | `a94c4cdbee4039dc8265b148c92b687bc6b3fa7ed9a5545a1cce4306a537a8a3` | `0x4ee988` | `0x5984e0` |

The Mono writer reads its bound from `mem[4]`. Stereo reads `mem[2]`, broadcasts its low DWORD, shifts it right by one, converts the integer to Float and subtracts one. It therefore derives its stereo frame count from the number of Float words. Keeping the original bytes exercises these distinct paths.

## Implementation boundaries

The frontend assembles each raw window using the existing cached numeric and limit packets, raw Double-width lane selection, `ShufflePd` and `Reinterpret128`. No new backend opcode, runtime cache allocation, public binding API or project format is needed. MOVAPD sees the same sixteen bytes as MOVAPS/MOVUPS; small integer metadata is never converted numerically or flushed as a floating-point subnormal. Both backends use the existing raw-bit operations.

Descriptor writes remain forbidden for every field. Field 0 remains an opaque bounded reference loaded through an integer register, rather than exposing a host address through an XMM packet. Scalar numeric reads, pointer/target provenance, selector bounds and general 64-bit numeric restrictions remain in place. Descriptor rebindings and clears become visible without recompilation or an audio restart.

## Headless validation

The `assembler_descriptor_packets` suite runs in the interpreter and LLVM ORC at buffer sizes **1, 3, 7, 31, 64 and 257**. It checks:

- Exact PS and PD words for all four windows, including field 3 crossing into the limit packet; absent/short/unaligned mono and stereo views; adjacent descriptor/array storage; reset and repeated live rebinding.
- Initialization, early and late stage visibility; empty render calls; preserved integer flags; read-only descriptors, bounded selectors and opaque pointer restrictions.
- Every passthrough sample/lane and destination bytes from both complete unchanged native writer bodies. Independent byte-write and nearest-even indexing oracles cover NaNs/infinities/negative positions, signed zero data, short buffers, incomplete trailing bytes, changing/clearing destinations and guard bytes outside the Mem view. Caller inputs and floating-point environment stay unchanged.
- Five Poly voices in two packets, staggered starts, release and EndVoice, slot reuse, overlapping destination indices and changing/cleared bindings. A separate event/byte model checks active-lane masks and packet/lane write order after every buffer.

The writer source's existing non-Poly control flow is preserved: an eligible lane 0 writes and exits early; if lane 0 is ineligible, later eligible lanes are considered. The Poly test checks all active lanes, including release voices, and excludes inactive lanes. No source branch was normalized to a different policy.

Related descriptor aliases, Float limits, frame bindings, engine tables, input writes and stream runtime tests pass. The descriptor and compile-audit suites also pass with the interpreter-only portable SoftFloat build. Test expectations are independent models/literal words, not newly captured FlowStone numerical traces.

## Compile audit

The rerun preserves every prior strict result: **58 prepared**, **66 frontend-accepted**, **19/19 serialized sources prepared in both backends**, eight explicit engine-binding requirements, zero missing mnemonic names and no backend-only failures. Corpus hashes and prior fixture/capture bytes remain unchanged.

The raw writer strings omit `in2` and their other caller ports, so the strict audit retains their original missing-name diagnostics. A new integration check supplies only those omitted ports and verifies that both complete writers prepare in both backends with no external engine-pointer binding. The Mono writer provides a regression check for the earlier limit-field work; the Stereo writer exercises this new packed numeric read.
