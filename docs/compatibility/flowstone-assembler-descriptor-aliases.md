# Numeric Mem fields through pointer-sized register aliases

Assembler now accepts `mov rax,mem[3]` and the equivalent RBX/RCX/RDX/RSI/RDI/RBP spellings when the source is an integer `memrefin` descriptor field. Fields 1, 2 and 3 supply byte length, Float-word count and channel count. The result defines the existing numeric DWORD parent, consumed through EAX/EBX/ECX/EDX/ESI/EDI/EBP. A previous pointer or local-routine target tag is cleared. MOV preserves the modeled comparison flags.

## Concrete native evidence

Three recovered Alpha 5483 sample readers start their channel dispatch with `mov rax,mem[3];cmp eax,1;`. FlowStone uses the pointer-sized spelling in both its x86 and x64 source strings. The compiler previously rejected the store with `use a 32-bit register for integer data`, before reaching the readers' actual signal ports or sample reads.

| Exact fixture | Bytes | Native file offset | SHA-256 |
| --- | ---: | --- | --- |
| `engine-mem-reader-channels.asm` | 903 | x86 `0x4ed6c8`; x64 `0x596990` | `0cb4dc6a8d785d83b56621a1339279ed5b5bd7f56a70e92fa5acbba695140868` |
| `engine-mem-reader-hop-x86.asm` | 979 | x86 `0x4edd38` | `656d4964077b6fc14e3262e4d27c249aecc8015ab14fb914ae6676c08d428f8d` |
| `engine-mem-reader-hop-x64.asm` | 992 | x64 `0x5972d0` | `518643728e6fa1152ffb9ea1cb10c4951f729949352744ee1aff701bdf751557` |

The executable hashes are x86 `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8` and x64 `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`. Fixture files preserve the complete original source bytes; none of their instructions, registers, branches or declarations are edited.

## Scope and representation

The new lowering applies only to a MOV into a pointer-sized register alias from the known integer Mem fields. It uses the same array-packet load and numeric GPR store as the existing DWORD form, with `writeGpr` clearing reference provenance. Both backends already implement those operations. Existing DWORD spellings retain their original lowering.

Field 0 still creates a bounded memory reference and keeps its separate pointer contract. Descriptor writes remain forbidden. An integer loaded from field 1/2/3 cannot be dereferenced or called as an old pointer/target. General numeric RAX operations, immediate/local-value wide MOVs, MemArray scalar-field aliases and unknown native encoding widths retain their previous diagnostics. This change does not add full 64-bit integer arithmetic or expose native addresses as sample bits.

The original alias pass tested each exact dispatch prefix, ending immediately before `mono:`, with explicit classification-only branch bodies. The subsequent [verified packed-limit implementation](flowstone-assembler-mem-limit.md) now also tests the complete sample-loading bodies with their omitted caller signal ports supplied. The original fixtures remain unchanged.

## Headless validation

`assembler_descriptor_aliases` runs in the interpreter and LLVM ORC. All seven aliases load all three integer fields under five independently specified incoming flag states. Every result word, DWORD-view conversion and carry/parity/zero/signed comparison outcome is checked. Empty bindings, one-/two-channel Mem, unaligned byte views, incomplete Float tails and byte lengths through 65537 exercise metadata updates between buffers without restarting the renderer. Owner bytes remain unchanged and caller floating-point state is preserved. Buffer sizes are 1, 3, 7, 31 and 64.

The three original dispatch prefixes are tested separately with absent, mono and stereo data, hop masks 0/1/3/15, global sample-clock expectations and buffers 1, 3, 7, 31, 64 and 257. Their exact CMP/JG/JE/JMP and native hop instructions remain intact. Tests cover calls, balanced stacks, replacing saved routine targets, loop-header pointer replacement, initialization/early/late metadata reads, reset and the existing DWORD numeric-skip width.

Negative cases reject stale pointer/target use, undeclared full-width numeric operations, descriptor writes, packed descriptor reads, field 4, missing flags, cross-stage GPR reads and unknown wide instruction widths. The audit integration test keeps each full unchanged reader's real missing `in1` diagnostic; supplying only the omitted ports now verifies complete frontend acceptance and the original hop-binding requirements. Related stream, extra-register and engine-table tests pass, as do portable interpreter checks without LLVM.

## Strict compile audit

Exactly these three sources advance from `use a 32-bit register for integer data` to `unknown DSP name 'in1'`. The strict corpus retains every earlier result otherwise: **58 prepared entries**, **66 frontend-accepted entries**, **19/19 serialized sources prepared**, **eight explicit engine-binding requirements**, zero missing mnemonic names and no backend-only failures. Prefix tests and supplied caller context do not count as complete standalone passes.

Earlier source fixtures and numerical captures remain byte-for-byte unchanged. These are recovered source and independent reference checks, not new native FlowStone Windows execution captures.
