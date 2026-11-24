# FlowStone Assembler stage directives

The compile audit identified two engine kernels using `stage N;` rather than the public DSP-style `stage(N) { ... }` blocks. OpenStone now accepts these Assembler directives for stages 0, 1, 2 and 3. A directive selects the stage for subsequent top-level instructions until another stage header. Code before any header remains in stage 2. Parenthesized blocks keep their existing OpenStone scope and return to stage 2 after the closing brace, including when mixed with directives. DSP Code continues to use parenthesized blocks.

Compact `stageN;` spellings and Assembler stage-0 output seeds are also supported; see [the additional source evidence and contract](flowstone-assembler-compact-stages.md).

Directives select the existing execution stages, rather than inserting instructions or a buffer delay. Stage 0 initializes once after construction/reset; stage 1 runs before ordinary sample processing; stage 2 is the default; stage 3 updates state after the completed sample. A stage-3 output retains the existing one-sample visibility rule. Locals persist across stages, while register/flag validity, branches, numeric spans, calls and stacks retain their existing stage/section boundaries. `stage` can still name a variable, connector or local label when used in those contexts.

## Unchanged executable evidence

Both fixture bodies occur byte-for-byte in **both FlowStone Alpha 5483 executables**. x86 SHA-256: `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8`; x64 SHA-256: `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`.

| Fixture | x86 file offset | x64 file offset | Bytes | SHA-256 |
| --- | --- | --- | --- | --- |
| `assembler-stage-delay1-float.asm` | `004dcb18` | `0057bda0` | 96 | `0c20ee459f935f133f1d6018c9cb39f47de803d635c24f8bd88074f5922efb81` |
| `assembler-stage-delay1-double.asm` | `004dce68` | `0057c3c0` | 228 | `9b3aebf8973b09063219be6c9970a95319ea2bbc7244f09e240e79aa30ecb4c7` |

The Float body writes the current input to `prev` in `stage 3;` and reads `prev` to the output in `stage 1;`. It omits its caller's ports, so its rendering test separately supplies only `streamin in0;streamout out0;`. The strict unchanged-source audit still reports its original missing `in0` after correctly parsing the first directive. No fixture instructions or declarations were rewritten.

The Double body already declares `streamdoublein in0;streamdoubleout out0;` and uses two MOVAPD windows (`[0]` and `[2]`) to transfer all four lanes. It now compiles standalone in the interpreter and LLVM ORC with no injected caller context. Neither kernel receives fabricated register initialization or engine values.

## Validation

The `assembler_stage_directives` suite renders both original bodies sample for sample against an independent one-sample-delay oracle, using scalar Mono/MonoDouble and packed Mono4/MonoDouble4. Raw tests include signed zeros, subnormals, infinities, quiet/signaling NaN payloads and the Double-only `1 + 2^-40` precision bit, checking every lane and sample word. Buffer sizes are **1, 3, 7, 31, 64 and 257**, plus a changing sequence containing zero-frame calls. Reset restores initial delay state, and rendering preserves the caller's rounding and exception environment.

Separate controlled-feedback tests use staggered, signed impulses in all four lanes and an independent 0.5-feedback recurrence. They compare capturing the sum, the delay, and both in either order, across every fixed buffer size. This checks graph dependency metadata and global stage scheduling without introducing extra latency. The Float decay stays in the normal range, and Double feedback retains its extra precision.

Additional tests cover source-order-independent stage execution, one-time initialization, held stage-3 outputs, directive persistence, mixed blocks, hexadecimal stage numbers, line comments, `stage` variables/labels, repeated helper names in separate sections, direct/indirect local calls, counted loops, numeric skips, and stage-ending RET. Diagnostics cover invalid numbers, missing semicolons, nested directives, stage-0 signal input access, outputs written in multiple processing stages, invalid register/flag reads, cross-stage jumps/calls/numeric displacements, and unbalanced integer/x87 stacks. The prior declaration grammar regression suite remains unchanged. Both suites also pass with LLVM disabled and portable SoftFloat enabled.

The audit integration test compiles the unchanged self-contained Double fixture in both available backends. The strict full audit accepts **49 → 50 category/source entries**, adding exactly `kernel-string/9b3aebf8973b09063219be6c9970a95319ea2bbc7244f09e240e79aa30ecb4c7`. Every earlier pass is retained, the Float fragment remains an honest missing-caller-port failure, and the missing-mnemonic list is empty. Existing fixtures and captures are unchanged. These are recovered native source evidence and OpenStone/reference-oracle comparisons; no new native FlowStone execution trace is claimed.
