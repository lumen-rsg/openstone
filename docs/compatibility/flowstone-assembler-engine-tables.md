# Engine data pointer cells and tables

OpenStone supports caller-supplied pointer cells and nested data-pointer tables behind `__const` names. The contract describes bounded regions and typed relationships separately from their raw backing bytes. No native address or machine-code entry point is imported from those bytes.

## Native evidence

The Alpha 5483 executable hashes remain x86 `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8` and x64 `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`. Evidence is static disassembly and exact executable source slices. No new Windows execution capture was available. The earlier [name-based relocation binder](flowstone-assembler-engine-pointers.md#verified-native-contract) remains the mechanism for supplying the root address.

### Channel buffers

The x86 primitive constructs `p_buffers` at `0x45baa0`, passes the address of its field `[esi+0xac]` at `0x45bab1`, and calls the binder at `0x45bac2`. Its x64 counterpart constructs the same name at `0x1400666d1`, passes the address of `[rbx+0x108]` at `0x1400666e4`, and binds at `0x1400666f4`. The x64 constructor zeroes this pointer field at `0x1400665a9`.

The unchanged consumer first loads a pointer from this supplied cell, then loads a per-channel buffer pointer from the resulting array. Its x86 source uses `mov rax,[p_buffers]` followed by `mov rdx,[rax+4*rcx]`; its x64 source uses `mov rax,p_buffers;mov rax,[rax]` followed by `mov rdx,[rax+8*rcx]`. Both finally read Float samples with `fld [rdx+4*rbx]`. Thus the supplied `__const` referent is a cell containing a table address, not a flat audio buffer. The field and entry widths follow the native source ABI, not OpenStone's host pointer size. FlowStone's pointer-sized RAX/RDX spelling appears in the x86 source too.

### Sample-index cells

For Float sample-index reads, x86 binds the address `0x9f2168` at `0x733e27`/`0x733e37`. x64 binds `0x140707710` at `0x140397a80`/`0x140397a90`. The x86 reset writes the fallback target address `0x9f2164` into that cell at `0x733f99`; x64 loads fallback address `0x140707708` at `0x140397bea` and stores it into the cell at `0x140397bf1`. Other native setters also replace the cell value. This independently verifies the pointer-to-data relationship and why its target can change.

The Double-output counterparts bind separate cells (`0x9f2170` and `0x140707720`). Their recovered code still uses **FLD**, reading a Float, before widening through DFST. Tests preserve this distinction instead of inventing Double source storage.

### Poly data versus code

The x64 Poly owner binds `p_dataMems` using the table pointer from `[rsi+0x2088]` (`0x1403c3ceb`–`0x1403c3cfe`), while `p_codeMem` is bound to the address of the separate field `[rsi+0x2090]` (`0x1403c3cb4`–`0x1403c3cc7`). The field is set from a generated code result at `0x1403c3c9a`. x86 has corresponding table and code fields at `0x1048` and `0x104c` (`0x75fdc6`–`0x75fdee` and `0x75fd7f`–`0x75fdb1`). The wrappers load per-process frames with a four/eight-byte table stride and call through the separate code pointer. This pass implements the data relationships; native code dispatch is still a separate contract.

## Binding API

`DspEnginePointerTable` owns a vector of `DspEnginePointerBinding` regions (Memory plus write permission) and a vector of `DspEnginePointerCell` descriptions. Region zero is the declared `__const` referent. A cell specifies its source region and byte offset, native width (4 or 8), optional target region, and target byte offset. `nullopt` is an explicit null target. Region references retain shared Memory backing, support byte views and aliases, and can describe cycles without recursive ownership.

For example, a native eight-byte cell referring to a Float buffer can be supplied as:

```cpp
DspEnginePointerTable table{
    {{Memory(std::vector<std::uint8_t>(8)), true}, {samples, false}},
    {{0, 0, 1, 0, 8}}
};
graph.bindDspEnginePointerTable(block.id, "p_sampleIndexAddress", table);
```

The table root can instead point to a second region containing channel cells, each referring to its own buffer region. The host chooses every region and relationship; names do not imply fabricated layouts or buffers. Missing active bindings remain errors. Existing direct `bindDspEnginePointer` is a one-region convenience and retains its behavior.

`Runtime::setDspEnginePointerTable` validates and replaces a binding between buffers. PolyRuntime and PolyRenderer forward this to their instances. Preparation and rebinding allocate off the render thread; reads/writes and pointer lookup allocate nothing. Runtime reset retains caller data and table relationships, just as it retains direct engine data. Rebinding does not rerun stage zero.

Pointer-sized MOV memory loads into RAX/RBX/RCX/RDX/RSI/RDI/RBP resolve a declared pointer cell and preserve opaque reference provenance. The same spelling handles four-byte x86 cells. Ordinary DWORD loads into EAX/etc retain integer semantics. Register copies and balanced stack saves preserve loaded references; comparisons with zero test nullness. Pointer values cannot be emitted as sample bits, stored as integers or called as native code. Legacy EDX-style pointer loads in the Poly wrappers remain part of the separate typed native-dispatch work; integer loads are not guessed to be addresses.

## Bounds, mutation and validation

Each instance has a private bounded virtual address layout. Current-referent checks prevent indexing out of one region into another region's cells or data. Null, undeclared, misaligned-to-cell or out-of-range pointer loads resolve to null; invalid data reads return zero and writes are ignored. Data writes honor the target region's permission. Host byte-view neighbors remain outside the reference bounds.

Pointer cells are typed overlays, not serialized virtual addresses. Numeric/SIMD reads overlapping their bytes yield zero, including through ordinary Mem aliases. An overlapping DSP write invalidates cell provenance, even if it writes the same bits. A later unannounced host-byte edit also cannot change its target: a pointer lookup checks the captured source bytes. An explicit validated rebind is required to establish an updated relationship. A previously loaded register reference remains a separate value.

Validation rejects overlapping source cells (including shared backing views), widths other than four/eight, cells crossing a region boundary, invalid target regions/offsets, and offsets on null targets. Regions plus padding must fit the signed 32-bit virtual address space. Invalid runtime updates leave the previous valid binding installed. This is OpenStone's bounded-memory policy, not a claim that native invalid pointer accesses are safe.

The unchanged buffer loop also required a compiler correction: backward-branch GPR liveness checks now run after forward branch targets are resolved. A register whose reference kind changes is accepted only if the old value is overwritten before any read on every resolved path. A negative regression case still rejects a live changed reference. No fixture instruction was rewritten to avoid this check. The related [flag-liveness correction](flowstone-assembler-loop-flags.md) now handles flags overwritten at loop headers and enables the recovered input/output transfer fragments in their caller contexts.

## Exact source fixtures

Every fixture below is an unchanged executable substring. The Float sample-index fragments receive only their missing `streamout out0;` declaration. The buffer fragments receive declared readout taps after the original body, reading its native frame writes. Their original code, labels and branches remain intact.

| Fixture | Bytes | Executable file offset | SHA-256 |
| --- | ---: | --- | --- |
| `engine-buffer-table-x64.asm` | 594 | x64 `0x4c2a70` | `eff414f2f3ee10796f89c8bf2155bb52760d23d19e48a34ffb61ce44da02c26f` |
| `engine-buffer-table-x86.asm` | 522 | x86 `0x470e98` | `134224f398d7fcb74dd4150069381c07a003b9aa1f2ef9dd01221d4919ea9e12` |
| `engine-sample-index-double-x64.asm` | 156 | x64 `0x58e8b0` | `a78ae3bec56432b7635c9d72eaa07519f83517586160771ed2edb71fe84fd20f` |
| `engine-sample-index-double-x86.asm` | 144 | x86 `0x4e8a60` | `ecf01d06d6b2e44af47608d2e51e4cd071708940976997d929417952b010661f` |
| `engine-sample-index-float-x64.asm` | 131 | x64 `0x58e240` | `13f25f1338468a41d483cdcca2079773124d9972f4fc2622310057b72d99ab11` |
| `engine-sample-index-float-x86.asm` | 119 | x86 `0x4e86d8` | `7f6202771d573f9e311590bd064393ef7222e2176f60f3aebdf405565748a49e` |

## Tests and audit

`assembler_engine_tables` checks both the interpreter and LLVM ORC. The six original Float/Double sample-index and channel-buffer sources are rendered against independent literal/sample-buffer expectations at buffers 1, 3, 7, 31, 64 and 257. The original buffer reader must advance its shared caller cursor exactly once per sample and hold its frame output after the last sample.

Further tests cover indexed nested cells, target addends, explicit null, invalid/negative indices, bounds across registered regions, child data writes and read-only policy, unaligned views and neighboring bytes, retained graph lifetime, finite traversal through aliased cyclic cells, null-terminated pointer-walking loops, all four Double lanes with `2^-40` precision, pointer-byte opacity, overlapping engine/Mem writes, external byte edits, invalid update recovery, live Poly rebinding, floating-point state preservation, and zero render allocations. The new table, earlier direct-pointer, frame and audit suites also pass with LLVM disabled and portable SoftFloat enabled.

The unchanged-source audit retains all **56 prepared entries** and now accepts **64 frontend entries**. Eight report `engine-bindings-required`; the two additional entries are precisely the unchanged x86/x64 Double sample-index kernels. Their explicit-context render tests pass, while the strict audit correctly supplies no invented table. Float fragments advance to their missing outlet declaration; buffer fragments advance to their missing output/readout contract. Poly wrappers advance to unsupported native code dispatch. The corpus still has 236 category/source entries and 231 source hashes, no missing mnemonic names, and no backend-only failure.

Native code/function-pointer invocation, arbitrary pointer-bit import and stores, legacy 32-bit register pointer inference, and automatic ownership adapters for imported primitives remain separate work. Static native-layout evidence and independent expected outputs establish this pass's contract; new FlowStone Windows numerical captures remain pending. Existing source and numerical fixtures are unchanged.
