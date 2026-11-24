# Assembler `__const` engine-pointer bindings

OpenStone accepts named `__const` declarations as caller-supplied, bounded data references. This supports original engine value readers and counters without exposing native host addresses. It is a data-binding API for primitive/import hosts; it adds no signal connectors or editor wiring controls.

## Verified native contract

The inspected Alpha 5483 executable hashes are x86 `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8` and x64 `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`. Evidence is static disassembly and exact recovered source bytes. No new native Windows output capture was available.

The lexer registers `__const` at x86 `0x617068` (handler `0x64d560`) and x64 `0x1402568f1` (handler `0x14028cea0`). The name-binding methods at x86 `0x5d88c0`–`0x5d893b` and x64 `0x14020cfe0`–`0x14020d06f` walk relocation records, compare their names with the supplied name, select a generated code buffer and patch site, add the recorded displacement to the supplied address, and store a DWORD or QWORD. In x64, `0x14020d037`–`0x14020d03e` handles the QWORD patch and `0x14020d044`–`0x14020d049` the DWORD patch. The x86 method similarly handles either patch width at `0x5d88f6`–`0x5d892a`.

Concrete callers establish that the primitive owner supplies each resource:

- Float value reader: x86 constructs `p_address` at `0x72a8b9`, supplies data address `0x9ee20c` at `0x72a8ca` and calls the binder at `0x72a8da`. x64 constructs the same name at `0x14038e189`, supplies address `0x140700104` in R8 at `0x14038e19c` and calls the binder at `0x14038e1ac`.
- Packed counter: x64 constructs `p_counter` at `0x14038f4e8`, loads its owner's buffer pointer from `[rbx+0xf8]` at `0x14038f4fb` and calls the binder at `0x14038f50b`.
- Another data owner supplies `p_value` from `[rbx+0xd8]` at `0x140393583`, then calls the binder at `0x140393596`.

Thus a name is not a universal engine address. `__const` fixes a source-level address binding; it does **not** make the pointed-to buffer read-only. The unchanged counter kernels store back through that address. Because the native binder visits relocation sites rather than every declaration, an unused declaration needs no address; OpenStone tracks references accordingly. This last requirement is inferred from the verified binder traversal.

## OpenStone contract

`DspProgram::enginePointers()` lists declared names; `requiresEnginePointer(index)` identifies referenced declarations. Comma-separated names are supported. Before compiling an active block, its host supplies each referenced resource through `Graph::bindDspEnginePointer(block, name, memory, writable)`. An explicitly empty `green::Memory` is a null binding. A missing binding is a diagnostic naming the resource. Unrendered blocks and unused declarations do not require bindings.

The runtime retains each Memory capability and its backing allocation, including unaligned byte views. Caller-owned data can be shared deliberately, while identical names in separate graphs remain independent. Writable is an explicit host policy, defaulting to true. Reads outside the view return zero; writes crossing its boundary or targeting a read-only view are ignored. Views are limited to signed 32-bit byte lengths, matching existing Mem references.

`Runtime::setDspEnginePointer` rebinds a named resource between buffers without compilation or stage-zero execution. PolyRuntime and PolyRenderer forward the operation to their instances. Binding/rebinding belongs off the render thread; render accesses allocate nothing. Whole-runtime reset reinitializes DSP state from the current binding but retains external resource data. The caller initializes or resets its own counters and buffers.

Supported forms include `[p]`, `[p+16]`, `[p+eax*4]`, and loading `p` into a supported 32/64-bit GPR alias before dereferencing it. Register copies, stack saves/restores and existing pointer offset operations preserve reference provenance. SIMD/x87 Float and Double operations use the same bytes in both backends. Comparing a bound reference with zero checks resource presence. A pointer remains opaque: assigning to `p`, treating it as scalar sample bits, or combining/scaling reference bases is rejected.

Memory operations use the existing bounded-memory helpers in the interpreter and LLVM ORC. Local routine targets stored through these resources retain target provenance. Overlapping writes through engine and ordinary Mem views invalidate that provenance, including byte writes that leave the numeric bits unchanged. This does not authorize calls to native engine machine-code addresses.

## Exact native fixtures

Every following file is an unchanged executable substring. Only the Float fragment lacks its caller's `streamout out0;` declaration; the headless test adds that declaration without editing its body. Existing sources and numerical captures remain unchanged.

| Fixture | Bytes | Executable file offset | SHA-256 |
| --- | ---: | --- | --- |
| `engine-bound-double-counter-x86.asm` | 256 | x86 `0x4dbef8` | `63df00a0e86dcd4f0e3041bde1a92a04f7bb1f355630fc77e6e5561261739272` |
| `engine-bound-double-counter.asm` | 250 | x64 `0x57a880` | `2f9d28e781b04bfeaf0085d4bbcaa673b0cf2d46dcfa7d23e0a68b76e6bc48a8` |
| `engine-bound-double-pair-x64.asm` | 256 | x64 `0x593870` | `6693c58132d64d139497d454798122ec12b71dd68ecc1ab0e6f3ce4c31eb9e9d` |
| `engine-bound-double-pair-x86.asm` | 230 | x86 `0x4eba38` | `a9dba3f74dd7936a12555c80a8531a734761dd374cb3269a124106b5cf20975e` |
| `engine-bound-double-read-x86.asm` | 109 | x86 `0x4d8868` | `720f7f719358e2e4a656b7a2605e854d9adaffab0f5506220f3142c4bb2df466` |
| `engine-bound-double-read.asm` | 122 | x64 `0x575500` | `15c8c44e548af39e6d5d1229a98e59359eb61e846700a06cc96e02b8a5989b78` |
| `engine-bound-float-read.asm` | 83 | x86 `0x4d8518` | `31a7e89ba01be2149ea260da2e86def7be1fed0c8b822059acb8c8704de2ca82` |

## Validation and remaining work

`assembler_engine_pointers` runs both the interpreter and LLVM ORC. It checks every sample of the unchanged Float reader, Double readers, independent Double pair and four-lane Double counters against independent literal/arithmetic expectations. Buffers 1, 3, 7, 31, 64 and 257 exercise boundaries where applicable. Counters and readers retain `2^-40` precision. Tests also cover multiple resource names, read-only policy, explicit null and missing bindings, byte/indexed accesses, view bounds and neighboring bytes, shared ownership after the Graph is destroyed, instance isolation, runtime reset/stage timing, next-buffer rebinding, packed Poly voices, numeric skips, target-alias invalidation, diagnostics and completion. The new suite also passes in the portable SoftFloat interpreter-only build.

The strict unchanged-source audit now accepts **62 frontend entries**, retaining all **56 entries prepared by both backends**. Six newly accepted native Double reader/counter sources report `engine-bindings-required` rather than receiving invented data. They render successfully in the explicit-context headless tests above. No prior prepared pass regressed and there are no unlisted mnemonic names. The manifest distinguishes frontend acceptance, missing context, and actual backend preparation.

Pointer cells and pointer-to-pointer data buffers now have [typed engine table bindings](flowstone-assembler-engine-tables.md). Native code/function tables remain separate work. The new data contract covers `p_buffers` and `p_sampleIndexAddress`; `p_codeMem` invocation and Poly ownership/dispatch adapters remain open. Reading their raw numeric bytes cannot silently manufacture a data reference or native call target; they need explicit typed contracts from the owning primitive/importer. Other missing implicit ports, wrapper/register state and engine allocation layouts remain visible in the audit. These tests establish portable behavior from verified static contracts and independent oracles, not new FlowStone runtime parity captures.
