# x87 access width and named memory

Assembler x87 loads and stores now use the instruction's width independently of the named operand's declared type. `DFLD`/`DFST`/`DFSTP` access eight raw bytes; `FLD`/`FST`/`FSTP` and `FILD`/`FIST`/`FISTP` access four. FLD interprets the bytes as binary32, DFLD as binary64 and FILD as a signed integer. The declaration supplies the underlying storage layout and constant-index stride, without an extra numeric conversion.

Previously the frontend rejected cross-type operands with `x87 operand has the wrong type`. Its direct store path also converted the result into the declaration's type. That cannot express a Double written into two Float/Int words or a Float written into one half of a Double.

## Recovered conversion fragments

These four exact Alpha 5483 executable source strings exercise the missing form:

| Fixture | Bytes | SHA-256 | x86 file offset | x64 file offset |
| --- | ---: | --- | --- | --- |
| `engine-float-to-double-move.asm` | 130 | `7fb21805ee412120fd895300cd9a0c1ee5c16a857a03b6132af0de426fcdd6d0` | `0x49fa20` | `0x506a50` |
| `engine-float-to-double-add.asm` | 128 | `c62c937eb97a8e634623a71175497866e2265ae62e99991ea3008b0d630ed827` | `0x49fcc0` | `0x506d10` |
| `engine-double-to-float-move.asm` | 111 | `a17c46a7a80676a8f00e106f7fe4908fe99297dae94f2c3554c67d860b62e6df` | `0x49fac8` | `0x506b00` |
| `engine-double-to-float-add.asm` | 110 | `ee059826129b7c44dd02481f2c5038141a1d44ebb514e1e9fdcc50c18554f19c` | `0x49fd78` | `0x506dd0` |

The widening fragments use `DFSTP tmp[0]`, `[2]`, `[4]`, `[6]`, then load two raw XMM packets from `tmp[0]` and `tmp[4]`. The narrowing fragments use `DFLD io[0]`, `[2]`, `[4]`, `[6]`, then write four Float lanes. Tests supply eight DWORDs of scratch storage, input setup, required initial XMM accumulators and raw-word readouts around the unchanged fragments. These caller buffers are explicit test context, rather than declarations inserted into the recovered source itself.

Native source-selector references independently locate the widening-move string at x86 `0x5d2c53` and x64 `0x14020644b`. Both executable files and all four source occurrences are verified by hash and exact bytes. This is static source/caller evidence, not a new native Windows numerical trace.

## Implementation and retained bounds

The frontend removes the declared-type equality check for ordinary x87 memory operands. All x87 stores now use the existing raw assembly-packet memory operation with a one-element write mask. Existing interpreter and LLVM ORC operations handle splitting/merging the four-byte pieces across declared cells, so no backend opcode, public runtime API or schematic format changes.

Both indexed and bare operands use this path. For example, a bare `FSTP` into a Double input working copy updates its low DWORD and retains its high DWORD and other lanes. A `DFSTP` into a Float or Int array writes two adjacent DWORDs. FST/DFST/FIST retain ST(0), while their P variants pop it as before. Stored routine-target provenance is cleared for every overwritten cell; disjoint targets stay callable.

Named memory retains its established bounds and four-byte alignment policy. A four-byte-aligned eight-byte access may straddle declared Double cells; a complete access that crosses the object's end reads zero or leaves memory unchanged. Negative/misaligned offsets cannot reach neighboring allocations. Indirect Mem/frame/engine-pointer accesses retain their existing byte-addressing implementation. Supporting other named-memory alignment forms remains separate work.

Read-only external arrays and descriptors remain protected. Register operands, x87 stack balance, pointer provenance, and initialization-stage signal-input restrictions retain their checks. Signal-input writes affect refreshed private working copies and cannot modify caller buffers or upstream fan-out.

## Headless validation

The new `assembler_x87_memory_views` suite executes in the interpreter and LLVM ORC across buffers **1, 3, 7, 31, 64 and 257**. It covers:

- All four complete unchanged conversion fragments, capturing raw result words for every sample and lane. Widening uses an independent IEEE bit-expansion model; narrowing uses literal expected words for ties, Double-only precision, normal/subnormal boundaries, overflow, infinities, signed zeros and signaling/quiet NaN payloads.
- Nearest-even results under three caller rounding modes, with caller rounding and sticky exception flags restored after each render. The move fragment retains x87 subnormal results; the add fragment additionally follows the existing SSE ADDPS denormal policy, tested separately from conversion.
- FST, DFST and FIST followed by loads through Float/Double/Int packets and arrays. An independent bounded byte-vector model checks half-Double writes, two-cell writes, four-byte-aligned overlapping windows, invalid offsets, atomic complete-access bounds, untouched words and neighboring guards.
- Bare mixed-width writes to Double signal inputs, per-sample refresh and upstream/caller isolation; overlapping/disjoint local-target provenance; protected external arrays/descriptors and stage-zero inputs.
- Widening scratch storage across five Poly voices/two packets, release, EndVoice, slot reuse and independent sample mixing.

The related stream-runtime, raw-XMM, input-write and compile-audit checks pass. The conversion, stream-runtime and audit suites also pass in an interpreter-only build using portable SoftFloat. No existing source fixture or numerical capture is modified.

## Compile audit

Every prior strict result is retained: **58 prepared entries**, **66 frontend-accepted entries**, **19/19 serialized sources prepared in both backends**, eight explicit engine-binding requirements, zero missing mnemonic names and no backend-only failures. The four raw fragments still omit their caller-owned `io` operand, so their strict diagnostics remain unchanged. New integration checks prepare each unchanged fragment with the required caller context and raw readout in both backends.
