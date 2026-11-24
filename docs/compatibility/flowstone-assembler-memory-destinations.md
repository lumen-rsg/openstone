# Integer memory destinations

The remaining operand-form audit exposed a frontend restriction: `add [eax],ebx` failed with `expected an integer register destination`, although the same bounded memory operand could be read or written with MOV. Assembler now accepts writable 32-bit memory destinations for **ADD, SUB, AND, OR, SHL and SHR**. Existing XOR, TEST, INC and DEC memory forms remain covered by the expanded tests.

## Operand and flag contract

ADD/SUB/AND/OR accept a 32-bit register or immediate source with a memory destination. SHL/SHR retain their byte-immediate count form and mask that count to five bits. Two-memory-operand operations stay rejected. Named destinations retain the existing 32-bit storage requirement; this pass does not introduce general 64-bit integers, Double-backed integer stores, CL-count DWORD shifts or new instruction names.

These are instruction-reference checks, not new FlowStone runtime captures. [Intel's ADD operand table](https://cdrdv2-public.intel.com/843847/253666-sdm-vol-2a-dec-24.pdf#page=134) lists both memory/register and memory/immediate forms. [The SHL/SHR reference](https://cdrdv2-public.intel.com/929354/253667-093-sdm-vol-2b.pdf#page=613) specifies masked counts, unchanged flags for count zero, and defined overflow only for one-bit shifts. Tests avoid asserting signed comparisons that depend on undefined overflow for larger counts.

The frontend now sends these memory destinations through its existing integer operations and bounded scalar store path. No interpreter/LLVM opcode, public API or project format changes. Existing carry/borrow, overflow, parity and zero handling is reused. Address registers and register sources stay unchanged, including when the same numeric register supplies both the source and destination byte offset.

A separate fix makes masked-zero memory shifts leave the object and its private routine-target provenance untouched. The compiler still validates the memory operand's read/write permissions and width, but discards the captured value into private scratch storage instead of writing it back. This preserves earlier flags and an existing callable local label. Actual arithmetic/logical writes continue to invalidate overwritten target tags, even when the resulting numeric bits happen to match.

## Memory boundaries

The existing local-packet, array, native-frame, Mem and explicit engine-pointer implementations keep their bounds and addressing rules. Incomplete out-of-bounds accesses retain the runtime's existing zero-read/ignored-write behavior. Read-only engine regions, external arrays, descriptors and constants remain protected. Numeric data cannot acquire pointer provenance. Stream-input destinations use refreshed private DSP working copies; they cannot mutate caller buffers or upstream fan-out.

## Headless tests

The existing `assembler_integer_flags` suite now includes:

- ADD/SUB register and immediate sources into named and indirect memory, plus AND/OR across register and memory forms. Independent unsigned arithmetic/bitwise models check every result and all sixteen existing conditional-branch spellings, including overflow, wraparound, both incoming carry states and all 256 low-byte parity patterns.
- SHL/SHR through local packets, arrays, Mem and engine bindings at counts 0, 1, 2, 7, 31, 32, 33, 63 and 255. Tests verify result words, defined flags, masked-zero preservation, unchanged address registers, owner guard bytes and reset across buffer sizes 1, 3, 7, 31, 64 and 257.
- Read-modify-write with the source register also indexing its destination. An independent byte-vector model checks each sample/readout and every backing byte after each buffer, including unaligned Mem views, partial tails, negative offsets and out-of-range offsets.
- Private indexed stream-input writes, unchanged neighboring lanes, per-sample refresh, caller/upstream isolation, read-only engine storage, zero-count local-target calls, actual-write provenance invalidation, and retained invalid-operand diagnostics.
- Local and native-frame mutations in stages 0/1/2/3, including reset, cross-buffer persistence and existing one-sample late-output visibility.

All run in the interpreter and LLVM ORC. The suite and compile-audit integration also pass with the interpreter-only portable SoftFloat configuration. Related engine-pointer/table, input-write and extra-register suites pass.

## Compile audit and limits

The strict corpus retains every prior result: **58 prepared entries**, **66 frontend-accepted**, **19/19 serialized sources prepared in both backends**, eight explicit engine-binding requirements, zero missing mnemonic names and no backend-only failures. No recovered source was patched or supplied synthetic caller context to change these totals. This pass expands the operand matrix; it does not turn an unrelated missing-caller-context diagnostic into a standalone pass.

The compile-audit integration adds positive preparation checks for the seven scalar memory-destination spellings and negative two-memory-operand checks. Existing numerical/source fixtures, native executables and the primitive inventory remain unchanged. Fresh native Windows numerical comparison remains pending.
