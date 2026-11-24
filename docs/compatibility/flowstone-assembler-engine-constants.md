# Verified FlowStone engine constants

These bindings come from the constant factory in the unpacked **FlowStone Alpha 5483** executables, not from inferred symbol names or declarations added to the recovered kernels.

The x86 executable SHA-256 is `39c50f8b4973dca4202400a0951d7181ed0fe2a0ca21704e96ac895b0c8516e8`; x64 is `f4dbec8a34ba6fb7b8e91ed22038b69d081b94e275562c80d7ca9815a0b3832e`.

| Name | Raw binary64 value | x86 name comparison VA | x64 name comparison VA |
| --- | --- | --- | --- |
| `_DONE` | `3ff0000000000000` (+1) | `005d4af9` | `140208441` |
| `_DMONE` | `bff0000000000000` (−1) | `005d4b8e` | `1402084f3` |
| `_DABS` | `7fffffffffffffff` (clear-sign mask) | `005d4865` | `140208169` |
| `_DSIGN` | `8000000000000000` (sign-only mask) | `005d4919` | `140208228` |

For `_DONE`, x86 loads the repeated pair at VA `008a1930` (file `004a0330`), and x64 loads VA `1405083f0` (file `005073f0`). Both contain two little-endian `3ff0000000000000` QWORDs. `_DMONE` uses VA `00885a70` (file `00484470`) and VA `140508430` (file `00507430`), with two `bff0000000000000` QWORDs.

For `_DABS`, the x86 branch writes DWORDs `ffffffff`/`7fffffff` at `005d4878`/`005d487f`, then uses `movsd` and `unpcklpd` at `005d48a0`/`005d48a8`. The x64 branch loads `7fffffffffffffff` into RSI at `14020817d`, copies it with `movq` at `14020819b`, then duplicates it with `unpcklpd` at `1402081a0`.

For `_DSIGN`, x86 writes the zero EAX returned by the successful name comparison into the low DWORD at `005d492c` and `80000000` into the high DWORD at `005d492f`, then duplicates the value at `005d4950`/`005d4958`. x64 loads `8000000000000000` at `14020823c` and copies/duplicates it at `14020825a`/`14020825f`.

The x86 Double constructor (`006908e0`) receives four repeated QWORD arguments through two `movups` stores. Its x64 counterpart (`1402dc710`) receives the first Double in XMM3, the next at `[rsp+20]`, and the final pair at `[rsp+28]`. This establishes the four identical Double lanes as well as their exact bits. Float constructors and neighboring names are separate bindings; the first batch added only these four verified Double names.

These facts can be checked by mapping PE section raw offsets/RVAs and disassembling x86 VA `005d4865`–`005d4bc3` or x64 VA `14020810b`–`140208545` with `llvm-objdump --x86-asm-syntax=intel`. The original executables are not copied into the repository.

## Unchanged Double kernels

All bodies below are exact slices from the x64 executable. No constant declarations, input wrappers or synthetic register initialization were inserted.

| Fixture | File offset | Bytes | SHA-256 |
| --- | --- | --- | --- |
| `engine-double-floor.asm` | `0058d270` | 304 | `03bb4473d51e3e8a7ff35ff8afdf7166b5945a642dc9f3431bfa6679fe958330` |
| `engine-double-reciprocal.asm` | `005808f0` | 151 | `12dd6f9dbb3a5926fcafc273edd6b469dc22ffa351e48da4e6d99222708ead40` |
| `engine-double-ceiling.asm` | `0058d3b0` | 302 | `15fd5c5dca77de76e878ed0124934ab143a0fbb4c9902c9feb37bcf1b6b06fe4` |
| `engine-double-sign.asm` | `00580b50` | 184 | `7545fae03126fcf665b046e01b31bbf37449e2335a067d14458bd44ff92261b3` |
| `engine-double-absolute.asm` | `00580990` | 151 | `c8ee61f0dc613101dfa98cfe5cfae0082978c84e4f1b23f07ad1b47576190790` |

