# Native Assembler entry bindings

OpenStone now supplies the **base engine sample counter in ECX**, including CL/CH, and permits save/restore of otherwise unknown incoming GPRs. It does not expose host pointers or reconstruct the complete FlowStone frame ABI. The subsequent [incoming XMM pass](flowstone-assembler-xmm-entry.md) supplies explicit raw SIMD banks with a documented portable zero fallback. The subsequent [native frame-memory pass](flowstone-assembler-frame-bindings.md) adds the bounded RDX/RBP binding and verified frame aliases.

## Native evidence

Inspected unpacked FlowStone Alpha 5483 executables:

| Build | Executable SHA-256 |
| --- | --- |
| x86 | `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8` |
| x64 | `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e` |

These are static disassembly observations, not new Windows execution traces. Addresses below are executable virtual addresses, with the files' preferred image bases.

| Observation | x86 | x64 |
| --- | --- | --- |
| Owner constructor clears the persistent counter | `0x458a8b`, owner + `0x18` | `0x140062f4a`–`0x140062f5c`, owner + `0x28` |
| Load persistent counter before processing | `0x458c15`–`0x458c1d` | `0x1400630de`–`0x1400630e7` |
| Set frame argument and ECX before calling generated sample code | `0x458c84`–`0x458c89` (also `0x458cc0`–`0x458cc4`) | `0x140063150`–`0x140063155` (also `0x140063185`–`0x14006318a`) |
| Advance counter once per rendered sample | `0x458cd8` | `0x1400631ad` |
| Reduce counter at end of buffer and persist it | `0x458d1b`–`0x458d2d` | `0x1400631f3`–`0x140063207` |

The final sequence uses LEA to subtract `0x3b9aca00`, CMP against that same value, and **signed CMOVLE** to keep the original counter at or below the threshold. Therefore it subtracts exactly 1,000,000,000 once, strictly above the threshold, at buffer end. It is neither a buffer-local index nor an unconditional per-sample modulo. The engine constructor initializes it to zero. ECX is set from the persistent value immediately before each sample function call; in x64 this also zero-extends RCX.

The sample call supplies the native frame in EDX/RDX. Both builds contain the generated prologue source `push rbp;mov rbp,rdx;` (x86 file offset `0x49f6b4`, x64 `0x506678`); the x64 builder references it at `0x140205422`. The supplied user code dump independently contains the actual prologue bytes `55 48 8b ea`.

A separate code-emission branch constructs `mov eax,[rbp+0x1030];push rcx;sub ecx,eax;` (x64 builder `0x1402055ef`–`0x1402056a8`, x86 `0x5d2149`–`0x5d21d8`). This proves that some generated wrappers adjust ECX relative to a native frame field. It does **not** establish which imported contexts should receive that adjustment. That wrapper and its frame layout remain future work. The public binding implemented here is the verified sample-function base counter. Packed Poly tests below validate OpenStone's integration of that base clock; they are not evidence that every native Poly wrapper has been reproduced.

## Runtime and compiler behavior

The render runtime owns the clock. Every active Assembler stage receives its current value in a private program slot, with no dependence on native host CPU registers. Reset returns it to zero; successful nonempty buffers apply the observed end-of-buffer reduction. Empty OpenStone render calls retain their existing no-op contract. Absolute HOP scheduling and fault frames remain separate 64-bit clocks.

The interpreter has an explicit `ReadSamplePosition` operation. LLVM ORC lowers it through a typed helper reading the same render context; the stage call ABI remains unchanged. Empty stages do not become active merely because ECX has a binding. ECX writes and partial CL/CH writes change only the current block/stage's register value, never the engine counter for the next sample. Scalar, packed Float/Double, headless Poly and the editor's Double packed Poly renderer use the same clock rules. All voices in a root renderer receive its current base engine counter, including newly allocated/reused voices and voices in a second SSE packet.

PUSH of an unknown incoming GPR creates an opaque saved value. POP may restore that value, including into a different register, but leaves its bits unavailable to arithmetic, output, comparisons, memory addressing and call targets. It is not replaced with zero. Initialized numeric values and pointer/target tags retain ordinary runtime stack storage. Stack availability and reference provenance merge across branches and survive nested/recursive calls; any path carrying an unknown value prevents a subsequent observable read. Incoming flags are still undefined. Incoming XMM values now use the explicit bank described in the subsequent [XMM entry pass](flowstone-assembler-xmm-entry.md); that portable default does not reproduce native upstream scratch-register allocation.

## Unchanged recovered sources

New fixtures retain the exact source bytes, including CRLF and trailing whitespace. Offsets below identify the first stored occurrence in each executable; the audit manifest records all copies.

| Fixture | x86 file offset | x64 file offset | Bytes | SHA-256 |
| --- | --- | --- | --- | --- |
| `assembler-incoming-clock-smooth.asm` | `0x64a7a4` | `0x7947a4` | 187 | `0114659c9cd2b94654a10d586487f4eb6e72759d7d1cf83d95d20103dcb00574` |
| `assembler-incoming-clock-ramp.asm` | `0x66e31b` | `0x7b831b` | 263 | `dae8bd756d7269e3d4d23b0f330be82445bb6dd62361cc52f4fc4ae70a2ef505` |
| `assembler-mem-nibble-reader.asm` | `0x660b29` | `0x7aab29` | 1381 | `7f8d86c70843ebb05637a8a0d6207f814e30a06de271430a8d8decb520dbeddc` |
| `assembler-memarray-flat-reader.asm` | `0x67185d` | `0x7bb85d` | 1452 | `d750d8477dfe297b25d6765ff37fc1f398d55aeb3be18f1f1c7e9b4563a93c62` |

The existing byte reader (`17a241c5e13c`) and writer (`2ec4728ea714`) fixtures are also tested unchanged, now without injected caller-register prologues. No previous fixture or capture was modified.

## Verification and audit

`assembler_entry_bindings` runs in the interpreter and LLVM ORC. Its independent expected values cover:

- Every sample of the original ramp (updates every four samples) and Smooth (every 64 samples), including independent packed lanes.
- The four Mem/MemArray helpers: numeric byte reads, raw byte writes with neighbor preservation, high/low nibbles, two loaded waves and sample-index clamping, plus null Mem paths.
- Literal counter boundary vectors: exactly one billion, strictly greater, single subtraction, signed limits and 32-bit wrap. Backend entry reads preserve all counter bits, independently of the absolute frame argument.
- Buffers 1, 3, 7, 31, 64 and 257 where applicable, variable buffer sequences, zero-frame calls, reset, two independent blocks and stages 0–3. Scalar/packed Float and Double rendering are covered.
- CL/CH reads, partial writes, caller floating-point state, opaque saves/restores, joins, nested/recursive calls, and rejection of observable unknown values.
- Five voices across two SSE packets, staggered notes, idle periods and slot reuse in headless Poly; the editor Poly renderer is checked separately for two packets, idle periods, new notes and reset.

The new suite and updated legacy byte-register suite also pass with LLVM disabled and portable SoftFloat enabled. Two legacy negative test suites now deliberately use unknown parents rather than assuming ECX has no entry value.

The compile audit now accepts **56 of 236 category/source entries**, retaining the exact previous set of 50 and adding precisely these six embedded sources. Embedded acceptance rises from 11/19 to 17/19; the other category totals are unchanged. No backend-only failure or missing mnemonic is observed. Native frame dereferences, special ECX-relative wrappers, `__const` engine pointers, implicit connectors/templates, and scratch XMM/flag entry contracts remain separate compatibility work. See the [audit report](flowstone-assembler-audit.md) and [manifest](flowstone-assembler-audit.json).
