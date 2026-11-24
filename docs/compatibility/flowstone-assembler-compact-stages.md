# Compact Assembler stages and output initialization

Assembler now accepts the engine spellings `stage0;`, `stage1;`, `stage2;` and `stage3;`, alongside `stage N;` and parenthesized blocks. A compact directive selects the existing stage for subsequent top-level instructions. It is not an instruction and adds no sample or buffer delay. Stage 0 can also initialize an Assembler output, independently of that output's single processing-stage writer.

## Recovered source evidence

Two Alpha 5483 de-zipper fragments previously stopped at `unsupported Assembler instruction 'stage0'`. Both seed `out0` from `p_target` in stage 0, then read and update that same output in stage 2. Supporting the directive alone would still reject their initialization store.

| Exact fixture | Executable file offset | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `engine-dezipper-compact-x86.asm` | x86 `0x4ddfd0` | 337 | `fc0139f23d1323a423d0eabce774d27dcca00c4cc52d8c46e6446b46bc340b67` |
| `engine-dezipper-compact-x64.asm` | x64 `0x57e0b0` | 391 | `f6ecaf927f28898f20776d8cde4423ef082b5cc9a5a91312b14ff73ada090e45` |

The executable hashes are x86 `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8` and x64 `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`. Fixture files are byte-for-byte substrings at these offsets, without source edits or line-ending conversion.

These fragments omit their caller's outlet declaration. Rendering tests prepend only `streamout out0;` and bind the original named target, positive/negative step and reset resources through the existing bounded engine-pointer API. No original stage, instruction or operand is rewritten.

The architecture difference is deliberately preserved: the x64 body tests and clears `p_reset`; the x86 body tests and clears the first DWORD of `p_target`. Both declare `p_reset`, but the x86 body does not use it. Tests model those exact bodies rather than silently substituting the x64 behavior into x86. This observation describes the recovered source; it is not a new Windows execution measurement.

## Execution and parsing contract

Stage-0 output seeds are recorded separately from the processing stage. They execute on the first nonempty render after construction/reset, and publish only initialized outlets before ordinary sample processing. Initialization-only outlets retain their seed. A late outlet exposes its seed on the first sample and its preceding stage-3 value thereafter; unseeded late outlets still start at zero. Float and Double slots preserve their declared representation, including Double-only low bits.

Packed Poly initialization publishes and commits only starting voices. Active peers retain their state when a new voice joins a packet, and recycled slots initialize afresh. Initialization does not turn a late output into a current-sample graph dependency, so controlled feedback keeps its one-sample recurrence.

Compact stage names are contextual: declared variables and connectors keep their names, and `stage0:` remains a local label. Section indexing distinguishes instruction starts from operands such as `call stage0;`; operand and skipped-instruction scans likewise keep label references distinct from directives. Stage boundaries still terminate numeric spans and local routine scans. Stage 0 cannot read or write signal inputs, registers/flags do not carry validity across stages, stacks must balance, and each output still has at most one writer among processing stages 1/2/3. DSP Code retains its existing stage syntax and stage-0 output restriction.

## Validation and audit

`assembler_compact_stages` runs both unchanged original bodies in the interpreter and LLVM ORC, comparing every lane/sample word against independent smoothing recurrences. It covers distinct lane targets and positive/negative steps, rising/falling transitions, target changes, reset consumption, scalar COMISS branch decisions, untouched control words, reset, empty calls and preserved caller floating-point state. Buffers are 1, 3, 7, 31, 64 and 257 samples, split at the same control-event frames so results are independent of buffer size.

Additional tests exercise all three stage-header forms; once-per-reset initialization; early, ordinary, late, initialization-only and unseeded outputs; four-lane Float/Double output seeds; `1 + 2^-40` precision; seeded feedback; staggered Poly voices across two packets, release/end and slot reuse; labels, calls, loops, numeric spans, skipped operands and contextual variable names. Negative tests retain cross-stage, initialization-input, malformed-header, multi-writer, stack and DSP Code restrictions. The existing stage-directive, stream, declaration, input-write and Poly regressions pass, as do portable interpreter checks without LLVM.

The strict compile audit preserves exactly **58 prepared entries**, **66 frontend-accepted entries**, **19/19 prepared serialized sources** and **eight explicit engine-binding requirements**, with no backend-only failures and zero missing mnemonic names. Only the two compact-stage fragments change their first diagnostic: they now reach their genuine missing caller outlet, `out0`. Integration tests keep that strict failure and separately verify frontend acceptance plus required engine bindings when only the caller outlet is declared. Contextual render tests are not counted as standalone corpus passes.

Earlier source and numerical fixtures remain unchanged. These tests use recovered source evidence and independent expected results; no new native FlowStone Windows numerical capture is claimed.