## Double behavior and validation

Assembler resolves these exact uppercase names as read-only Double-backed packets when there is no explicit declaration of the same name. Bindings are lazy, local to a compiled program, and initialized from raw bits on reset. Explicit variables retain their existing meaning and writability; this policy preserves OpenStone source compatibility. Unknown names remain errors. DSP Code's namespace is unchanged. The four names are also exposed to Assembler highlighting and completion.

The `assembler_engine_constants` suite renders all five original kernels sample for sample against independent raw/math oracles in the interpreter and LLVM ORC at buffer sizes 1, 3, 7, 31 and 64. It covers both physical Double windows, all four lanes, signed zeros, infinities, subnormals, NaN payloads for masks/sign, double-only precision differences, caller floating-point state, raw Float/Int/byte and x87 reads, stages, calls, numeric spans, per-program/reset isolation, shadowing, and rejected packed/scalar/byte/x87 writes. Reciprocal numerical checks avoid claiming an uncaptured NaN policy. Portable interpreter-only checks pass too.

The preserved Floor/Ceiling kernels first convert through signed DWORDs. Tests retain that algorithm's range limits, indefinite conversion results and NaN behavior; they do not replace it with an unrestricted mathematical floor/ceiling. The Sign kernel returns ±1 from the raw sign bit, including signed zeros and signed NaNs. These are static executable evidence plus reference-model numerical tests, not new native FlowStone execution traces.

The strict compile audit gains precisely these five category/source entries: **44 → 49**, with all earlier accepted entries retained and no missing mnemonic names. Incoming register state, other engine constants, `__const` pointer bindings and frame-dependent fragments remain unresolved.

## Verified Float bindings

The same x86 and x64 executables also agree on all nine Float constants in this factory. Each has **four identical binary32 lanes**, independent of the Double constant pool. These values are copied as raw bits, including the mask NaN; they are not evaluated from decimal literals or converted from binary64.

| Name | Raw binary32 word | x86 name comparison VA | x64 name comparison VA |
| --- | --- | --- | --- |
| `_PI` | `40490fdb` | `005d4604` | `140207ef5` |
| `_E` | `402df854` | `005d46b3` | `140207fa7` |
| `_SQRT2` | `3fb504f3` | `005d4762` | `140208059` |
| `_ABS` | `7fffffff` | `005d4811` | `14020810b` |
| `_SIGN` | `80000000` | `005d48c5` | `1402081ca` |
| `_DENORM` | `219392ef` | `005d4975` | `140208289` |
| `_ZERO` | `00000000` | `005d4a24` | `14020833b` |
| `_ONE` | `3f800000` | `005d4a9a` | `1402083e4` |
| `_MONE` | `bf800000` | `005d4b32` | `140208496` |

For x86, the `_PI`, `_E`, `_SQRT2`, `_DENORM`, `_ONE` and `_MONE` branches each pass four identical immediate DWORDs on the stack. The first payload instructions are `005d4636`, `005d46e5`, `005d4794`, `005d49a7`, `005d4acc` and `005d4b64`, respectively. `_ABS` and `_SIGN` load the raw DWORDs at `005d4824` and `005d48d8`, then use MOVSS/SHUFPS to duplicate them. `_ZERO` jumps from `005d4a49` to the shared zero constructor path `005d4bd7`, with four zero DWORD stores starting at `005d4be0`. The Float constructor `0068f630` copies the four stack arguments to object offsets `24`, `28`, `2c`, `30` (hex), preserving their bits.

For x64, the scalar payloads are at these data VAs/file offsets:

| Name | Data VA | File offset |
| --- | --- | --- |
| `_PI` | `1405083c0` | `005073c0` |
| `_E` | `1405083bc` | `005073bc` |
| `_SQRT2` | `1405083b8` | `005073b8` |
| `_DENORM` | `1405083b4` | `005073b4` |
| `_ONE` | `1404ba608` | `004b9608` |
| `_MONE` | `1404bd570` | `004bc570` |

