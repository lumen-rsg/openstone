# Additional FlowStone Assembler registers

The audit exposed two frame-load fragments using `[rbp+rax]` and a serialized Mem byte writer using `edi`. OpenStone previously recognized only the EAX–EDX families. It now also recognizes **ESI, EDI and EBP** as independent 32-bit numeric registers, with **RSI, RDI and RBP** as their pointer/local-target aliases, following the existing RAX–RDX model. Names are case-insensitive and appear in Assembler highlighting/completion. They are reserved register names in Assembler declarations; DSP Code's namespace is unchanged.

All seven modeled register families use independent program-owned slots, assignment tracking, pointer/target provenance and per-stage state. Arithmetic, logic, comparisons, flags, memory indexing, stack saves/restores, and local calls reuse the existing instruction lowerings in both backends. The six added names do not create full 64-bit integer arithmetic, automatic native frame bindings, or implicit incoming values. ESP/RSP and additional byte/word aliases remain separate work.

The eight legacy byte aliases still map only to EAX/EBX/ECX/EDX. Their parent count is now explicit and independent of the number of modeled GPRs: adding ESI/EDI/EBP must never make AH/BH/CH/DH address those new slots.

## Native source evidence

The Alpha 5483 x86 executable SHA-256 is `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8`; x64 is `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`.

Both binaries contain the same register-name sequence `eax, ecx, edx, ebx, esp, ebp, esi, edi, rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi`, stored as NUL-terminated names. The added names occur at these exact file offsets:

| Name | x86 file offset | x64 file offset |
| --- | --- | --- |
| `esi` | `0049ef34` | `00505e4c` |
| `edi` | `0049ef38` | `00505e50` |
| `ebp` | `0049ef30` | `00505e48` |
| `rsi` | `0049ef54` | `00505e6c` |
| `rdi` | `0049ef58` | `00505e70` |
| `rbp` | `0049ef50` | `00505e68` |

All three recovered bodies below occur unchanged in both binaries. The table gives the first occurrence in each architecture; the byte writer also has a second serialized copy in each executable.

| Fixture | x86 file offset | x64 file offset | Bytes | SHA-256 |
| --- | --- | --- | --- | --- |
| `assembler-mem-byte-writer.asm` | `0066190f` | `007ab90f` | 1530 | `2ec4728ea71438c3b180133a4cd0bf93223aac851e05cf37b14bb83912c8b7f8` |
| `engine-frame-load-float.asm` | `0049f9ac` | `005069d8` | 56 | `212c1294f742bac39a123ab962278ef7032e88b1e12d8acda4ba93d30c354f7f` |
| `engine-frame-load-double.asm` | `0049f868` | `00506880` | 115 | `a9d330a1c9b08a6633ffb4362eb450f399df1ede499e5133fb78f310b49c8b3a` |

### Explicit caller context

The original byte writer saves incoming ECX/EAX/EBX/EDX/EDI before using them. Its test supplies seven distinct nonzero numeric caller values separately, runs the original body, and appends readback taps. The original save/restore instructions and every fixture byte remain intact. The test checks that all caller registers survive, every reported byte is correct, and only the selected byte in shared Mem changes. Indices stay within complete writable DWORDs, so this does not depend on native out-of-bounds memory behavior.

The frame-load fragments omit declarations and enter with RBP/RAX supplied by the engine. Rendering tests explicitly bind RBP to bounded Mem storage, set EAX to a moving byte cursor, and append Float/Double output taps around the unchanged body. They compare all raw copies and both Double windows to a byte-wise reference reader, including bounded tail reads. This establishes the operand forms under a declared caller contract; it does not reconstruct FlowStone's native frame layout or ABI.

The strict audit receives none of these wrappers. The byte writer keeps its incoming-register failure. Both RBP load fragments now pass address parsing and report the missing incoming register, while three format-template fragments advance to their unresolved `%d` substitutions. No arbitrary values are injected to increase standalone acceptance.

## Tests and audit

`assembler_extra_gprs` checks ESI/EDI/EBP arithmetic and logic with independent 32-bit result/flag oracles, all 16 supported branch spellings, signed overflow/wraparound, both incoming carry states, and preservation of all other GPRs. The matrix covers ADD/SUB/CMP/INC/DEC/AND/OR/XOR/TEST, raw outputs and caller floating-point state. Recovered Float/Double frame loads and the EDI writer render sample for sample in the interpreter and LLVM ORC at buffer sizes **1, 3, 7, 31 and 64**, with reset between runs. Additional cases cover opaque pointer copies, stack preservation, local target aliases, direct/register tail transfers, counted loops, numeric skips, XCHG, legacy byte parents, stage isolation, invalidation of old pointers, register-width diagnostics, reserved names and completion. Caller values are explicit test inputs; the compiler does not synthesize incoming state.

The new suite and the existing legacy byte-register suite also pass in an interpreter-only build with portable SoftFloat enabled. The full audit retains the **exact same 50 accepted category/source entries**, with no backend-only failures or missing mnemonic names. Two diagnostics move from unsupported address forms to missing incoming state; original source/capture fixtures and package hashes are unchanged. These are native source/name evidence and independent reference-model tests, not new native FlowStone execution traces.
