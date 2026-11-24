# FlowStone Assembler compile audit

Compile-only audit of unchanged recovered source. No samples were rendered; passing compilation does not establish numerical parity.

## Coverage

| Source category | Distinct sources | Instances | Frontend pass | Interpreter prepared | LLVM ORC compiled |
|---|---:|---:|---:|---:|---:|
| asm-snippet | 1 | 1 | 1 | 1 | 1 |
| embedded-record | 19 | 68 | 19 | 19 | 19 |
| kernel-string | 205 | 300 | 36 | 28 | 28 |
| module-record | 11 | 36 | 11 | 11 | 11 |

Scanned 145 module files, the Assembler snippets, and the executables listed in the JSON manifest. Exact source bytes are deduplicated within each category. Identical x86/x64 strings share one entry. No implicit ports, constants, caller-register prologues, or instruction rewrites were injected into the recovered sources. Compiler-owned entry bindings apply, including OpenStone's zero fallback for unbound incoming XMM registers. This fallback is a portable policy, not a verified native scratch-register value.

Module records are recognized by their stored string length and inlet/outlet counts. Embedded executable records also accept a zero/zero connector header when the exact length and declaration boundaries match; unused component copies have these headers. Kernel strings are null-terminated executable strings that look like assembly; some are fragments, format templates or require FlowStone engine bindings. This is a recovered plaintext corpus, not a full schematic/resource decoder or an exhaustive instruction/operand matrix.

## First diagnostic groups

The compiler stops at the first error. The independent mnemonic census scans the whole recovered source; unlisted names inside engine fragments are leads, not proof that a standalone Assembler block requires them.

| Category | Sources |
|---|---:|
| control-flow | 12 |
| engine-bindings-required | 8 |
| frontend-other | 20 |
| incoming-register-state | 7 |
| pass | 59 |
| unbound-name-or-engine-context | 130 |

## Unlisted mnemonic census

| Name | Distinct source hashes | First location |
|---|---:|---|
| None | 0 | — |

## Failed self-contained records

Kernel strings are excluded from this table; all diagnostics and locations remain in CSV/JSON.

| Category | SHA-256 prefix | First location | First diagnostic |
|---|---|---|---|

## Reproduce

Build `openstone_assembler_probe` with `OPENSTONE_BUILD_ASSEMBLER_AUDIT=ON` and `OPENSTONE_ENABLE_LLVM_ORC=ON`, then:

```sh
python3 tools/audit_flowstone_assembler.py --flowstone-root /path/to/unpacked/FlowStone \
  --probe /path/to/build/openstone_assembler_probe \
  --output /path/to/report/flowstone-assembler-audit
```

Use `--extract-dir /path/to/local/corpus` to retain the unchanged extracted sources. Every source and containing file has a SHA-256 and every occurrence has a byte offset. The JSON also hashes the probe and compiler sources to identify the exact audited implementation. Backend statuses `unavailable` and `requires-bindings` are never counted as passes. The latter means the source compiled but requires explicit caller-owned `__const` resources; the probe does not invent null/default bindings. Sources are compiled in separate processes with a timeout; stage 0 and rendering are never executed.
