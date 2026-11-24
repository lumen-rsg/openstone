# Backward-branch flag liveness

OpenStone now permits a loop backedge to invalidate incoming flags when the loop replaces those flags before any read on every resolved control-flow path. The check covers zero and signed comparison state, carry and parity independently. It applies to label and supported numeric backedges. No new flag values, opcode semantics or backend operations are introduced.

## Concrete audit gap

Two unchanged Alpha 5483 buffer-transfer fragments stopped at their second loop's backward jump with `flags are unavailable at backward branch target`. Their loop headers always execute `mov eax,[rcx];cmp eax,0;jz ...` before using any flags. The pointer increments later in the body clear OpenStone's abstract numeric-flag availability, since a portable memory cursor is not a native address. Those incoming flags are nevertheless dead: each next iteration's CMP defines the flags it consumes.

The compiler previously required the zero/signed group unconditionally whenever it had been available on the first visit to the header. It checked carry/parity liveness before unresolved forward branches had received their real destinations. The table loops therefore failed despite their explicit comparison at the header.

Flag liveness is now checked after all forward label/numeric destinations and stage exits are resolved, using the same control-flow traversal already used for changed GPR references. The check stops a path at a complete store to the relevant flag slot and rejects a load reached first. It does not relax the immediate checks for branches with missing producers, stack balance, source pointer provenance, local-call rules or other register state.

INC's carry preservation remains significant: defining zero/signed/parity does not define a needed incoming carry. A dedicated negative test keeps that case rejected. Pointer arithmetic's flag invalidation also remains intact; the compiler never derives x86 arithmetic flags from virtual addresses.

## Exact recovered sources

The inspected executable hashes are x86 `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8` and x64 `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`. Both fixtures below are exact 574-byte executable substrings. The sole source difference is the channel pointer stride: four bytes in x86, eight in x64.

| Fixture | Executable file offset | SHA-256 |
| --- | --- | --- |
| `engine-buffer-transfer-x86.asm` | x86 `0x4ba678` | `96a99c3b8507df429d99a281413acc9a2134585bac478380f782838f855fc42d` |
| `engine-buffer-transfer-x64.asm` | x64 `0x539950` | `fc7080d97b8f966a862064a9a1fa88912707d251f56c4a91710feda4deafa695` |

The original body copies four Float words between frame offsets and per-channel engine buffers, iterating over zero-terminated offset lists. It saves four registers and increments EBX by four; it is a fragment whose surrounding caller owns restoration, cursor writeback and visible readouts. Headless tests add those caller operations around the unchanged body and bind the [verified engine data tables](flowstone-assembler-engine-tables.md). No source instruction or branch is rewritten.

## Validation

`assembler_loop_flags` runs in both the interpreter and LLVM ORC. Original x86/x64 fragments are checked sample for sample and word for word against independently populated buffers at buffer sizes 1, 3, 7, 31, 64 and 257. Tests exercise zero, one and several input/output channels independently, exact four-lane packet routing, cursor advancement by four, byte-view neighbors, unchanged caller input buffers and preserved floating-point state. Raw packets include signed zeros, subnormals, infinities and payload/signaling NaNs; MOVAPS must preserve their bits.

Synthetic loops cover all twelve comparison branch spellings used by the suite: equality/inequality, signed order, unsigned order and parity. Counts and expected branch hits come from independent integer comparisons and low-byte parity. Tests include forward destinations, a numeric `jmp -25`, conditional `jnz` backedges and INC defining parity while preserving unused carry. Buffers 1, 3, 7, 31 and 64 verify repeatability across process boundaries and reset.

Negative cases reject a live incoming zero, signed, carry or parity value consumed before replacement, including a forward jump to that consumer. An INC case rejects live preserved carry. Missing/clobbered flag producers outside loops remain errors. The earlier stream, integer-flag, frame and engine-table suites pass too. The new loop, integer-flag, table and audit suites also pass with LLVM disabled and portable SoftFloat enabled.

## Audit result and limits

All **56 prepared entries** and **64 frontend-accepted entries** are retained, with eight explicit caller-binding requirements, no backend-only failures and no unlisted mnemonic names. Exactly the two transfer fragments change their first diagnostic: they now reach the genuine missing caller stack restoration. The strict audit leaves those fragments unwrapped and does not count their explicit-context render tests as standalone passes. The audit integration test checks both their unchanged diagnostic and frontend acceptance with only the missing caller cleanup/readout supplied.

This validation uses exact recovered sources and independent expected outputs. No new native FlowStone Windows capture was available. Other context gaps, native code dispatch and unsupported forms retain their existing diagnostics. Earlier fixtures and numerical captures are unchanged.
