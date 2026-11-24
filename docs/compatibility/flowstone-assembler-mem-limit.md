# The packed Mem descriptor limit

Assembler `memrefin` now exposes field 4 as a read-only 128-bit packet of four identical Float limits. This completes the descriptor access needed by the three recovered Wave Read / Wave Read Hop bodies. Tests run each complete original body with only its omitted caller ports declared, plus its original hop control bound when required.

## Verified binder representation

The native `CVarMemRef` virtual method at index 2 supplies the descriptor in both Alpha 5483 builds. RTTI identifies the x86 vtable at `0x8a5214` and x64 vtable at `0x14050d880`. The implementation writes four eight-byte fields, followed at byte offset 32 by a packed Float broadcast. The layout uses eight-byte metadata fields in the x86 build too.

| Build | Binder virtual address | File offset | Bytes | SHA-256 |
| --- | --- | --- | ---: | --- |
| x86 | `0x692b70` | `0x291f70` | 265 | `dfbf57a49a3363759dd130ceb9311d7b1747eb5f43dbe54bb6e8b7320b1afee0` |
| x64 | `0x1402dedd0` | `0x2de1d0` | 178 | `bebb7787ac7d82c0bded2d04ca230a03b4469ead3b2e40d4ba18b7e32138df6f` |

Executable hashes are x86 `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8` and x64 `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`. These binder-byte hashes cover the ranges ending at `0x692c79` and `0x1402dee82`, respectively.

The x86 branch at `0x692ba1` and x64 branch at `0x1402dee04` select byte length divided by eight for stereo and by four otherwise, truncating incomplete frames. Both then perform **CVTDQ2PS, SUBSS 1.0, SHUFPS 0**, in that order. The x86 conversion/subtraction are at `0x692bc3` / `0x692bd4`; x64 uses `0x1402dee2d` / `0x1402dee42`. The subtraction constants at x86 `0x86cba8` (file `0x46b5a8`) and x64 `0x1404ba608` (file `0x4b9608`) both contain `0x3f800000`.

Thus a valid descriptor's limit is `float32(float32(complete_frame_count) - 1.0f)`. The two rounding steps matter: 16,777,217 frames produce 16,777,215 (`0x4b7fffff`), and 16,777,219 frames produce 16,777,220 (`0x4b800002`). Computing the integer last index first would change these bits. OpenStone preserves the native calculation, including these large-count results; its existing bounded Mem read policy still applies.

The null branches at x86 `0x692c14` and x64 `0x1402dee62` clear the entire descriptor, including this packet. An absent Mem therefore supplies zero, while a present short byte buffer containing zero complete frames supplies -1. Green Memory canonicalizes an empty buffer as absent; it has no distinct non-null zero-byte object to reproduce.

## OpenStone cache and operand contract

The private descriptor cache now reserves separate numeric-metadata and Float-limit packets. Existing fields 0/1/2/3 and their bounded reference, byte length, Float-word count and channel semantics are unchanged. The cache is rebuilt by `setDspMemRefInput` when a binding changes, including a clear to empty, without recompilation or an audio restart. A floating-point environment guard fixes rounding to OpenStone's established nearest-even policy and restores the caller's complete environment, including sticky exceptions. The rounded count is materialized separately before subtraction.

Field 4 loads reuse existing array-packet and raw-128-bit interpretation operations. Packed Float instructions, a scalar DWORD read, FLD, COMISS and CVTSS2SI can read the limit. MOVAPD retains the raw Float packet bytes instead of numerically converting them to Double. All descriptor stores stay forbidden and selectors are bounded to 0..4. The subsequent [packed numeric descriptor work](flowstone-assembler-descriptor-packets.md) also enables raw XMM reads of fields 1..3; field 0 stays opaque. General wide numeric operations and pointer provenance rules are unchanged. No public schematic format or backend opcode is added.

## Complete recovered readers

The existing exact fixtures `engine-mem-reader-channels.asm`, `engine-mem-reader-hop-x86.asm` and `engine-mem-reader-hop-x64.asm` are preserved byte for byte. [Their lengths, hashes and native offsets](flowstone-assembler-descriptor-aliases.md#concrete-native-evidence) remain the same. Unlike the earlier dispatch-prefix test, the new `assembler_mem_limit` suite executes the entire source after prepending only `streamin in1;streamout out0,out1;`. Hop sources additionally receive their explicit caller-owned `p_hop` mask.

An independent wave-lookup oracle compares every output sample word and all four lanes in the interpreter and LLVM ORC at buffer sizes 1, 3, 7, 31, 64 and 257. Cases include absent, mono, stereo and single-frame waves; unaligned views with incomplete trailing bytes; fractional nearest-even sample indices; negative, infinite and NaN positions; changing/clearing waves between buffers; all four hop masks 0/1/3/15; reset; and the exact output-hold behavior on skipped/absent frames. Complete reader instructions, fields, registers and branches are not rewritten.

Separate tests verify raw limit words, real large-buffer views near 2^24 frames under three caller rounding modes, no exception leakage during binding/rendering, scalar/x87 reads, Float-to-Int conversion, comparisons, raw PD views, isolation from a second Mem descriptor and a following FloatArray input, and initialization/early/late visibility across binding changes and clears. Empty and short buffers have distinct literal expectations. Descriptor writes, unsupported wide numeric loads and invalid selectors remain negative cases. Existing stream, descriptor-alias, engine-table and input-write suites pass, as do interpreter-only tests with portable SoftFloat.

## Audit scope

The strict corpus retains exactly **58 prepared entries**, **66 frontend-accepted entries**, all **19 serialized sources prepared**, **eight explicit engine-binding requirements**, zero missing mnemonic names and no backend-only failures. The recovered reader strings still omit `in1` and their outlets, so their strict diagnostics remain honest missing-caller-context failures. Audit integration now verifies that supplying only those omitted ports makes every complete reader frontend-accepted: the plain reader prepares in both backends, and the hop readers correctly require the original engine binding.

Earlier fixtures and numerical captures are unchanged. The native evidence is static binder/source evidence; numerical tests use independent reference results. No new Windows execution trace is claimed.