`_ABS` and `_SIGN` instead use immediate `7fffffff` and `80000000` at `14020811f` and `1402081de`, copied with MOVD to XMM3. `_ZERO` uses XORPS at `140208363`. Each branch supplies XMM3 plus three identical scalar stack arguments at `[rsp+20]`, `[rsp+28]`, `[rsp+30]`. Constructor `1402db0c0` stores the four Float words at object offsets `48`, `4c`, `50`, `54` (hex). This independently confirms the lane width and repeated bits in both architectures. PE section mapping and disassembly of x86 `005d4604`–`005d4c0a` and x64 `140207ef5`–`1402084ee`, plus those constructors, reproduce the checks. Package SHA-256 values are pinned above.

### Unchanged Float kernel fixtures

The following exact bodies occur in **both** executables. Their fixture bytes have no added declarations or rewritten instructions.

| Fixture | x86 file offset | x64 file offset | Bytes | SHA-256 |
| --- | --- | --- | --- | --- |
| `engine-float-ceiling.asm` | `004e78a8` | `0058cb50` | 121 | `13b9bce06d5e7a6e32dbf840bf21a1b63428e5999ad26f3a53446cabd4ea03a9` |
| `engine-float-floor.asm` | `004e7828` | `0058cad0` | 122 | `2dc5aa95d3150a49dd766a3680e599d863fa716fa03c9d48039c9c5e560f2936` |
| `engine-float-sign.asm` | `004df210` | `0057feb0` | 65 | `84e36ec3da5afb4a41aa0baec49a6f08f9003859f29f7de4ca17e4ad9cdcd763` |
| `engine-float-absolute.asm` | `004df184` | `0057fe10` | 49 | `bfd9e432387f61887f73fda8ea230ad240e85faaa847d5e92b98d761b3316f18` |

These are internal engine fragments: they use `in0` and `out0` without declaring their ports. Rendering tests supply the explicit caller context `streamin in0;streamout out0;` before the original body. They do **not** declare constants, add synthetic register values, or alter fixture bytes. The strict audit still compiles the original source alone and honestly reports the missing `in0`; this batch does not turn those fragments into standalone audit passes.

### Float behavior and validation

The same lazy, read-only, exact-uppercase fallback policy applies, with a typed Float packet instead of a Double packet. Explicit declarations still shadow the bindings and remain writable; the DSP Code namespace is unchanged. All nine names are available to Assembler highlighting and completion. Reading a Float constant with MOVAPD views the original four DWORDs as two raw QWORDs rather than numerically converting them.

The extended `assembler_engine_constants` suite compares every sample word of all four recovered Float kernels against independent bit/math oracles over four distinct lanes, buffer sizes **1, 3, 7, 31, 64**, and both interpreter and LLVM ORC. It covers finite fractions, signed limits and conversion-invalid results, signed zeros, infinities, quiet/signaling NaNs and subnormals; the original Floor/Ceiling DWORD conversion algorithm and CMPGTPS unordered behavior are retained. Caller rounding and exception state is restored. Every Float binding also has raw PS/PD, DWORD and all four byte-position checks, local calls, stage-3 latency and reset, numeric branch spans, local-copy isolation, shadowing, read-only packed/scalar/byte/x87 diagnostics, and highlighting/namespace checks. Mixed Float/Double reads and the earlier Double kernel regressions pass. The portable interpreter-only build passes the complete extended suite too.

The strict compile audit retains **all 49 accepted category/source entries**, with zero missing mnemonic names. Existing sources and captures remain unchanged. Remaining Double constants and pointer/register/frame bindings are separate work. These results verify constructor bits and OpenStone rendering against reference models; they are not newly recorded native FlowStone execution traces.
